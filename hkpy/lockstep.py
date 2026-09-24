"""Per-frame lockstep of the sim against a full-state recording (.hkstate, docs/state-record.md); docs/lockstep.md.

For every recorded frame f: overwrite the sim's state with the game's frame f (import), step exactly one frame of the
kind the recording shows with the recorded action, read the sim's state back (export) and compare it field by field
with the game's frame f+1, floats by bit pattern.  Every divergence is therefore one step's: one frame, one field.

    python -m hkpy.lockstep <rec.hkstate> [--corpus <corpus.json>] [--out report.json] [--frames N]
    python -m hkpy.lockstep --merge <report.json>...

The sim side is sim/core/lockstep_api.c: a table of entries (owner GameObject, component label, recorded field name,
value type) that this module pairs with recorded (entity, field) slots through the GameObjects' identity (path at
birth and clone index).  Entries with no recorded counterpart are not compared; the report counts them.
"""
import argparse
import collections
import ctypes
import json
import os
import re
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
if os.path.dirname(HERE) not in sys.path:
    sys.path.insert(0, os.path.dirname(HERE))
from hkpy import sim_config, sim_driver, staterec  # noqa: E402

FRAME_STEP, FRAME_LIVE, FRAME_LIVE_LAST, FRAME_FROZEN = 0, 1, 2, 3   # sim/core/lockstep.h
KIND_NAMES = {FRAME_STEP: "step", FRAME_LIVE: "live", FRAME_LIVE_LAST: "live_last", FRAME_FROZEN: "frozen"}
UNKNOWN = -2          # a name, string or object the sim does not have (never imported)
NONE = -(1 << 63)     # the sim has no such value now (a body's fields without a body): not compared
RO = 1                # export-only entry (sim/core/lockstep_api.c F_RO)
COL_TYPES = ("BoxCollider2D", "CircleCollider2D", "PolygonCollider2D", "EdgeCollider2D", "CapsuleCollider2D")   # fsm_tables.h COL_*
TOUCH = {0: "touching", 1: "no shape", 2: "no contact", 3: "no overlap"}   # hkls_touch results
UI_ROOTS = ("_GameCameras",)   # the cameras and the HUD: outside the gameplay scope, counted under their own [ui] kinds


def load_lib(dll=None):
    lib = sim_driver.load(dll or sim_driver.DLL)
    c = ctypes
    lib.hksim_frame.argtypes = [c.c_void_p, c.c_int, c.POINTER(c.c_int32)]
    lib.hksim_frame.restype = c.c_int
    lib.hksim_fsm_world.argtypes = [c.c_void_p]; lib.hksim_fsm_world.restype = c.c_void_p
    lib.hkfsm_go_count.argtypes = [c.c_void_p]; lib.hkfsm_go_count.restype = c.c_int32
    lib.hkfsm_go_path.argtypes = [c.c_void_p, c.c_int32]; lib.hkfsm_go_path.restype = c.c_char_p
    lib.hkfsm_go_info.argtypes = [c.c_void_p, c.c_int32, c.POINTER(c.c_int32)]
    lib.hkls_open.argtypes = [c.c_void_p]; lib.hkls_open.restype = c.c_void_p
    lib.hkls_close.argtypes = [c.c_void_p]
    lib.hkls_error.argtypes = [c.c_void_p]; lib.hkls_error.restype = c.c_char_p
    lib.hkls_refresh.argtypes = [c.c_void_p]; lib.hkls_refresh.restype = c.c_int32
    lib.hkls_entry.argtypes = [c.c_void_p, c.c_int32, c.POINTER(c.c_int32), c.POINTER(c.c_char_p), c.POINTER(c.c_char_p)]
    lib.hkls_export.argtypes = [c.c_void_p, c.c_void_p, c.c_int32]; lib.hkls_export.restype = c.c_int
    lib.hkls_struct.argtypes = [c.c_void_p, c.c_int32]; lib.hkls_struct.restype = c.c_char_p
    lib.hkls_value_of.argtypes = [c.c_void_p, c.c_int32, c.c_char_p, c.c_int32, c.POINTER(c.c_int64)]
    lib.hkls_value_of.restype = c.c_int
    lib.hkls_name_of.argtypes = [c.c_void_p, c.c_int32, c.c_int64]; lib.hkls_name_of.restype = c.c_char_p
    lib.hkls_import.argtypes = [c.c_void_p, c.c_void_p, c.c_void_p, c.c_int32, c.POINTER(c.c_int32)]
    lib.hkls_import.restype = c.c_int
    lib.hkls_list.argtypes = [c.c_void_p, c.c_int32, c.POINTER(c.c_int32), c.c_int32]; lib.hkls_list.restype = c.c_int32
    lib.hkls_import_list.argtypes = [c.c_void_p, c.c_int32, c.POINTER(c.c_int32), c.c_int32]
    lib.hkls_import_list.restype = c.c_int
    lib.hkls_live_add.argtypes = [c.c_void_p, c.c_int32, c.c_int32, c.c_int32, c.c_char_p, c.c_float]
    lib.hkls_live_add.restype = c.c_int
    lib.hkls_enter_state.argtypes = [c.c_void_p, c.c_int32, c.c_int32]; lib.hkls_enter_state.restype = c.c_int
    lib.hkls_update_pairs.argtypes = [c.c_void_p]; lib.hkls_update_pairs.restype = c.c_int
    lib.hkls_touch.argtypes = [c.c_void_p] + [c.c_int32] * 7; lib.hkls_touch.restype = c.c_int
    return lib


def f32(bits):
    return struct.unpack("<f", struct.pack("<I", int(bits) & 0xFFFFFFFF))[0]


def hash_list(vals):
    """sim/core/lockstep_api.c hash_list: FNV-1a 64 over the count and the elements as little-endian 32-bit words."""
    h = 0xcbf29ce484222325
    for x in [len(vals)] + list(vals):
        x &= 0xFFFFFFFF
        for j in range(4):
            h ^= (x >> (8 * j)) & 0xFF
            h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h >> 2


def raw_of(e, fi):
    """A recorded entity's raw value of field index fi (a field never put is zero / empty)."""
    return e.vals[fi] if fi < len(e.vals) else staterec._default(e.cls.types[fi])


def field_kind(comp, field):
    """The field kind a divergence counts under: the component class and the field, object names stripped."""
    if comp.startswith("FSM:"):
        cls = "FsmStateAction" if "/A" in comp else "FsmState" if "/S" in comp else "FsmVariables" if comp.endswith("/V") \
            else "PlayMakerFSM"
    elif comp == "PlayMakerGlobals/V":
        cls = "FsmVariables(globals)"
    else:
        cls = re.sub(r"#\d+$", "", comp)
    if cls.startswith("FsmVariables"):
        field = field.split(":", 1)[0] + ":*" + ("." + field.rsplit(".", 1)[1] if re.search(r"\.[xyzwrgba]$", field) else "")
    return cls + "." + field


class Recording:
    """The recorded side: frames applied one at a time, with the lookups the pairing needs."""

    def __init__(self, path):
        self.path = path
        self.rec = staterec.StateRecord(path)
        self.frames = self.rec.frames()
        self.fr = None
        self.go_by_key = {}          # (path at birth, cloneIndex) -> GameObject eid

    def next(self):
        self.fr = next(self.frames, None)
        if self.fr is None:
            return None
        r = self.rec
        for e in self.fr.died:
            if e.cls.name == "GameObject":
                k = (e.key, e.raw("cloneIndex"))
                if self.go_by_key.get(k) == e.eid:
                    del self.go_by_key[k]
        for eid in self.fr.born:
            e = r.entities.get(eid)
            if e is not None and e.cls.name == "GameObject":
                self.go_by_key[(e.key, e.raw("cloneIndex"))] = eid
        return self.fr

    def first(self, cls):
        d = self.rec.by_class.get(cls)
        return next(iter(d.values())) if d else None

    def go_of(self, e):
        """The GameObject entity an entity belongs to (walking up the parents), or None."""
        seen = 0
        while e is not None and e.cls.name != "GameObject" and seen < 8:
            e = self.rec.entities.get(e.parent)
            seen += 1
        return e if e is not None and e.cls.name == "GameObject" else None


class Lockstep:
    def __init__(self, lib, rec_path, actions=None, max_first=12, log=print):
        self.lib, self.log, self.max_first = lib, log, max_first
        self.R = Recording(rec_path)
        fr0 = self.R.next()
        if fr0 is None:
            raise SystemExit("%s: no frames" % rec_path)
        h = self.R.rec.header
        self.level = h.get("level")
        self.fpw = int(h.get("frames_per_wait") or 1)
        self.seed = int(h.get("seed") or 0)
        self.actions = actions
        t = self.R.first("Time")
        cfg = sim_driver.Config(self.level.encode(), self.fpw, self.seed, 0)
        cfg.scene_ready_frame = fr0.frame
        cfg.fixed_count0 = fr0.fixed_count
        cfg.time0 = t["time"] if t is not None else 0.0
        cfg.time_since_level_load0 = t["timeSinceLevelLoad"] if t is not None else 0.0
        self.sim = lib.hksim_create(ctypes.byref(cfg))
        if not self.sim:
            raise SystemExit("hksim_create: %s" % lib.hksim_last_error(None).decode())
        if lib.hksim_reset(self.sim, self.seed) != 0:
            raise SystemExit("hksim_reset: %s" % lib.hksim_last_error(self.sim).decode())
        sim_config.apply(lib, self.sim)
        self.world = lib.hksim_fsm_world(self.sim)
        self.ls = lib.hkls_open(self.sim)
        if not self.ls:
            raise SystemExit("hkls_open failed")
        self.cp = lib.hksim_checkpoint_new(self.sim)
        # the entry table
        self.n = 0
        self.go = np.zeros(0, np.int32); self.typ = np.zeros(0, np.uint8); self.flags = np.zeros(0, np.uint8)
        self.comp, self.field = [], []
        self.by_go = collections.defaultdict(list)
        self.sim_go_key = []          # sim GameObject index -> (path, occurrence) or None (a prefab asset)
        self.sim_path_count = collections.Counter()
        # pairing
        self.slot = []                # entry -> (eid, field index, conv) or None
        self.rec_val = np.zeros(0, np.int64)
        self.mapped = np.zeros(0, bool)
        self.watch = collections.defaultdict(list)   # eid -> [entries paired with it]
        self.pending_field = collections.defaultdict(set)   # class id -> entries waiting for a field
        self.cls_nf = {}
        self.cache_value = {}
        self.dynamic = []             # entries recomputed every frame (proxies, structures)
        self._grow()
        self.go_rec = {}              # sim go -> recorded GameObject eid
        self.iid_go = {}              # recorded GameObject iid -> sim go
        self.col_ident = {}           # recorded Collider2D iid -> "simgo/Type#ord"
        self._fsm_active = {}
        self.report = {"recording": rec_path, "level": self.level, "frames": [], "kinds": {}, "carried": {},
                       "import_lossy": {}, "initial": {}, "traps": {},
                       "contact_import": {}}

    # ------------------------------------------------------------------ sim table
    def _grow(self):
        lib = self.lib
        n = lib.hkls_refresh(self.ls)
        if n < 0:
            raise SystemExit("hkls_refresh: %s" % lib.hkls_error(self.ls).decode())
        ngo = lib.hkfsm_go_count(self.world)
        info = (ctypes.c_int32 * 8)()
        for g in range(len(self.sim_go_key), ngo):
            lib.hkfsm_go_info(self.world, g, info)
            if info[1]:
                self.sim_go_key.append(None)
                continue
            p = lib.hkfsm_go_path(self.world, g).decode()
            self.sim_go_key.append((p, self.sim_path_count[p]))
            self.sim_path_count[p] += 1
        if n == self.n:
            return []
        old = self.n
        out = (ctypes.c_int32 * 3)(); cs, fs = ctypes.c_char_p(), ctypes.c_char_p()
        go, typ, fl = [], [], []
        for i in range(old, n):
            lib.hkls_entry(self.ls, i, out, ctypes.byref(cs), ctypes.byref(fs))
            go.append(out[0]); typ.append(out[1]); fl.append(out[2])
            self.comp.append(cs.value.decode()); self.field.append(fs.value.decode())
            self.by_go[out[0]].append(i)
        self.go = np.concatenate([self.go, np.array(go, np.int32)])
        self.typ = np.concatenate([self.typ, np.array(typ, np.uint8)])
        self.flags = np.concatenate([self.flags, np.array(fl, np.uint8)])
        self.slot.extend([None] * (n - old))
        self.rec_val = np.concatenate([self.rec_val, np.zeros(n - old, np.int64)])
        self.mapped = np.concatenate([self.mapped, np.zeros(n - old, bool)])
        self.n = n
        return list(range(old, n))

    def export(self):
        """The sim's value of every entry, read inside a checkpoint save/restore (reads resolve lazy caches)."""
        lib = self.lib
        lib.hksim_checkpoint_save(self.sim, self.cp)
        out = np.zeros(self.n, np.int64)
        rc = lib.hkls_export(self.ls, out.ctypes.data, self.n)
        structs = {i: (lib.hkls_struct(self.ls, i) or b"").decode() for i in self.structs()}
        lib.hksim_checkpoint_restore(self.sim, self.cp)
        if rc != 0:
            raise RuntimeError("hkls_export: %s" % lib.hkls_error(self.ls).decode())
        return out, structs

    def structs(self):
        return [int(i) for i in np.nonzero(self.typ == ord("x"))[0]]

    def value_of(self, i, name, intern=0):
        key = (i if self.typ[i] == ord("e") else -1, name, intern)
        v = self.cache_value.get(key)
        if v is None:
            o = ctypes.c_int64()
            self.lib.hkls_value_of(self.ls, i, name.encode(), intern, ctypes.byref(o))
            v = self.cache_value[key] = o.value
        return v

    def show(self, i, v):
        if isinstance(v, tuple):
            return str(list(v[1]))
        if v == NONE:
            return "<none>"
        t = chr(self.typ[i])
        if t == "f":
            return repr(f32(v))
        if t in "es":
            return repr((self.lib.hkls_name_of(self.ls, i, int(v)) or b"").decode()) if v >= -1 else "<not in sim>"
        if t == "g":
            return "null" if v == -1 else "<not in sim>" if v < -1 else self.go_label(int(v))
        return str(int(v))

    def go_label(self, g):
        k = self.sim_go_key[g] if 0 <= g < len(self.sim_go_key) else None
        return "%s#%d" % k if k else "go%d" % g

    # ------------------------------------------------------------------ pairing
    def rec_go_for(self, g):
        k = self.sim_go_key[g] if g >= 0 else None
        if k is None:
            return None
        return self.R.go_by_key.get(k) or self.R.go_by_key.get(("DDOL/" + k[0], k[1]))

    def _component(self, goe, comp):
        r = self.R.rec
        kids = r.children(goe.eid)
        if comp == "GameObject":
            return goe
        if comp == "Rigidbody2D":
            return next((e for e in kids if e.cls.name == "UnityEngine.Rigidbody2D"), None)
        m = re.match(r"^(\w+Collider2D)#(\d+)$", comp)
        if m:
            same = sorted((e for e in kids if e.cls.name == "UnityEngine." + m.group(1)), key=lambda e: e.raw("index"))
            k = int(m.group(2))
            return same[k] if k < len(same) else None
        m = re.match(r"^FSM:(.*)#(\d+)(/S(\d+):(.*?))?(/A(\d+))?(/V)?$", comp)
        if m:
            name, k = m.group(1), int(m.group(2))
            same = sorted((e for e in kids if e.cls.name == "PlayMakerFSM" and e["fsm.name"] == name), key=lambda e: e.raw("index"))
            if k >= len(same):
                return None
            f = same[k]
            if m.group(8):
                return r.entities.get(f.raw("fsm.vars"))
            if m.group(3) is None:
                return f
            states = f.raw("fsm.states")
            si = int(m.group(4))
            if si >= len(states):
                return None
            st = r.entities.get(states[si])
            if st is None or st["name"] != m.group(5):
                return None
            if m.group(6) is None:
                return st
            acts = st.raw("actions")
            ai = int(m.group(7))
            return r.entities.get(acts[ai]) if ai < len(acts) else None
        return next((e for e in kids if e.cls.name == comp), None)

    def _global(self, comp):
        R = self.R
        if comp in ("Time", "Env", "Random"):
            return R.first(comp)
        if comp == "HeroController":
            return R.first("HeroController")
        if comp == "cState":
            hc = R.first("HeroController")
            return R.rec.entities.get(hc.raw("cState")) if hc is not None and "cState" in hc else None
        if comp == "PlayerData":
            return R.first("plain:PlayerData")
        if comp == "PlayMakerGlobals/V":
            pg = R.first("PlayMakerGlobals")
            return R.rec.entities.get(pg.raw("vars")) if pg is not None else None
        return None

    def resolve(self, i):
        """Pair entry i with its recorded (entity, field index, conversion), or None."""
        comp, field, t = self.comp[i], self.field[i], chr(self.typ[i])
        if t == "x" or field == "n.proxies":
            return None
        g = int(self.go[i])
        if comp in ("Time", "Env", "Random", "HeroController", "cState", "PlayerData", "PlayMakerGlobals/V"):
            e = self._global(comp)
        else:
            eid = self.rec_go_for(g)
            if eid is None:
                return ("absent",) if comp == "GameObject" and field == "exists" else None
            e = self._component(self.R.rec.entities[eid], comp)
        if e is None:
            return None
        if field == "exists":
            return (e.eid, -1, "one")
        m = re.match(r"^(.*)#(\d+)$", field)
        fname, elem = (m.group(1), int(m.group(2))) if m else (field, None)
        fi = e.cls.index.get(fname)
        if fi is None:
            self.pending_field[e.cls.id].add(i)
            return None
        rt = e.cls.types[fi]
        conv = None
        if elem is not None and rt in "IL":
            conv = ("elem", elem)
        elif t in "fiub" and (rt == t or (t in "iu" and rt in "ilb") or (t == "b" and rt in "b")):
            conv = "raw"
        elif t == "e" and rt == "s":
            conv = "name"
        elif t == "s" and rt == "s":
            conv = "string"
        elif t == "g" and rt == "o":
            conv = "go"
        elif t == "l" and rt == "I":
            conv = "list"
        if conv is None:
            return None
        return (e.eid, fi, conv)

    @property
    def env_eid(self):
        e = self.R.first("Env")
        return e.eid if e is not None else None

    def rec_value(self, i, sl):
        if sl[0] == "absent":
            return 0
        eid, fi, conv = sl
        if conv == "one":
            return 1
        e = self.R.rec.entities.get(eid)
        if e is None:
            return None
        raw = raw_of(e, fi)
        if conv == "raw":
            if eid == self.env_eid and raw < 0:
                return 0   # Env.step is -1 before the episode's first step; TrainingEnv._stepCount is 0 then
            return int(raw)
        if isinstance(conv, tuple):
            return int(raw[conv[1]]) if conv[1] < len(raw) else None
        if conv == "name":
            s = self.R.rec.strings[raw]
            return self.value_of(i, s[5:] if s.startswith("clip:") else s)
        if conv == "string":
            return self.value_of(i, self.R.rec.strings[raw])
        if conv == "go":
            return -1 if raw == 0 else self.iid_go.get(raw, UNKNOWN)
        if conv == "list":
            return hash_list(raw)
        return None

    def pair(self, entries):
        for i in entries:
            old = self.slot[i]
            if old is not None and old[0] != "absent":
                w = self.watch.get(old[0])
                if w is not None and i in w:
                    w.remove(i)
            sl = self.resolve(i)
            self.slot[i] = sl
            v = self.rec_value(i, sl) if sl is not None else None
            self.mapped[i] = v is not None
            self.rec_val[i] = v if v is not None else 0
            if sl is not None and sl[0] != "absent":
                self.watch[sl[0]].append(i)

    def refresh_identity(self, gos):
        """Re-pair every entry of the sim GameObjects `gos` (and their identity maps)."""
        R = self.R
        todo = []
        for g in gos:
            eid = self.rec_go_for(g) if g >= 0 else None
            if g >= 0:
                if eid is None:
                    self.go_rec.pop(g, None)
                else:
                    self.go_rec[g] = eid
                    e = R.rec.entities[eid]
                    self.iid_go[e.raw("iid")] = g
                    cnt = collections.Counter()
                    for c in sorted(R.rec.children(eid), key=lambda c: c.raw("index") if "index" in c else 0):
                        if c.cls.name.endswith("Collider2D") and "iid" in c:
                            t = c.cls.name.split(".")[-1]
                            self.col_ident[c.raw("iid")] = "%d/%s#%d" % (g, t, cnt[t])
                            cnt[t] += 1
            todo.extend(self.by_go.get(g, []))
        self.pair(todo)

    # ------------------------------------------------------------------ recorded per-frame values
    def _affected(self, fr):
        """sim GameObjects whose pairing a frame's births and deaths may change."""
        R = self.R
        rec_gos, gos = set(), set()
        for eid in fr.born:
            e = R.rec.entities.get(eid)
            if e is None:
                continue
            if e.cls.name == "GameObject":
                gos.update(self._sim_gos_for_key(e.key, e.raw("cloneIndex")))
            else:
                ge = R.go_of(e)
                if ge is not None:
                    rec_gos.add(ge.eid)
                elif e.cls.name in ("HeroController", "plain:HeroControllerStates", "plain:PlayerData", "PlayMakerGlobals"):
                    gos.add(-1)
        for e in fr.died:
            if e.cls.name == "GameObject":
                gos.update(self._sim_gos_for_key(e.key, e.raw("cloneIndex")))
            elif e.eid in self.watch:
                for i in self.watch[e.eid]:
                    gos.add(int(self.go[i]))
        inv = {v: k for k, v in self.go_rec.items()} if rec_gos else {}
        gos.update(inv[x] for x in rec_gos if x in inv)
        return gos

    def _sim_gos_for_key(self, key, ci):
        p = key[5:] if key.startswith("DDOL/") else key
        if not hasattr(self, "_key_go"):
            self._key_go = {}
        if len(self._key_go) != len(self.sim_go_key):
            self._key_go = {k: g for g, k in enumerate(self.sim_go_key) if k}
        g = self._key_go.get((p, ci))
        return [g] if g is not None else []

    def update_recorded(self, fr):
        R = self.R
        # classes that grew fields: retry entries waiting for one
        retry = set()
        for cid, c in R.rec.classes.items():
            if self.cls_nf.get(cid) != len(c.fields):
                self.cls_nf[cid] = len(c.fields)
                if cid in self.pending_field:
                    retry |= self.pending_field.pop(cid)
        affected = self._affected(fr)
        self.refresh_identity(sorted(affected))
        if retry:
            self.pair(sorted(retry))
        for eid, fis in fr.changed.items():
            for i in self.watch.get(eid, ()):
                sl = self.slot[i]
                if sl is not None and sl[1] in fis:
                    v = self.rec_value(i, sl)
                    self.mapped[i] = v is not None
                    self.rec_val[i] = v if v is not None else 0

    def rec_structs(self):
        """The recorded b2World contact list and PhysicsContacts2D m_Collisions, in the sim's identity space."""
        r = self.R.rec
        out = {}
        fx_col = {}
        for e in r.by_class.get("b2Fixture", {}).values():
            fx_col[e.eid] = e.raw("collider")
        ident = lambda iid: self.col_ident.get(iid, "?iid%d" % iid)
        w = self.R.first("b2World")
        if w is not None:
            parts = []
            for c in w.raw("contactList"):
                ce = r.entities.get(c)
                if ce is None:
                    continue
                parts.append("%s|%s" % (ident(fx_col.get(ce.raw("fixtureA"), 0)), ident(fx_col.get(ce.raw("fixtureB"), 0))))
            out["contactList"] = ";".join(parts)
        pc = self.R.first("PhysicsContacts2D")
        if pc is not None:
            parts = []
            for c in pc.raw("collisions"):
                ce = r.entities.get(c)
                if ce is None:
                    continue
                parts.append("%s|%s:%d" % (ident(ce.raw("colliderA")), ident(ce.raw("colliderB")), ce.raw("state")))
            out["collisions"] = ";".join(parts)
        # per-collider proxy ids
        prox = collections.defaultdict(list)
        for e in r.by_class.get("b2Fixture", {}).values():
            p = e.raw("proxies")
            prox[e.raw("collider")].extend(p[1::2])
        return out, prox

    # ------------------------------------------------------------------ the loop
    def frame_kind(self, prev, fr):
        if not fr.live:
            return FRAME_STEP if fr.step != prev.step else FRAME_FROZEN
        t = self.R.first("Time")
        return FRAME_LIVE_LAST if t is not None and t["timeScale"] == 0.0 else FRAME_LIVE

    def compare(self, sim_val, sim_structs):
        diff = np.nonzero(self.mapped & (sim_val != self.rec_val) & (sim_val != NONE))[0]
        rows = []
        for i in diff:
            i = int(i)
            if self.typ[i] == ord("l"):   # lists compare by hash; the row carries the lists themselves
                sl = self.slot[i]
                e = self.R.rec.entities[sl[0]]
                rows.append((i, (int(sim_val[i]), self.sim_list(i)), (int(self.rec_val[i]), tuple(raw_of(e, sl[1])))))
            else:
                rows.append((i, int(sim_val[i]), int(self.rec_val[i])))
        rstr, prox = self.rec_structs()
        for i in self.structs():
            f = self.field[i]
            if f in rstr and rstr[f] != sim_structs.get(i, ""):
                rows.append((i, sim_structs.get(i, ""), rstr[f]))
        inv_col = {v: k for k, v in self.col_ident.items()}
        for i in self.proxy_entries():
            ident = "%d/%s" % (self.go[i], self.comp[i])
            iid = inv_col.get(ident)
            if iid is None:
                continue
            want = prox.get(iid, [])
            if int(sim_val[i]) != hash_list(want):
                rows.append((int(i), (int(sim_val[i]), self.sim_list(i)), (hash_list(want), tuple(want))))
        return rows

    def sim_list(self, i):
        buf = (ctypes.c_int32 * 256)()
        n = self.lib.hkls_list(self.ls, i, buf, 256)
        return tuple(buf[:max(0, min(n, 256))])

    def proxy_entries(self):
        if not hasattr(self, "_prox") or len(self._prox_n) != 1 or self._prox_n[0] != self.n:
            self._prox = [i for i in range(self.n) if self.field[i] == "n.proxies"]
            self._prox_n = [self.n]
        return self._prox

    def do_import(self, rows):
        """Write the recorded values of the differing importable entries; returns (written, skipped)."""
        idx, vals = [], []
        for i, _s, rv in rows:
            if isinstance(rv, str) or self.flags[i] & RO or self.field[i] == "n.proxies":
                continue
            if isinstance(rv, tuple):
                lst = (ctypes.c_int32 * max(1, len(rv[1])))(*rv[1])
                if self.lib.hkls_import_list(self.ls, i, lst, len(rv[1])) != 0:
                    raise RuntimeError("hkls_import_list: %s" % self.lib.hkls_error(self.ls).decode())
                continue
            if self.typ[i] == ord("s") and rv == UNKNOWN:   # a string the sim has never seen: intern it
                sl = self.slot[i]
                e = self.R.rec.entities.get(sl[0])
                txt = self.R.rec.strings[raw_of(e, sl[1])]
                rv = self.value_of(i, txt, 1)
                self.cache_value[(-1, txt, 0)] = rv
                self.rec_val[i] = rv
            idx.append(i); vals.append(rv)
        if not idx:
            return 0, 0
        a = np.array(idx, np.int32); v = np.array(vals, np.int64)
        sk = ctypes.c_int32()
        if self.lib.hkls_import(self.ls, a.ctypes.data, v.ctypes.data, len(idx), ctypes.byref(sk)) != 0:
            t = "import: " + self.lib.hkls_error(self.ls).decode()
            self.report["traps"][t] = self.report["traps"].get(t, 0) + 1
        return len(idx), sk.value

    def label(self, i):
        g = int(self.go[i])
        owner = self.go_label(g) if g >= 0 and self.comp[i] not in ("HeroController", "cState") else ""
        return "%s|%s|%s" % (owner, self.comp[i], self.field[i])

    def importable(self, i, b):
        return not (isinstance(b, str) or self.flags[i] & RO or self.field[i] == "n.proxies")

    def fsm_entry(self, i):
        """The `fsm.active` entry of the FSM that entry i belongs to."""
        key = (int(self.go[i]), self.comp[i].split("/", 1)[0])
        j = self._fsm_active.get(key)
        if j is None:
            j = next((j for j in self.by_go.get(key[0], []) if self.comp[j] == key[1] and self.field[j] == "fsm.active"), None)
            self._fsm_active[key] = j
        return j

    def reenter(self, rows):
        """Enter the recorded active state of every FSM the game switched (or re-entered) during the frame, with the
        recorded values of its actions' private fields (hkls_enter_state).  Returns how many."""
        todo = {}
        for i, a, b in rows:
            f = self.field[i]
            if f == "fsm.active" and self.slot[i] is not None:
                todo[i] = b
            elif self.comp[i].startswith("FSM:") and f in ("loopCount", "stateTime") and "/A" not in self.comp[i]:
                if f == "stateTime" and f32(b) >= f32(a):
                    continue
                j = self.fsm_entry(i)
                m = re.search(r"/S(\d+):", self.comp[i])
                if j is not None and m and int(self.rec_val[j]) == int(m.group(1)):
                    todo.setdefault(j, int(self.rec_val[j]))
        r = self.R.rec
        for j, st in todo.items():
            if st < -1:
                continue
            fe = r.entities.get(self.slot[j][0]) if self.slot[j] else None
            states = fe.raw("fsm.states") if fe is not None else ()
            se = r.entities.get(states[st]) if 0 <= st < len(states) else None
            for k, aid in enumerate(se.raw("actions") if se is not None else ()):
                ae = r.entities.get(aid)
                if ae is None:
                    continue
                for fi, (fname, t) in enumerate(zip(ae.cls.fields, ae.cls.types)):
                    if t == "f" and not fname.startswith("base.") and len(fname) < 48:
                        self.lib.hkls_live_add(self.ls, j, st, k, fname.encode(), f32(raw_of(ae, fi)))
            if self.lib.hkls_enter_state(self.ls, j, st) != 0:
                t = "import: " + self.lib.hkls_error(self.ls).decode()
                self.report["traps"][t] = self.report["traps"].get(t, 0) + 1
        return len(todo)

    def sync(self, rows):
        """Import the recorded frame into the sim: re-enter the states the game entered, then write every differing
        field, until nothing importable is left (at most three rounds: a SetActive or a state entry can change fields
        that already matched).  Returns the rows still differing: the import's residual."""
        for _round in range(3):
            if self.reenter(rows):
                sim_val, ss = self.export()
                rows = self.compare(sim_val, ss)
            written, _sk = self.do_import(rows)
            sim_val, ss = self.export()
            rows = self.compare(sim_val, ss)
            if not written or not any(self.importable(i, b) for i, _a, b in rows):
                break
        self.import_contacts()
        sim_val, ss = self.export()
        return self.compare(sim_val, ss)

    def kind_of(self, i):
        """field_kind, marked [ui] for the objects under UI_ROOTS."""
        k = field_kind(self.comp[i], self.field[i])
        g = int(self.go[i])
        key = self.sim_go_key[g] if 0 <= g < len(self.sim_go_key) else None
        return k + " [ui]" if key and key[0].startswith(UI_ROOTS) else k

    def import_contacts(self):
        """The game's touching collider pairs (PhysicsContacts2D m_Collisions) into the sim's contact state."""
        lib, r = self.lib, self.R.rec
        lib.hkls_update_pairs(self.ls)
        pc = self.R.first("PhysicsContacts2D")
        stats = self.report["contact_import"]
        for c in (pc.raw("collisions") if pc is not None else ()):
            ce = r.entities.get(c)
            if ce is None:
                continue
            ids = []
            for iid in (ce.raw("colliderA"), ce.raw("colliderB")):
                m = re.match(r"^(-?\d+)/(\w+)#(\d+)$", self.col_ident.get(iid, ""))
                ids.append((int(m.group(1)), COL_TYPES.index(m.group(2)), int(m.group(3))) if m and m.group(2) in COL_TYPES else None)
            if None in ids:
                res = "not paired"
            else:
                res = TOUCH.get(lib.hkls_touch(self.ls, *ids[0], *ids[1], ce.raw("state")), "trap")
            stats[res] = stats.get(res, 0) + 1

    def count(self, table, rows, frame_index):
        for k, c in collections.Counter(self.kind_of(i) for i, _a, _b in rows).items():
            d = table.setdefault(k, {"frames": 0, "fields": 0, "first_frame": frame_index, "objects": collections.Counter()})
            d["frames"] += 1
            d["fields"] += c
        for i, _a, _b in rows:
            table[self.kind_of(i)]["objects"][self.label(i)] += 1

    def start(self):
        """Import the recorded first frame (SceneReady) into the sim."""
        R = self.R
        self.refresh_identity(range(-1, len(self.sim_go_key)))
        self.update_recorded(R.fr)
        sim_val, sim_structs = self.export()
        rows = self.compare(sim_val, sim_structs)
        self.report["initial"] = {"differing": len(rows), "kinds": dict(collections.Counter(
            self.kind_of(i) for i, _a, _b in rows).most_common())}
        left = self.sync(rows)
        self.resid = set(left)
        self.prev = R.fr
        return left

    def advance(self):
        """Step the sim one frame of the next recorded frame's kind and compare it with that frame.  Returns
        (frame, kind, rows, fresh, trap), or None at the recording's end.  `rows` differ after the step; `fresh` are
        the ones the import had not already left differing (the step's own); the sim is then synced to the frame."""
        fr = self.R.next()
        if fr is None:
            return None
        kind = self.frame_kind(self.prev, fr)
        act = None
        if kind == FRAME_STEP:
            if self.actions is None or fr.step - 1 >= len(self.actions):
                raise SystemExit("frame %d: step %d has no recorded action (pass --corpus)" % (fr.index, fr.step))
            act = (ctypes.c_int32 * 4)(*[int(x) for x in self.actions[fr.step - 1]])
        rc = self.lib.hksim_frame(self.sim, kind, act)
        trap = self.lib.hksim_last_error(self.sim).decode() if rc != 0 else None
        new = self._grow()
        if new:
            self.refresh_identity(sorted({int(self.go[i]) for i in new}))
        self.update_recorded(fr)
        sim_val, sim_structs = self.export()
        self.stepped = sim_val   # the sim's values after the step, before the sync
        rows = self.compare(sim_val, sim_structs)
        # a difference the import could not remove and the step left as it was is carried, not this step's
        fresh = [r for r in rows if r not in self.resid]
        out = (fr, kind, rows, fresh, trap)
        left = self.sync(rows)
        lossy = self.report["import_lossy"]
        for i, _a, b in left:
            if self.importable(i, b):
                k = self.kind_of(i)
                lossy[k] = lossy.get(k, 0) + 1
        self.resid = set(left)
        self.prev = fr
        return out

    def run(self, max_frames=None):
        self.start()
        kinds, carried = self.report["kinds"], self.report["carried"]
        n_frames = 0
        while max_frames is None or n_frames < max_frames:
            r = self.advance()
            if r is None:
                break
            fr, kind, rows, fresh, trap = r
            n_frames += 1
            if trap:   # the frame stopped inside the sim: counted under the trap (its differences are the trap's)
                self.report["traps"][trap] = self.report["traps"].get(trap, 0) + 1
            else:
                self.count(kinds, fresh, fr.index)
                self.count(carried, [x for x in rows if x not in fresh], fr.index)
            gp = [x for x in fresh if not self.kind_of(x[0]).endswith(" [ui]")]
            first = [{"field": self.label(i), "sim": self.show(i, a) if not isinstance(a, str) else a[:300],
                      "game": self.show(i, b) if not isinstance(b, str) else b[:300]} for i, a, b in gp[:self.max_first]]
            self.report["frames"].append({"index": fr.index, "frame": fr.frame, "step": fr.step, "kind": KIND_NAMES[kind],
                                          "differing": len(gp), "differing_ui": len(fresh) - len(gp),
                                          "carried": len(rows) - len(fresh), "first": first,
                                          **({"trap": trap} if trap else {})})
            if len(self.report["traps"]) > 50 or sum(self.report["traps"].values()) > 200:
                self.log("frame %d: too many traps, stopping" % fr.index)
                break
        self.report["n_frames"] = n_frames
        self.report["entries"] = self.n
        self.report["paired"] = int(self.mapped.sum())
        return self.report

    def close(self):
        self.lib.hksim_checkpoint_free(self.cp)
        self.lib.hkls_close(self.ls)
        self.lib.hksim_destroy(self.sim)


def find_actions(rec_path, corpus=None):
    """The episode's actions: --corpus, a corpus next to the recording, or the STEP events of its .hktrace."""
    cands = [corpus] if corpus else []
    base = rec_path[:-len(".hkstate")]
    stem = re.sub(r"(\.a|\.e\d+)$", "", base)
    for s in (stem, re.sub(r"_(on|off)$", "", stem)):
        # corpora_v2 keeps a scene's full-state episodes in <scene>/state/, next to their own corpus
        cands += [s + ".corpus.json", os.path.join(os.path.dirname(os.path.dirname(s)), os.path.basename(s) + ".corpus.json")]
    for c in cands:
        if c and os.path.exists(c):
            with open(c, encoding="utf-8") as fh:
                return json.load(fh)["steps"]
    tr = base + ".hktrace"
    if os.path.exists(tr):
        from hkpy import hktrace
        t = hktrace.read_trace(tr)
        steps = [r.args["action"] for r in t.records if r.kind == 0x10 and r.ev == 1]
        return steps or None
    return None


def ranked(kinds, top):
    """Table lines for {kind: {frames, fields, first, objects}}: gameplay kinds by the frames they diverge in, then one
    line for the [ui] kinds."""
    gp = sorted(((k, d) for k, d in kinds.items() if not k.endswith(" [ui]")), key=lambda kv: (-kv[1]["frames"], kv[0]))
    ui = [d for k, d in kinds.items() if k.endswith(" [ui]")]
    lines = ["%-52s %5s %7s %8s  %-24s %s" % ("field kind", "recs", "frames", "fields", "first", "most frequent object")]
    for k, d in gp[:top]:
        obj = max(d["objects"].items(), key=lambda kv: kv[1])[0] if d["objects"] else ""
        lines.append("%-52s %5d %7d %8d  %-24s %s" % (k[:52], d.get("recordings", 1), d["frames"], d["fields"],
                                                      str(d["first"])[:24], obj[:100]))
    lines.append("%d gameplay kinds; %d [ui] kinds in %d frames (not ranked)" % (len(gp), len(ui), max([d["frames"] for d in ui] or [0])))
    return lines


def summarize(rep, top=25):
    return ranked({k: dict(d, first=d["first_frame"]) for k, d in rep["kinds"].items()}, top)


def merge(paths, top=40):
    """The ranked table over several reports (--out files): a kind's frames and fields summed, its recordings counted,
    its first divergence the earliest recording and frame."""
    agg, traps, lines = {}, collections.Counter(), []
    for p in sorted(paths):
        with open(p, encoding="utf-8") as fh:
            rep = json.load(fh)
        name = os.path.basename(rep["recording"]).replace(".a.hkstate", "")
        nd = sum(1 for f in rep["frames"] if f.get("differing"))
        lines.append("%-12s %-26s %5d frames %5d diverging %4d traps  first: %s" % (
            name, rep["level"], rep["n_frames"], nd, sum(rep["traps"].values()),
            next(("%d %s" % (f["index"], f["first"][0]["field"][:70]) for f in rep["frames"] if f.get("first")), "-")))
        traps.update(rep["traps"])
        for k, d in rep["kinds"].items():
            a = agg.setdefault(k, {"recordings": 0, "frames": 0, "fields": 0, "first": None, "objects": collections.Counter()})
            a["recordings"] += 1
            a["frames"] += d["frames"]
            a["fields"] += d["fields"]
            if a["first"] is None or d["first_frame"] < int(a["first"].split("@")[1]):
                a["first"] = "%s@%d" % (name, d["first_frame"])
            a["objects"].update(d["objects"])
    lines += ranked(agg, top)
    for t, c in traps.most_common(20):
        lines.append("trap x%d: %s" % (c, t[:150]))
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("recording", nargs="+", help="a .hkstate, or with --merge the --out reports to rank together")
    ap.add_argument("--merge", action="store_true")
    ap.add_argument("--corpus")
    ap.add_argument("--out")
    ap.add_argument("--frames", type=int)
    ap.add_argument("--top", type=int, default=25)
    a = ap.parse_args(argv)
    if a.merge:
        for line in merge(a.recording, a.top):
            print(line)
        return None
    lib = load_lib()
    a.recording = a.recording[0]
    L = Lockstep(lib, a.recording, find_actions(a.recording, a.corpus), log=lambda s: print(s, flush=True))
    rep = L.run(a.frames)
    L.close()
    nd = sum(1 for f in rep["frames"] if f.get("differing"))
    print("lockstep %s: %d frames, %d with divergence, %d entries (%d paired), initial %d differing, %d traps"
          % (os.path.basename(a.recording), rep["n_frames"], nd, rep["entries"], rep["paired"], rep["initial"]["differing"],
             sum(rep["traps"].values())))
    for t, c in sorted(rep["traps"].items(), key=lambda kv: -kv[1]):
        print("trap x%d: %s" % (c, t[:160]))
    for line in summarize(rep, a.top):
        print(line)
    if a.out:
        for t in (rep["kinds"], rep["carried"]):
            for d in t.values():
                d["objects"] = dict(d["objects"].most_common(10))
        with open(a.out, "w", encoding="utf-8") as fh:
            json.dump(rep, fh, indent=1)
    return rep


if __name__ == "__main__":
    main()
