"""Method-level oracle: replay every call the game recorded (oracle/Record/MethodRecorder.cs) through the sim, one
callback at a time, and compare what it wrote.  docs/method-oracle.md has the record format and the method.

    python tools/method_oracle.py replay <file.methods.jsonl.gz | dir> ... [--out report.json] [--jobs N]
    python tools/method_oracle.py inert <on.hktrace> <off.hktrace>      # recorder inertness (OBS from step 2)
    python tools/method_oracle.py selfcheck <file | dir> ...              # recorder known answers (literal Set*Value)
    python tools/method_oracle.py pd <file | dir> ...                     # PlayerData at scene load: sim reset vs the game
    python tools/method_oracle.py coverage <report.json>                  # where the never-compared action types live
    python tools/method_oracle.py show <file | dir> --type T [--verdict mismatch] [--n 3]   # every differing field
    python tools/method_oracle.py record [--scenes a,b] [--policy 4] [--perturbed 2] [--jobs 4]   # needs HKRL_GAME

For each action activation (OnEnter .. OnExit of one action instance) the replay restores a reset world, finds the
FSM and action by identity, and for each recorded callback in order: sets the inputs the game had (the FSM's and the
globals' variables, the touched GameObjects' local poses down their parent chains, velocity, active flags, animator
and HealthManager state, the knight's HeroActions, PlayerData, the RNG words, the clocks, the FSM's active state and
pending transition), runs that one callback through the action's vtable, and compares the outputs (variables,
GameObjects, PlayerData, RNG state, Finished, pending transition, events sent, delayed events) bit-exactly.  The sim
carries the action's private state from one callback to the next, as the game does.  Component calls
(HealthManager.Hit, Recoil, tk2dSpriteAnimator) are single calls.

Verdicts per call:
  exact      every compared output is bit-identical
  mismatch   an output differs: first field, game value, sim value
  rng-only   only the RNG state after the call differs (the draw count; not a defect by itself, principles.md)
  input      outputs differ and the recorded inputs could not be reproduced (a GameObject the call reads is missing
             in the sim, or its world position reads back far off after its local pose chain is set): the call is not
             evidence either way
  trap       the sim trapped (HKSIM_UNIMPLEMENTED / ASSERT)
  unported   the sim has no vtable (or no ported function) for it
  identity   the sim has no such GameObject / FSM / state / action, or another type at that index
Calls that cascaded into other action callbacks in the game (`nd` > 0) are compared on their own FSM's outputs only
(variables, Finished, pending transition, events, delayed events): the cascade's effects belong to other records.
"""
import argparse, collections, ctypes as C, gzip, json, os, struct, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
from hkpy import sim_driver  # noqa: E402

BUCKET = {"f": 0, "i": 1, "b": 2, "s": 3, "v2": 4, "v3": 5, "r": 6, "q": 7, "c": 8, "go": 9, "a": 10, "e": 11, "o": 12, "m": 13, "t": 14}
BUCKET_NAME = {v: k for k, v in BUCKET.items()}
SKIP_BUCKETS = {"a", "o", "m", "t"}          # not modelled as values by the sim (fsm.h fsm_val)
CB = {"E": 0, "U": 1, "F": 2, "L": 3, "X": 4, "V": 5}
HM_FIELDS = ("hp", "isDead", "invincible", "invincibleFromDirection", "evasionByHitRemaining", "directionOfLastAttack")
RECOIL_STATES = {"Ready": 0, "Frozen": 1, "Recoiling": 2}
stats_compose = collections.Counter()   # GameObjects whose world position the sim composes off by rounding (set_gos)
# actions that take a clone from a pool: the clone they spawn and the clones the record names share one canonical path
SPAWN_TYPES = ("SpawnObjectFromGlobalPool", "SpawnObjectFromGlobalPoolOverTime", "SpawnObjectFromGlobalPoolOverTimeV2", "CreateObject",
               "FlingObjectsFromGlobalPool", "FlingObjectsFromGlobalPoolTime", "FlingObjectsFromGlobalPoolVel", "SpawnFromPool", "SpawnRandomObjectsV2",
               "AudioPlayerOneShot", "AudioPlayerOneShotSingle")
HERO_TYPES = ("CallMethodProper", "SendMessage")   # their receivers on the knight are HeroController methods (MethodRecorder.cs kHeroTypes)
VERDICTS = ("exact", "mismatch", "rng-only", "input", "trap", "unported", "identity")


def fbits(x):
    """float32 bits of a JSON number (the recorder writes float.ToString("R"), which round-trips)."""
    return struct.unpack("<I", struct.pack("<f", float(x)))[0]


def bits_f(u):
    return struct.unpack("<f", struct.pack("<I", u))[0]


def canon(path):
    """The game's canonical path of a sim GameObject path: `$k` / `#k` pool-copy suffixes dropped (sim/core/rng.c)."""
    out = []
    for seg in path.split("/"):
        for mark in ("$", "#"):
            k = seg.rfind(mark)
            if k > 0 and seg[k + 1:].isdigit():
                seg = seg[:k]
        out.append(seg)
    return "/".join(out)


# ------------------------------------------------------------------ records

def read_records(path):
    op = gzip.open if path.endswith(".gz") else open
    with op(path, "rt", encoding="utf-8") as f:
        try:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    yield json.loads(line)
                except json.JSONDecodeError:
                    return   # a stream cut short by a killed process: the complete lines before it stand
        except (EOFError, OSError):
            return


class Episode:
    def __init__(self, level, head, marker):
        self.level, self.head, self.marker, self.recs = level, head, marker, []
        self.pd_base = marker.get("pd", {})
        self.pd_type = marker.get("pdt", {})
        self.knight = marker.get("knight") or "Knight"
        self.version = int((head or {}).get("methods", 2))


def level_key(path, level):
    """The sim's level key of a recording: BossLevel 0 is the canonical dump (no tier suffix); BossLevel k>=1
    is `<scene>@T<k>` (analysis/dumps_t<k>, gen_tables.py split_level_key, sim/core/scene_registry.inc -- there
    is no `@T0` entry, only the bare scene name). The directory a recording lies in spells out k as
    `<scene>@T<k>` (`method_oracle.py record --tier k`, including k=0) or `<scene>_T<k>` (corpora_v2/port's
    per-tier corpus directories); either way, k selects the key, not the separator text."""
    d = os.path.basename(os.path.dirname(os.path.abspath(path)))
    for sep in ("@T", "_T"):
        prefix = level + sep
        if d.startswith(prefix) and d[len(prefix):].isdigit():
            k = int(d[len(prefix):])
            return level if k == 0 else "%s@T%d" % (level, k)
    return level


def episodes(path):
    out, cur, head = [], None, None
    for r in read_records(path):
        if "methods" in r:
            head = r
        elif r.get("ev") == "scene_ready":
            cur = Episode(level_key(path, r["level"]), head, r)
            out.append(cur)
        elif "k" in r and cur is not None:
            cur.recs.append(r)
    return out


def full_var_maps(recs):
    """Attach the full before / after variable maps to each record of one activation (deltas in the file)."""
    prev = {}
    for r in recs:
        before = dict(prev)
        before.update(r.get("vb", {}))
        after = dict(before)
        after.update(r.get("va", {}))
        r["_vb"], r["_va"] = before, after
        prev = after


# ------------------------------------------------------------------ sim binding

class TrapError(Exception):
    pass


class Sim:
    def __init__(self, dll=None):
        self.lib = L = sim_driver.load(dll or sim_driver.DLL)
        P, I, F, S = C.c_void_p, C.c_int32, C.c_float, C.c_char_p
        FP, IP, UP = C.POINTER(C.c_float), C.POINTER(C.c_int32), C.POINTER(C.c_uint32)

        def fn(name, res, *args):
            f = getattr(L, name); f.restype = res; f.argtypes = list(args); return f
        self.world = fn("hkmo_world", P, P)
        fn("hksim_bind_arena", None, P)
        self.err = fn("hkmo_last_error", S)
        self.reg_count = fn("hkmo_registry_count", I)
        self.reg_name = fn("hkmo_registry_name", S, I)
        self.find_go = fn("hkmo_find_go", I, P, S, I)
        self.go_path = fn("hkmo_go_path", S, P, I)
        self.find_fsm = fn("hkmo_find_fsm", I, P, I, S)
        self.state_index = fn("hkmo_state_index", I, P, I, S)
        self.action_type = fn("hkmo_action_type", S, P, I, I, I, IP)
        self.var_set = fn("hkmo_var_set", C.c_int, P, I, I, S, FP, I, S)
        self.vars_dump = fn("hkmo_vars_dump", I, P, I, C.c_char_p, I)
        self.go_get = fn("hkmo_go_get", C.c_int, P, I, FP)
        self.go_set = fn("hkmo_go_set", C.c_int, P, I, FP, FP, F, FP, I, I)
        self.go_set_local = fn("hkmo_go_set_local", C.c_int, P, I, FP, FP, F)
        self.go_set_gravity = fn("hkmo_go_set_gravity", C.c_int, P, I, F)
        self.anim_set = fn("hkmo_anim_set", C.c_int, P, I, S, F, F, I, I, I)
        self.anim_get = fn("hkmo_anim_get", S, P, I, FP)
        self.hm_set = fn("hkmo_hm_set", C.c_int, P, I, FP)
        self.hm_get = fn("hkmo_hm_get", C.c_int, P, I, FP)
        self.recoil_set = fn("hkmo_recoil_set", C.c_int, P, I, FP)
        self.recoil_get = fn("hkmo_recoil_get", C.c_int, P, I, FP)
        self.input_set = fn("hkmo_input_set", C.c_int, P, IP, I)
        self.pd_set = fn("hkmo_pd_set", C.c_int, P, S, I, F, I)
        self.pd_get = fn("hkmo_pd_get", C.c_int, P, S, I, FP, IP)
        self.set_clock = fn("hkmo_set_clock", None, P, F, F, F, C.c_uint32, I)
        self.set_rng = fn("hkmo_set_rng", None, P, UP)
        self.get_rng = fn("hkmo_get_rng", None, P, UP)
        self.log_mark = fn("hkmo_log_mark", I, P)
        self.log_count = fn("hkmo_log_count", I, P)
        self.log_event = fn("hkmo_log_event", C.c_int, P, I, IP, C.POINTER(S), IP)
        self.delayed_count = fn("hkmo_delayed_count", I, P, I)
        self.delayed_get = fn("hkmo_delayed_get", S, P, I, I, FP)
        self.switch_to = fn("hkmo_switch_to", S, P, I)
        self.prepare = fn("hkmo_prepare", C.c_int, P, I, I, F, I, I)
        self.go_parent = fn("hkmo_go_parent", I, P, I)
        self.go_set_parent = fn("hkmo_go_set_parent", C.c_int, P, I, I)
        self.lse_set = fn("hkmo_lse_set", C.c_int, P, I, IP, I)
        self.go_activate = fn("hkmo_go_activate", C.c_int, P, I, I)
        self.act_state_get = fn("hkmo_act_state_get", I, P, I, I, I, C.c_void_p, I)
        self.act_state_set = fn("hkmo_act_state_set", None, P, I, I, I, C.c_char_p, I)
        self.reserve = fn("hkmo_reserve", None, P)
        self.alert_set = fn("hkmo_alert_set", None, P, I, I)
        self.set_event_data = fn("hkmo_set_event_data", None, P, I, I, F, S)
        self.call = fn("hkmo_call", C.c_int, P, I, I, I, I, S, I, IP)
        self.hm_hit = fn("hkmo_hm_hit", C.c_int, P, I, I, I, I, F, I, I, F, F)
        self.recoil_by_direction = fn("hkmo_recoil_by_direction", C.c_int, P, I, I, F)
        self.recoil_fixed_update = fn("hkmo_recoil_fixed_update", C.c_int, P, I)
        self.anim_update = fn("hkmo_anim_update", C.c_int, P, I, F)
        self.anim_play = fn("hkmo_anim_play", C.c_int, P, I, S, F, F)
        self.set_value = fn("hksim_set_value", C.c_int, P, S, C.c_double)
        self.get_value = fn("hksim_get_value", C.c_int, P, S, C.POINTER(C.c_double))
        self.s = None
        self.cp = None
        self._buf = C.create_string_buffer(1 << 16)

    def registry(self):
        return sorted(self.reg_name(i).decode() for i in range(self.reg_count()))

    def open(self, level, seed):
        """A reset world of `level` and its checkpoint (restored before every activation)."""
        if self.s:
            self.lib.hksim_checkpoint_free(self.cp)
            self.lib.hksim_destroy(self.s)
        cfg = sim_driver.Config(level=level.encode(), frames_per_wait=2, seed=seed, trace=0)
        self.s = self.lib.hksim_create(C.byref(cfg))
        if not self.s:
            raise RuntimeError("hksim_create(%s): %s" % (level, (self.lib.hksim_last_error(None) or b"?").decode()))
        rc = self.lib.hksim_reset(self.s, seed)
        if rc != 0:
            raise RuntimeError("hksim_reset(%s): %s" % (level, self.lib.hksim_last_error(self.s).decode()))
        self.w = self.world(self.s)
        self.lib.hksim_bind_arena(self.s)
        self.cp = self.lib.hksim_checkpoint_new(self.s)
        self._go_cache, self._path_cache, self._live_cache = {}, {}, {}
        # GameObjects below this index are the checkpoint's and keep their index across restores; one an activation
        # Instantiated (ObjectPool.Spawn growing a pool) is gone after the restore, so its index is not cached.
        v = C.c_double()
        self.get_value(self.s, b"fsm.n_gos", C.byref(v))
        self.n_gos0 = int(v.value)

    def restore(self):
        if self.lib.hksim_checkpoint_restore(self.s, self.cp) != 0:
            raise RuntimeError("checkpoint restore failed")

    def set_pd_base(self, ep):
        """The game's PlayerData store at SceneReady into the reset world, then its checkpoint again: a record's `pd`
        holds only the fields that differ from that store."""
        for k, v in ep.pd_base.items():
            _pd_write(self, ep, k, v)
        if self.lib.hksim_checkpoint_save(self.s, self.cp) != 0:
            raise RuntimeError("checkpoint save failed")

    def go(self, path):
        if path is None:
            return -1
        g = self._go_cache.get(path)
        if g is None:
            g = self.find_go(self.w, path.encode(), 0)
            if g < self.n_gos0:
                self._go_cache[path] = g
        return g

    def gpath(self, g):
        p = self._path_cache.get(g)
        if p is None:
            p = canon(self.go_path(self.w, g).decode(errors="replace")) if g >= 0 else None
            if g < self.n_gos0:
                self._path_cache[g] = p
        return p

    def vars(self, fsm):
        n = self.vars_dump(self.w, fsm, self._buf, len(self._buf))
        if n > len(self._buf):
            self._buf = C.create_string_buffer(n + 1024)
            self.vars_dump(self.w, fsm, self._buf, len(self._buf))
        out = {}
        for line in self._buf.value.decode("utf-8", errors="replace").split("\n"):
            if line:
                b, name, val = line.split("\t", 2)
                out.setdefault(BUCKET_NAME[int(b)] + ":" + name, val)   # the first of a name, as FsmVariables.GetFsm*(name)
        return out

    def rng(self):
        a = (C.c_uint32 * 4)()
        self.get_rng(self.w, a)
        return list(a)

    def errmsg(self):
        return self.err().decode(errors="replace")


# ------------------------------------------------------------------ value encodings (game JSON <-> sim dump text)

def game_val_key(bucket, v, sim):
    """The game's JSON value of a variable in the sim dump's text form, or None when it cannot be expressed."""
    if bucket == "f":
        return "%08x" % fbits(v)
    if bucket in ("i", "e", "b"):
        return "%d" % int(v) if v is not None else None
    if bucket == "s":
        return "\x01" if v is None else v
    if bucket in ("v2", "v3", "r", "q", "c"):
        xs = list(v) + [0.0] * (4 - len(v))
        return ",".join("%08x" % fbits(x) for x in xs[:4])
    if bucket == "go":
        return "%d" % sim.go(v) if v is not None else "-1"
    return None


def show_sim_val(bucket, s, sim):
    if bucket == "f":
        return bits_f(int(s, 16))
    if bucket in ("v2", "v3", "r", "q", "c"):
        return [bits_f(int(x, 16)) for x in s.split(",")]
    if bucket == "go":
        return sim.gpath(int(s))
    if bucket == "s":
        return None if s == "\x01" else s
    return s


def feq(a, b):
    """Bit-identical float32, except that +0 and -0 are equal: no HK code path divides by a zero it could tell apart,
    and Unity canonicalises some signed zeros natively (a Rigidbody2D velocity set to -0 reads back +0)."""
    x, y = fbits(a), fbits(b)
    return x == y or (x & 0x7FFFFFFF) == 0 == (y & 0x7FFFFFFF)


def vec_eq(game_vec, sim_vec, n):
    return all(feq(game_vec[k], sim_vec[k]) for k in range(n))


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


DEG2RAD, RAD2DEG = bits_f(0x3C8EFA35), bits_f(0x42652EE1)   # Mathf.Deg2Rad, Mathf.Rad2Deg (Mathf.cs:171,174)
MP_NEG = bits_f(0xBBBBBF2F)                                  # fl(-0.0001f * Rad2Deg), Quaternion.cs:164-183
MP_POS = f32(360.0 + MP_NEG)


def unity_euler_z(z):
    """localEulerAngles.z after `localEulerAngles = (0, 0, z)`: Unity stores the quaternion, not the angle
    (analysis/native_specs/native-transform_time.md T0, §3.5 EulerToQuaternion, §2.5 NormalizeSafe + QuaternionToEuler
    + MakePositive).  +-*/ and sqrt of float32 operands are computed in double and rounded once, which is exact
    rounding; sinf / cosf / atan2f are libm stand-ins for the UCRT routines the engine links (the spec's own caveat)."""
    import math
    h = f32(f32(f32(z) * DEG2RAD) * 0.5)
    qz, qw = f32(math.sin(h)), f32(math.cos(h))
    m = f32(math.sqrt(f32(f32(qz * qz) + f32(qw * qw))))
    if m < 1e-05:
        qz, qw = 0.0, 1.0
    else:
        qz, qw = f32(qz / m), f32(qw / m)
    zr = f32(math.atan2(f32(2.0 * f32(qw * qz)), f32(f32(-f32(qz * qz)) + f32(qw * qw))))
    zd = f32(zr * RAD2DEG)
    if zd < MP_NEG:
        zd = f32(zd + 360.0)
    elif zd > MP_POS:
        zd = f32(zd - 360.0)
    return zd


stats_readback = collections.Counter()   # localEulerZ outputs equal only after Unity's quaternion round trip, per type


def angle_eq(a, b, typ=None):
    """The game's localEulerAngles.z against the sim's stored angle: equal mod 360, or equal to what Unity reads back
    after storing that angle as a quaternion (the sim stores the angle; a port that computed the game's angle
    then differs from the game only through that round trip)."""
    x, y = float(a) % 360.0, float(b) % 360.0
    if fbits(x) == fbits(y):
        return True
    if fbits(a) == fbits(unity_euler_z(b)):
        stats_readback[typ] += 1
        return True
    return False


# ------------------------------------------------------------------ inputs

def set_vars(sim, fsm, vmap, local_keys):
    fv = (C.c_float * 4)()
    for key, v in vmap.items():
        glob = key.startswith("G")
        k = key[1:] if glob else key
        b, name = k.split(":", 1)
        alert = b == "o" and isinstance(v, list) and len(v) == 2 and v[0] == "AlertRange"   # FindAlertRange's store
        if (b in SKIP_BUCKETS and not alert) or (not glob and k not in local_keys):
            continue
        iv, sv = 0, None
        if b == "f":
            fv[0] = float(v)
        elif b in ("v2", "v3", "r", "q", "c"):
            for j in range(4):
                fv[j] = float(v[j]) if j < len(v) else 0.0
        elif b in ("i", "e", "b"):
            iv = int(v) if v is not None else 0
        elif b == "go":
            iv = sim.go(v)
        elif alert:
            iv = sim.go(v[1])
        elif b == "s":
            sv = v.encode() if v is not None else None
        sim.var_set(sim.w, -1 if glob else fsm, BUCKET[b], name.encode(), fv, iv, sv)


COMPOSE_TOL = 1e-4   # a world position further off than this after the local chain is set is a missing input, not rounding


def set_gos(sim, gos, skip_world):
    """Parent, local pose, velocity, active flags, animator, HealthManager and LimitSendEvents of each recorded
    GameObject; the parent chain's local poses first, root first.  Returns the first touched GameObject whose world
    position then reads back far from the game's (the recorded inputs are not reproduced), or None.  GameObjects
    whose world (or local) position reads back off by rounding only are added to `skip_world` as (path, "p" | "lp"):
    the sim composes the parent chain (and a Rigidbody2D's local position from its world one) in other float steps
    than Unity, so that position is not an output of the call."""
    nodes = {}                                  # path -> (local pos, local scale, local euler z, parent path, depth)
    for path, d in gos.items():
        anc = d.get("anc", [])
        for k, a in enumerate(anc):
            nodes.setdefault(a[0], (a[1], a[2], a[3], anc[k - 1][0] if k else None, k))
    for path, d in gos.items():
        nodes[path] = (d["lp"], d["ls"], d["lz"], d.get("par"), len(d.get("anc", [])))
    fp = (C.c_float * 3)(); fs = (C.c_float * 3)(); fvel = (C.c_float * 2)()
    for path in sorted(nodes, key=lambda p: nodes[p][4]):
        g = sim.go(path)
        if g < 0:
            continue
        lp, ls, lz, par, _ = nodes[path]
        pg = sim.go(par) if par is not None else -1
        if (par is None or pg >= 0) and sim.go_parent(sim.w, g) != pg:
            if sim.go_set_parent(sim.w, g, pg) != 0:
                raise TrapError("go_set_parent %s: %s" % (path, sim.errmsg()))
        for j in range(3):
            fp[j] = lp[j]; fs[j] = ls[j]
        if sim.go_set_local(sim.w, g, fp, fs, float(lz)) != 0:
            raise TrapError("go_set_local %s: %s" % (path, sim.errmsg()))
    out = (C.c_float * 19)()
    bad = None
    for path, d in gos.items():
        g = sim.go(path)
        if g < 0:
            if bad is None:   # an object the call reads (owner or a field's) that the sim does not have
                bad = ("go[%s] (input)" % path, "an object in the game", "no such sim GameObject")
            continue
        v = None
        if "v" in d:
            fvel[0], fvel[1] = d["v"]; v = fvel
        if sim.go_set(sim.w, g, None, None, 0.0, v, int(d["a"]), int(d["h"])) != 0:
            raise TrapError("go_set %s: %s" % (path, sim.errmsg()))
        if "g" in d:
            sim.go_set_gravity(sim.w, g, float(d["g"]))
        an = d.get("an")
        if an is not None:
            sim.anim_set(sim.w, g, an["clip"].encode() if an.get("clip") else None, float(an["clipTime"]), float(an["clipFps"]),
                         int(an["previousFrame"]), int(an["state"]), int(an.get("sprite", -1)))
        hm = d.get("hm")
        if hm is not None:
            sim.hm_set(sim.w, g, (C.c_float * 6)(*[float(hm[k]) for k in HM_FIELDS]))
        if "ar" in d:
            sim.alert_set(sim.w, g, int(d["ar"]))
        if "lse" in d:
            ids = [sim.go(x) for x in d["lse"]]
            sim.lse_set(sim.w, g, (C.c_int32 * max(1, len(ids)))(*ids), len(ids))
            if bad is None and len(set(d["lse"])) < len(d["lse"]):   # pool clones: one canonical path, one sim clone
                bad = ("go[%s].lse (input)" % path, d["lse"], "clones that share a path are one sim object")
        if sim.go_get(sim.w, g, out) != 0:
            raise TrapError("go_get %s: %s" % (path, sim.errmsg()))
        if not out[16]:
            continue
        for key, lo, name in (("p", 0, "position"), ("lp", 3, "localPosition")):
            if vec_eq(d[key], out[lo:lo + 3], 3):
                continue
            if max(abs(float(d[key][k]) - out[lo + k]) for k in range(3)) > COMPOSE_TOL:
                if bad is None:
                    bad = ("go[%s].%s (input)" % (path, name), d[key], list(out[lo:lo + 3]))
            else:
                skip_world.add((path, key))
    return bad


def activate_owner(sim, owner, gos):
    """The owner active in the game but not in the reset world: SetActive(true) through the lifecycle, parents first,
    so its components are enabled as in the game (an OnDisable the call raises then happens)."""
    d = gos.get(owner)
    if d is None or not int(d["h"]):
        return
    out = (C.c_float * 19)()
    for path in [a[0] for a in d.get("anc", [])] + [owner]:
        g = sim.go(path)
        if g < 0:
            continue
        sim.go_get(sim.w, g, out)
        if not out[14] and sim.go_activate(sim.w, g, 1) != 0:
            raise TrapError("SetActive(true) %s: %s" % (path, sim.errmsg()))


PD_KIND = {"f": 0, "i": 1, "b": 2}


def set_pd(sim, ep, delta, prev_keys):
    """PlayerData: the record's non-baseline fields, and the baseline value back for fields a previous record of this
    activation set.  Returns the keys set."""
    for k in prev_keys:
        if k not in delta and k in ep.pd_base:
            _pd_write(sim, ep, k, ep.pd_base[k])
    for k, v in delta.items():
        _pd_write(sim, ep, k, v)
    return set(delta)


def _pd_write(sim, ep, k, v):
    kind = PD_KIND.get(ep.pd_type.get(k, ""), None)
    if kind is None:
        return
    sim.pd_set(sim.w, k.encode(), kind, float(v) if kind == 0 else 0.0, int(v) if kind else 0)


def set_common(sim, r):
    phase = 0 if r.get("fx") else 1
    if r.get("cb") == "F":
        phase = 0
    elif r.get("cb") == "L":
        phase = 2
    sim.set_clock(sim.w, float(r["dt"]), float(r["fdt"]), float(r["tm"]), int(r["f"]), phase)
    sim.set_rng(sim.w, (C.c_uint32 * 4)(*r["r0"]))
    if "in" in r:
        sim.input_set(sim.w, (C.c_int32 * len(r["in"]))(*r["in"]), len(r["in"]))


# ------------------------------------------------------------------ outputs

def compare_gos(sim, gos_before, gos_after, diffs, skip_world=(), typ=None):
    out = (C.c_float * 19)()
    fa = (C.c_float * 6)()
    for path, d in gos_after.items():
        g = sim.go(path)
        if g < 0 or path not in gos_before:
            continue    # an object the call only named on its way out (a stored GetParent / FindChild result) was no input
        if sim.go_get(sim.w, g, out) != 0:
            raise TrapError("go_get %s: %s" % (path, sim.errmsg()))
        if out[16]:
            if (path, "p") not in skip_world and not vec_eq(d["p"], out[0:3], 3):
                diffs.append(("go[%s].position" % path, d["p"], list(out[0:3])))
            if (path, "lp") not in skip_world and not vec_eq(d["lp"], out[3:6], 3):
                diffs.append(("go[%s].localPosition" % path, d["lp"], list(out[3:6])))
            if not vec_eq(d["ls"], out[6:9], 3):
                diffs.append(("go[%s].localScale" % path, d["ls"], list(out[6:9])))
            if not angle_eq(d["lz"], out[9], typ):
                diffs.append(("go[%s].localEulerZ" % path, d["lz"], out[9]))
        if "v" in d and out[13] and not vec_eq(d["v"], out[11:13], 2):
            diffs.append(("go[%s].velocity" % path, d["v"], list(out[11:13])))
        if "g" in d and out[13] and not feq(float(d["g"]), out[17]):
            diffs.append(("go[%s].gravityScale" % path, d["g"], out[17]))
        if int(d["a"]) != int(out[14]):
            diffs.append(("go[%s].activeSelf" % path, d["a"], int(out[14])))
        an = d.get("an")
        if an is not None:
            compare_anim(sim, g, an, diffs, "go[%s].anim." % path)
        hm = d.get("hm")
        if hm is not None and sim.hm_get(sim.w, g, fa) == 0:
            for k, name in enumerate(HM_FIELDS):
                if not feq(float(hm[name]), fa[k]):
                    diffs.append(("go[%s].hm.%s" % (path, name), hm[name], fa[k]))
                    break


def compare_anim(sim, g, an, diffs, pre):
    fa = (C.c_float * 6)()
    clip = sim.anim_get(sim.w, g, fa)
    if clip is None:
        return
    clip = clip.decode()
    if (an.get("clip") or "") != clip:
        diffs.append((pre + "clip", an.get("clip"), clip))
        return
    for k, name in ((0, "clipTime"), (1, "clipFps"), (2, "previousFrame"), (3, "state")):
        if not feq(float(an[name]), fa[k]):
            diffs.append((pre + name, an[name], fa[k]))
            return


def compare_pd(sim, ep, before, after, diffs):
    f, i = C.c_float(), C.c_int32()
    for k in set(before) | set(after):
        kind = PD_KIND.get(ep.pd_type.get(k, ""), None)
        if kind is None:
            continue
        gv = after.get(k, ep.pd_base.get(k))
        if sim.pd_get(sim.w, k.encode(), kind, C.byref(f), C.byref(i)) != 0:
            continue
        sv = f.value if kind == 0 else i.value
        if (not feq(gv, sv)) if kind == 0 else (int(gv) != int(sv)):
            diffs.append(("PlayerData.%s" % k, gv, sv))


def hero_keys(hc):
    """(sim key, game value) of every HeroController field and cState flag the record carries."""
    for name, v in hc.get("f", {}).items():
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            yield "hero.f." + name, v
    for name, v in hc.get("cs", {}).items():
        yield "hero.cstate." + name, v


def set_hero(sim, hc):
    """The HeroController's fields and cState as the game had them (the records of MethodRecorder.cs kHeroTypes);
    returns the keys the sim's hero has."""
    known = set()
    for key, v in hero_keys(hc):
        if sim.set_value(sim.s, key.encode(), float(v)) == 0:
            known.add(key)
    return known


def compare_hero(sim, hca, known, diffs):
    out = C.c_double()
    for key, v in hero_keys(hca):
        if key not in known or sim.get_value(sim.s, key.encode(), C.byref(out)) != 0:
            continue
        if not feq(float(v), out.value):   # an int or bool field reads back exactly as a double
            diffs.append((key, v, out.value))


def other_fsm(sim, vx):
    """(sim FSM index, its variable keys) of the FSM a GetFsm* / SetFsm* record addresses (`vx`), or None."""
    if not vx:
        return None
    g = sim.go(vx["o"])
    f = sim.find_fsm(sim.w, g, vx["n"].encode()) if g >= 0 else -1
    return (f, set(sim.vars(f).keys())) if f >= 0 else None


def unrecorded_hit_responder(sim, gos):
    """HitTaker.Hit (HitTaker.cs:7-22) also hits the HealthManagers of the target's next two ancestors.  A recording
    before format 3 does not carry them: an ancestor's HealthManager state is then an input the replay lacks."""
    fa = (C.c_float * 6)()
    for path, d in gos.items():
        for a in d.get("anc", [])[-2:]:
            if a[0] in gos:
                continue
            g = sim.go(a[0])
            if g >= 0 and sim.hm_get(sim.w, g, fa) == 0:
                return ("go[%s].hm (input)" % a[0], "a HealthManager the record does not carry", "the reset world's")
    return None


def live_fsm(sim, owner, name):
    """The sim has this FSM (an event to any other one has no sim counterpart to compare)."""
    key = (owner, name)
    v = sim._live_cache.get(key)
    if v is None:
        g = sim.go(owner)
        f = sim.find_fsm(sim.w, g, name.encode()) if g >= 0 else -1
        v = sim._live_cache[key] = f >= 0
    return v


def events_since(sim, mark, fsm, depth):
    """The events the call sent itself: logged since `mark` at the FsmExecutionStack depth it ran at (the recorder
    attributes events the same way), to its own FSM (`fsm`) or to any (None)."""
    out = []
    fi = C.c_int32(); nm = C.c_char_p(); dp = C.c_int32()
    for i in range(mark, sim.log_count(sim.w)):
        if sim.log_event(sim.w, i, C.byref(fi), C.byref(nm), C.byref(dp)) and dp.value == depth and (fsm is None or fi.value == fsm):
            out.append(nm.value.decode())
    return out


def compare_vars(sim, fsm, after, local_keys, diffs, globals_=True):
    """`globals_` False: a call that cascaded is compared on its own FSM; the globals other FSMs wrote in the cascade
    are theirs."""
    loc = sim.vars(fsm)
    glob = None
    for key, v in after.items():
        is_g = key.startswith("G")
        if is_g and not globals_:
            continue
        k = key[1:] if is_g else key
        b = k.split(":", 1)[0]
        if b in SKIP_BUCKETS:
            continue
        if is_g:
            if glob is None:
                glob = sim.vars(-1)
            sv = glob.get(k)
        else:
            sv = loc.get(k)
        if sv is None:
            continue
        gv = game_val_key(b, v, sim)
        if gv is None or gv == sv:
            continue
        width = {"f": 1, "v2": 2, "v3": 3}.get(b, 4)   # the sim's fsm_val holds four floats; a vector uses its first ones
        if b in ("f", "v2", "v3", "r", "q", "c") and all(feq(bits_f(int(x, 16)), bits_f(int(y, 16)))
                                                          for x, y in list(zip(gv.split(","), sv.split(",")))[:width]):
            continue
        if b == "go" and v is not None and sim.go(v) < 0:
            continue    # the game's object has no sim counterpart: an identity gap, not this call's output
        if b == "go" and v is not None and sim.gpath(int(sv)) == v:
            continue    # another pool clone of the same canonical path: the record cannot tell clones apart
        diffs.append(("var %s" % key, v, show_sim_val(b, sv, sim)))


def verdict(diffs, bad_input, all_diffs=False):
    if all_diffs and diffs:
        v = verdict(diffs, bad_input)
        v.field, v.game = "*", ([bad_input] if bad_input else []) + diffs
        return v
    if not diffs:
        return Outcome("exact")
    if bad_input is not None:
        f, g, s = bad_input
        return Outcome("input", f, g, s, note="then %s differs" % diffs[0][0])
    if all(d[0] == "rng state" for d in diffs):
        f, g, s = diffs[0]
        return Outcome("rng-only", f, g, s)
    real = [d for d in diffs if d[0] != "rng state"]
    f, g, s = real[0]
    return Outcome("mismatch", f, g, s, note="%d fields" % len(diffs))


class Outcome:
    __slots__ = ("verdict", "field", "game", "sim", "note")

    def __init__(self, verdict, field=None, game=None, sim=None, note=None):
        self.verdict, self.field, self.game, self.sim, self.note = verdict, field, game, sim, note


# ------------------------------------------------------------------ one activation

class Carry:
    """An action object's private state between the activations the replay runs of one site (the game keeps an
    action's private fields from one activation to the next; IdleBuzzV3's accelX / waitTime, for instance): the
    sim's st block after the last one, and the game's private fields then."""
    def __init__(self):
        self.sites = {}


def replay_activation(sim, ep, recs, all_diffs=False, carry=None):
    """[(record, Outcome)] for one activation, in callback order.  With `carry`, the sim's private state of the action
    continues from the previous activation replayed at the same site; when the game's private fields at this OnEnter
    are not the ones that activation ended with, an activation nobody recorded ran in between, and a mismatch is an
    `input` gap (the carried state is unknown)."""
    r0 = recs[0]
    if r0["cb"] != "E":
        return [(r, Outcome("identity", note="activation has no OnEnter record")) for r in recs]
    sim.restore()
    go = sim.go(r0["o"])
    if go < 0:
        return [(r, Outcome("identity", note="no GameObject %s" % r0["o"])) for r in recs]
    fsm = sim.find_fsm(sim.w, go, r0["n"].encode())
    if fsm < 0:
        return [(r, Outcome("identity", note="no FSM %s on %s" % (r0["n"], r0["o"]))) for r in recs]
    st = sim.state_index(sim.w, fsm, r0["s"].encode())
    if st < 0:
        return [(r, Outcome("identity", note="no state %s in %s/%s" % (r0["s"], r0["o"], r0["n"]))) for r in recs]
    ported = C.c_int32()
    typ = sim.action_type(sim.w, fsm, st, int(r0["i"]), C.byref(ported)).decode()
    if typ != r0["t"]:
        return [(r, Outcome("identity", note="action %d of %s is %s in the sim" % (r0["i"], r0["s"], typ or "missing"))) for r in recs]
    if not ported.value:
        return [(r, Outcome("unported")) for r in recs]
    full_var_maps(recs)
    local_keys = set(sim.vars(fsm).keys())
    out = []
    try:
        activate_owner(sim, r0["o"], r0["gb"])
    except TrapError as e:
        return [(r, Outcome("trap", note=str(e))) for r in recs]
    site = (r0["o"], r0["n"], r0["s"], r0["i"])
    carried_ok = True
    if carry is not None and site in carry.sites:
        blob, last_pa = carry.sites[site]
        sim.act_state_set(sim.w, fsm, st, int(r0["i"]), blob, len(blob))
        carried_ok = last_pa == r0.get("pb", {})
    call_out = (C.c_int32 * 3)()
    pd_keys = set()
    for r in recs:
        try:
            sim.reserve(sim.w)
            set_common(sim, r)
            set_vars(sim, fsm, r["_vb"], local_keys)
            pd_keys = set_pd(sim, ep, r.get("pd", {}), pd_keys)
            skip_world = set()
            bad_input = set_gos(sim, r["gb"], skip_world)
            stats_compose.update(skip_world)
            if bad_input is None and r["t"] == "TakeDamage":
                bad_input = unrecorded_hit_responder(sim, r["gb"])
            hero_known = set_hero(sim, r["hc"]) if "hc" in r else None
            other = other_fsm(sim, r.get("vx"))
            if other is not None:
                set_vars(sim, other[0], r["vx"]["v"], other[1])
            elif bad_input is None and r["t"].startswith(("GetFsm", "SetFsm")) and ep.version < 3:
                bad_input = ("other FSM's variables (input)", "not recorded before format 3", "the reset world's")
            if bad_input is None and ep.version < 3 and r["t"] in HERO_TYPES and (r["o"] + "/").startswith(ep.knight + "/"):
                bad_input = ("HeroController (input)", "fields a record before format 3 does not carry", "the reset world's")
            if bad_input is None and ep.version < 3 and r["t"] == "CheckAlertRange":
                bad_input = ("AlertRange (input)", "the latch and its object a record before format 3 does not carry", "the reset world's")
            sw0 = sim.state_index(sim.w, fsm, r["sw0"].encode()) if r.get("sw0") else -1
            ps0 = sim.state_index(sim.w, fsm, r["ps0"].encode()) if r.get("ps0") else -1
            sim.prepare(sim.w, fsm, st, float(r["st"]), sw0, ps0)
            evd = r.get("evd")
            if evd:
                g = sim.go(evd[0]) if evd[0] is not None else -1
                sender = sim.find_fsm(sim.w, g, evd[1].encode()) if g >= 0 and evd[1] is not None else -1
                sim.set_event_data(sim.w, sender, int(evd[2]), float(evd[3]), evd[4].encode() if evd[4] is not None else None)
            mark = sim.log_mark(sim.w)
            nd0 = sim.delayed_count(sim.w, fsm)
            rc = sim.call(sim.w, fsm, st, int(r["i"]), CB[r["cb"]], r.get("e", "").encode(), int(r.get("fin0", 0)), call_out)
            if rc == -1:
                out.append((r, Outcome("unported", note=sim.errmsg())))
                break
            if rc != 0:
                out.append((r, Outcome("trap", note=sim.errmsg())))
                break
            cascade = int(r.get("nd", 0)) > 0
            diffs = []
            if int(r["fin"]) != call_out[0]:
                diffs.append(("Finished", r["fin"], call_out[0]))
            if r["cb"] == "V" and "ret" in r and int(r["ret"]) != call_out[1]:
                diffs.append(("Event() return", r["ret"], call_out[1]))
            sw = sim.switch_to(sim.w, fsm).decode()
            if (r.get("sw") or "") != sw:
                diffs.append(("pending transition", r.get("sw"), sw))
            game_ev = [e[0] for e in r.get("evl", [])]
            sim_ev = events_since(sim, mark, fsm, call_out[2])
            if game_ev != sim_ev:
                diffs.append(("events sent", game_ev, sim_ev))
            nd1 = sim.delayed_count(sim.w, fsm)
            dl = C.c_float()
            sim_dly = [(sim.delayed_get(sim.w, fsm, k, C.byref(dl)).decode(), fbits(dl.value)) for k in range(nd0, nd1)]
            game_dly = [(d[0], fbits(d[-1])) for d in r.get("dly", [])]
            if game_dly != sim_dly:
                diffs.append(("delayed events", game_dly, sim_dly))
            compare_vars(sim, fsm, r["_va"], local_keys, diffs, globals_=not cascade)
            if not cascade:
                compare_pd(sim, ep, r.get("pd", {}), r.get("pda", {}), diffs)
                compare_gos(sim, r["gb"], r["ga"], diffs, skip_world, r["t"])
                if other is not None and "vxa" in r:
                    compare_vars(sim, other[0], r["vxa"]["v"], other[1], diffs)
                if hero_known is not None and "hca" in r:
                    compare_hero(sim, r["hca"], hero_known, diffs)
                if sim.rng() != list(r["r1"]):
                    diffs.append(("rng state", r["r1"], sim.rng()))
            if bad_input is None and not carried_ok:
                bad_input = ("private state (input)", r0.get("pb"), "carried from an activation that was not recorded")
            if bad_input is None and r["t"] in SPAWN_TYPES and diffs and all("(Clone)" in d[0] for d in diffs if d[0].startswith("go["))                     and any(d[0].startswith("go[") for d in diffs):
                bad_input = ("pool clone (input)", "a clone the record names by its shared path", "the first sim clone of that path")
            out.append((r, verdict(diffs, bad_input, all_diffs)))
        except TrapError as e:
            out.append((r, Outcome("trap", note=str(e))))
            break
    if carry is not None and out and out[-1][1].verdict != "trap":
        n = sim.act_state_get(sim.w, fsm, st, int(r0["i"]), None, 0)
        buf = C.create_string_buffer(max(n, 1))
        sim.act_state_get(sim.w, fsm, st, int(r0["i"]), buf, n)
        carry.sites[site] = (buf.raw[:n], recs[len(out) - 1].get("pa", {}))
    return out


# ------------------------------------------------------------------ components

def replay_component(sim, ep, r, all_diffs=False):
    sim.restore()
    go = sim.go(r["o"])
    if go < 0:
        return Outcome("identity", note="no GameObject %s" % r["o"])
    try:
        set_common(sim, r)
        set_pd(sim, ep, r.get("pd", {}), set())
        activate_owner(sim, r["o"], r["gb"])
        skip_world = set()
        bad_input = set_gos(sim, r["gb"], skip_world)
        stats_compose.update(skip_world)
        a, sb, t = r["args"], r["sb"], r["t"]
        mark = sim.log_mark(sim.w)
        if t == "HealthManager.Hit":
            sim.hm_set(sim.w, go, (C.c_float * 6)(*[float(sb[k]) for k in HM_FIELDS]))
            rc = sim.hm_hit(sim.w, go, sim.go(a["src"]), a["type"], a["dmg"], a["dir"], a["circ"], a["ign"], a["magm"], a["mul"])
        elif t.startswith("Recoil."):
            state = sb["state"] if isinstance(sb["state"], int) else RECOIL_STATES.get(sb["state"], 0)
            if sim.recoil_set(sim.w, go, (C.c_float * 5)(state, sb["recoilTimeRemaining"], sb["recoilSpeed"], sb["isRecoilSweeping"], sb["recoilSpeedBase"])) != 0:
                return Outcome("identity", note="no Recoil in the sim on %s" % r["o"])
            rc = sim.recoil_by_direction(sim.w, go, a["dir"], a["mag"]) if t == "Recoil.RecoilByDirection" else sim.recoil_fixed_update(sim.w, go)
        elif t == "tk2dSpriteAnimator.UpdateAnimation":
            sim.anim_set(sim.w, go, sb["clip"].encode() if sb.get("clip") else None, float(sb["clipTime"]), float(sb["clipFps"]),
                         int(sb["previousFrame"]), int(sb["state"]), int(sb.get("sprite", -1)))
            rc = sim.anim_update(sim.w, go, a["dt"])
        elif t == "tk2dSpriteAnimator.Play":
            if a.get("clip") is None:
                return Outcome("identity", note="Play(null clip)")
            sim.anim_set(sim.w, go, sb["clip"].encode() if sb.get("clip") else None, float(sb["clipTime"]), float(sb["clipFps"]),
                         int(sb["previousFrame"]), int(sb["state"]), int(sb.get("sprite", -1)))
            rc = sim.anim_play(sim.w, go, a["clip"].encode(), a["t"], a["fps"])
        else:
            return Outcome("unported", note=t)
        if rc == -1:
            return Outcome("identity", note=sim.errmsg())
        if rc != 0:
            return Outcome("trap", note=sim.errmsg())
        cascade = int(r.get("nd", 0)) > 0
        diffs = []
        game_ev = [e[0] for e in r.get("evl", []) if live_fsm(sim, e[-2], e[-1])]
        sim_ev = events_since(sim, mark, None, 0)
        if game_ev != sim_ev:
            diffs.append(("events sent", game_ev, sim_ev))
        sa = r["sa"]
        fa = (C.c_float * 6)()
        if t == "HealthManager.Hit" and sim.hm_get(sim.w, go, fa) == 0:
            for k, name in enumerate(HM_FIELDS):
                if not feq(float(sa[name]), fa[k]):
                    diffs.append(("hm.%s" % name, sa[name], fa[k]))
        if t.startswith("Recoil.") and sim.recoil_get(sim.w, go, fa) == 0:
            state = sa["state"] if isinstance(sa["state"], int) else RECOIL_STATES.get(sa["state"], 0)
            for k, (name, gv) in enumerate((("state", state), ("recoilTimeRemaining", sa["recoilTimeRemaining"]),
                                            ("recoilSpeed", sa["recoilSpeed"]), ("isRecoilSweeping", sa["isRecoilSweeping"]))):
                if not feq(float(gv), fa[k]):
                    diffs.append(("recoil.%s" % name, gv, fa[k]))
        if t.startswith("tk2dSpriteAnimator."):
            compare_anim(sim, go, sa, diffs, "anim.")
        if not cascade:
            gos = {k: v for k, v in r["ga"].items() if k == r["o"]}
            compare_gos(sim, r["gb"], gos, diffs, skip_world, t)
            if sim.rng() != list(r["r1"]):
                diffs.append(("rng state", r["r1"], sim.rng()))
        return verdict(diffs, bad_input, all_diffs)
    except TrapError as e:
        return Outcome("trap", note=str(e))


# ------------------------------------------------------------------ driver

def recording_files(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            for dp, _, fs in os.walk(p):
                out += [os.path.join(dp, f) for f in fs if f.endswith(".methods.jsonl.gz")]
        else:
            out.append(p)
    return sorted(out)


def replay_file(path, dll=None, limit=None):
    """({type: counts}, {type: {verdict: first example}}, {object|position: rounding-only input readbacks}, [a row per
    mismatch or trap, and per site one identity / unported]) for one recording."""
    sim = Sim(dll)
    stats = collections.defaultdict(collections.Counter)
    examples = collections.defaultdict(dict)
    rows = []
    gap_sites = set()

    def note(r, o, level):
        key = r["t"]
        site = (r["o"], r.get("n"), r.get("s"), r.get("i"), o.verdict)
        if o.verdict in ("mismatch", "trap") or (o.verdict in ("identity", "unported") and site not in gap_sites):
            gap_sites.add(site)
            rows.append([key, o.verdict, os.path.basename(path), r["q"], r["o"], r.get("n"), r.get("s"), r.get("i"), r.get("cb"),
                         o.field or o.note, _short(o.game, 120), _short(o.sim, 120)])
        stats[key]["calls"] += 1
        stats[key][o.verdict] += 1
        if int(r.get("nd", 0)) > 0:
            stats[key]["cascade"] += 1
        if o.verdict == "mismatch":
            stats[key]["field:" + o.field.split("[")[0]] += 1
        if o.verdict not in examples[key]:
            examples[key][o.verdict] = example(path, level, r, o)

    for ep in episodes(path):
        seed = int(ep.head.get("seed") or 0) if ep.head else 0
        sim.open(ep.level, seed)
        sim.set_pd_base(ep)
        acts = collections.defaultdict(list)
        comps = []
        for r in ep.recs:
            if r["k"] == "pm":
                acts[r["a"]].append(r)
            else:
                comps.append(r)
        carry = Carry()
        for n, aid in enumerate(sorted(acts)):   # activation ids are issued in the game's order
            if limit and n >= limit:
                break
            rs = sorted(acts[aid], key=lambda r: r["j"])
            for r, o in replay_activation(sim, ep, rs, carry=carry):
                note(r, o, ep.level)
        for r in comps:
            note(r, replay_component(sim, ep, r), ep.level)
    compose = {"%s|%s" % k: n for k, n in stats_compose.items()}
    stats_compose.clear()
    for k, n in stats_readback.items():
        stats[k]["readback"] += n
    stats_readback.clear()
    return {k: dict(v) for k, v in stats.items()}, dict(examples), compose, rows


def example(path, level, r, o):
    return {"file": os.path.basename(path), "level": level, "q": r["q"], "owner": r["o"], "fsm": r.get("n"), "state": r.get("s"),
            "index": r.get("i"), "cb": r.get("cb"), "j": r.get("j"), "field": o.field, "game": o.game, "sim": o.sim, "note": o.note}


def _worker(args):
    path, dll, limit = args
    t0 = time.time()
    try:
        s, e, c, rows = replay_file(path, dll, limit)
        return path, s, e, None, time.time() - t0, c, rows
    except Exception as ex:   # a crashed file is reported, not fatal to the table
        import traceback
        return path, {}, {}, "%s: %s\n%s" % (type(ex).__name__, ex, traceback.format_exc()[-1500:]), time.time() - t0, {}, []


def merge(results):
    stats = collections.defaultdict(collections.Counter)
    examples = collections.defaultdict(dict)
    for _, s, e, _, _, _, _ in results:
        for k, v in s.items():
            stats[k].update(v)
        for k, v in e.items():
            for vd, ex in v.items():
                examples[k].setdefault(vd, ex)
    return stats, examples


def cmd_replay(a):
    files = recording_files(a.paths)
    if not files:
        print("replay: no recordings under %s" % " ".join(a.paths)); return 2
    jobs = [(f, a.dll, a.limit) for f in files]
    results = []

    def done(res):
        results.append(res)
        print("replay: %s %s (%.0fs)" % (os.path.basename(res[0]), "FAILED " + res[3].splitlines()[0] if res[3] else "ok", res[4]), flush=True)
    if a.jobs > 1:
        import multiprocessing as mp
        with mp.Pool(a.jobs) as pool:
            for res in pool.imap_unordered(_worker, jobs):
                done(res)
    else:
        for j in jobs:
            done(_worker(j))
    stats, examples = merge(results)
    compose = collections.Counter()
    for r in results:
        compose.update(r[5])
    registry = Sim(a.dll).registry()
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            json.dump({"files": files, "stats": stats, "examples": examples, "registry": registry, "compose": dict(compose),
                       "errors": {r[0]: r[3] for r in results if r[3]}}, f, indent=1, default=str)
    if a.calls:
        with open(a.calls, "w", encoding="utf-8") as f:
            f.write("\t".join(("type", "verdict", "file", "q", "owner", "fsm", "state", "index", "cb", "field", "game", "sim")) + "\n")
            for r in results:
                for row in r[6]:
                    f.write("\t".join(str(x).replace("\t", " ") for x in row) + "\n")
    print(table(stats, examples, registry))
    print("compose: %d input positions read back off by rounding only after their local chain was set, on %d objects%s"
          % (sum(compose.values()), len(compose), ("; most: " + ", ".join("%s x%d" % kv for kv in compose.most_common(4))) if compose else ""))
    return 0


def table(stats, examples, registry):
    rows = sorted(stats.items(), key=lambda kv: (-kv[1].get("mismatch", 0), -kv[1].get("trap", 0), kv[0]))
    L = ["%-36s %6s %6s %5s %5s %5s %5s %5s %5s %5s  first mismatch / trap" % ("type", "calls", "exact", "mism", "rng", "input", "trap", "ident", "unpor", "rdbk")]
    for t, c in rows:
        ex = examples.get(t, {})
        e = ex.get("mismatch") or ex.get("trap") or {}
        first = ""
        if e:
            first = "%s: game=%s sim=%s [%s %s|%s|%s|%s %s]" % (e.get("field") or e.get("note"), _short(e.get("game")), _short(e.get("sim")),
                                                           e.get("file"), e.get("owner"), e.get("fsm"), e.get("state"), e.get("index"), e.get("cb"))
        L.append("%-36s %6d %6d %5d %5d %5d %5d %5d %5d %5d  %s" % (t, c.get("calls", 0), c.get("exact", 0), c.get("mismatch", 0), c.get("rng-only", 0),
                                                                    c.get("input", 0), c.get("trap", 0), c.get("identity", 0), c.get("unported", 0),
                                                                    c.get("readback", 0), first))
    called = set(stats)
    compared = {t for t, c in stats.items() if c.get("exact", 0) + c.get("mismatch", 0) + c.get("rng-only", 0) > 0}
    never = [t for t in registry if t not in called]
    uncompared = [t for t in registry if t in called and t not in compared]
    tot = collections.Counter()
    for c in stats.values():
        tot.update(c)
    L.append("TOTAL calls=%d %s" % (tot["calls"], " ".join("%s=%d" % (v, tot[v]) for v in VERDICTS)))
    L.append("registered action types: %d; called in the game: %d; compared (exact/mismatch/rng-only) %d; never called: %d; called but never compared: %d"
             % (len(registry), len([t for t in registry if t in called]), len([t for t in registry if t in compared]), len(never), len(uncompared)))
    L.append("never called: " + " ".join(never))
    L.append("called, never compared: " + " ".join(uncompared))
    return "\n".join(L)


def _short(v, n=80):
    s = json.dumps(v, default=str, ensure_ascii=False)
    return s if len(s) <= n else s[:n] + "..."


# ------------------------------------------------------------------ recorder checks

def cmd_selfcheck(a):
    """Known answers the recorder must reproduce: a SetFloatValue / SetBoolValue / SetIntValue with a LITERAL value
    leaves exactly that value in its variable after OnEnter (ACT/SetFloatValue.cs:23-30); the FSM dump gives the
    literal.  Can fail: a recorder that snapshotted the wrong FSM, or the before state as the after state, disagrees
    with the dump."""
    files = recording_files(a.paths)
    lits = {}
    ok = bad = 0
    first_bad = None
    for path in files:
        for ep in episodes(path):
            if ep.level not in lits:
                lits[ep.level] = load_fsm_literals(ep.level)
            lit = lits[ep.level]
            acts = collections.defaultdict(list)
            for r in ep.recs:
                if r["k"] == "pm":
                    acts[r["a"]].append(r)
            for rs in acts.values():
                rs.sort(key=lambda r: r["j"])
                full_var_maps(rs)
                r = rs[0]
                if r["cb"] != "E" or r["t"] not in LITERAL_SETTERS or int(r.get("nd", 0)) > 0:
                    continue
                key = (r["o"], r["n"], r["s"], r["i"])
                if key not in lit:
                    continue
                var, val = lit[key]
                if var not in r["_va"]:
                    continue
                got = r["_va"][var]
                same = fbits(got) == fbits(val) if var.startswith("f:") else int(got) == int(val)
                ok += same
                bad += not same
                if not same and first_bad is None:
                    first_bad = (os.path.basename(path), key, var, val, got)
    good = bad == 0 and ok > 0
    print("selfcheck: %s (%d literal Set*Value calls agree with the FSM dump, %d disagree)%s"
          % ("OK" if good else "FAIL", ok, bad, "" if not first_bad else " first: %s" % (first_bad,)))
    return 0 if good else 1


LITERAL_SETTERS = {"SetFloatValue": ("floatVariable", "floatValue", "f:"), "SetBoolValue": ("boolVariable", "boolValue", "b:"),
                   "SetIntValue": ("intVariable", "intValue", "i:")}


def load_fsm_literals(level):
    """(owner, fsm, state, index) -> (var key, literal) for Set{Float,Bool,Int}Value with a literal value, from
    analysis/fsm/<level>.json."""
    path = os.path.join(ROOT, "analysis", "fsm", level + ".json")
    out = {}
    if not os.path.exists(path):
        return out
    d = json.load(open(path, encoding="utf-8"))
    for f in d.get("fsms", []):
        owner = canon(f.get("goPath") or f.get("path") or "")
        for st in f.get("states", []):
            for i, act in enumerate(st.get("actions", [])):
                t = (act.get("type") or "").rsplit(".", 1)[-1]
                if t not in LITERAL_SETTERS:
                    continue
                vname, lname, pre = LITERAL_SETTERS[t]
                fields = {x.get("name"): x for x in act.get("fields", [])}
                var, lit = fields.get(vname), fields.get(lname)
                if not var or not lit:
                    continue
                vv, lv = var.get("value", {}), lit.get("value", {})
                if not isinstance(vv, dict) or not isinstance(lv, dict) or lv.get("useVariable") or not vv.get("name"):
                    continue
                x = lv.get("value")
                if x is None:
                    continue
                out[(owner, f.get("name") or f.get("fsmName"), st.get("name"), i)] = (pre + vv["name"], int(x) if isinstance(x, bool) else x)
    return out


def pd_diffs(sim, ep):
    """[(field, game, sim)] of the PlayerData int / bool / float fields where the sim's reset world differs from the
    game's store at SceneReady."""
    f, i = C.c_float(), C.c_int32()
    out = []
    for k, gv in sorted(ep.pd_base.items()):
        kind = PD_KIND.get(ep.pd_type.get(k, ""), None)
        if kind is None or sim.pd_get(sim.w, k.encode(), kind, C.byref(f), C.byref(i)) != 0:
            continue
        sv = f.value if kind == 0 else i.value
        if (not feq(gv, sv)) if kind == 0 else (int(gv) != int(sv)):
            out.append((k, gv, sv))
    return out


def cmd_pd(a):
    """Per scene, the PlayerData fields whose value in the sim's reset world is not the game's at SceneReady (the
    replay writes the game's store before replaying, so these are divergences of the sim's scene load, not of a
    call)."""
    sim = Sim(a.dll)
    seen = set()
    for path in recording_files(a.paths):
        head = ep = None
        for r in read_records(path):   # the first episode's marker is enough
            if "methods" in r:
                head = r
            elif r.get("ev") == "scene_ready":
                ep = Episode(level_key(path, r["level"]), head, r)
                break
        if ep is not None and ep.level not in seen:
            seen.add(ep.level)
            sim.open(ep.level, int(ep.head.get("seed") or 0) if ep.head else 0)
            d = pd_diffs(sim, ep)
            print("pd %s: %d of %d fields differ%s" % (ep.level, len(d), len(ep.pd_base),
                                                      (": " + ", ".join("%s game=%s sim=%s" % x for x in d)) if d else ""))
    return 0


def cmd_coverage(a):
    """For every registered action type no recording compared (exact / mismatch / rng-only), where the dumped scenes
    use it: in FSMs the sim runs (a recording that reaches that state would cover it) or only in FSMs it does not."""
    rep = json.load(open(a.report, encoding="utf-8"))
    compared = {t for t, c in rep["stats"].items() if c.get("exact", 0) + c.get("mismatch", 0) + c.get("rng-only", 0) > 0}
    todo = [t for t in rep["registry"] if t not in compared]
    sites = collections.defaultdict(list)   # type -> [(scene, owner, fsm, state, live)]
    sim = Sim(a.dll)
    fsm_dir = os.path.join(ROOT, "analysis", "fsm")
    for fn in sorted(os.listdir(fsm_dir)):
        scene = fn[:-5]
        if not fn.endswith(".json") or scene not in SCENE_CORPORA:
            continue
        sim.open(scene, 0)
        for fs in json.load(open(os.path.join(fsm_dir, fn), encoding="utf-8"))["fsms"]:
            for st in fs["states"]:
                for act in st["actions"]:
                    t = act["type"].rsplit(".", 1)[-1]
                    if t in todo:
                        sites[t].append((scene, canon(fs["path"]), fs["fsmName"], st["name"], live_fsm(sim, canon(fs["path"]), fs["fsmName"])))
    for t in todo:
        l = sites.get(t, [])
        live = [x for x in l if x[4]]
        where = "; ".join("%s %s|%s|%s" % x[:4] for x in live[:4]) + (" ..." if len(live) > 4 else "")
        called = " (called, never compared)" if t in rep["stats"] else ""
        print("coverage %s%s: %s" % (t, called, ("live in %d states: %s" % (len(live), where)) if live else
                                      ("only in FSMs the sim does not run (%d states)" % len(l)) if l else "in no recorded scene's FSMs"))
    return 0


def cmd_inert(a):
    """OBS payloads equal from the second step on (docs/engine-lifecycle.md: the reset observation carries
    interpolation residue), after the obs-wire.md §5 mask of wall-clock fields (hkpy/obs_codec.py).  Can fail: any
    recorder side effect on gameplay shows up in some observation; two different corpora compare DIFFERENT."""
    from hkpy import hktrace, obs_codec
    obs = []
    for p in (a.on, a.off):
        t = hktrace.read_trace(p)
        obs.append([obs_codec.mask(r.payload, obs_codec.decode(r.payload)) for r in t.records if type(r).__name__ == "Obs"])
    x, y = obs
    first = next((i for i, (u, v) in enumerate(zip(x, y)) if i >= 2 and u != v), None)
    same = first is None and len(x) == len(y)
    print("inert: %s (%d vs %d observations%s)" % ("OK" if same else "DIFFERENT", len(x), len(y),
                                                   "" if first is None else ", first differing step %d" % first))
    return 0 if same else 1


def cmd_show(a):
    """Every differing field of the first N calls of one type with a given verdict: the localisation step."""
    shown = 0
    for path in recording_files(a.paths):
        sim = Sim(a.dll)
        for ep in episodes(path):
            recs = [r for r in ep.recs if r["t"] == a.type]
            if not recs:
                continue
            sim.open(ep.level, 0)
            sim.set_pd_base(ep)
            if recs[0]["k"] == "pm":
                ids = {r["a"] for r in recs}
                acts = collections.defaultdict(list)
                for r in ep.recs:
                    if r["k"] == "pm" and r["a"] in ids:
                        acts[r["a"]].append(r)
                pairs = []
                for rs in acts.values():
                    rs.sort(key=lambda r: r["j"])
                    pairs += replay_activation(sim, ep, rs, all_diffs=True)
            else:
                pairs = [(r, replay_component(sim, ep, r, all_diffs=True)) for r in recs]
            for r, o in pairs:
                if o.verdict != a.verdict:
                    continue
                print("== %s q=%s %s|%s|%s|%s cb=%s j=%s nd=%s: %s %s" % (os.path.basename(path), r["q"], r["o"], r.get("n"), r.get("s"),
                                                                     r.get("i"), r.get("cb"), r.get("j"), r.get("nd"), o.verdict, o.note or ""))
                for f, g, s in (o.game if isinstance(o.game, list) and o.field == "*" else [(o.field, o.game, o.sim)]):
                    print("   %-50s game=%s  sim=%s" % (f, _short(g, 200), _short(s, 200)))
                shown += 1
                if shown >= a.n:
                    return 0
    return 0


# ------------------------------------------------------------------ recording

SCENE_CORPORA = {   # scene -> its policy corpus directory under analysis/ (analysis/README.md "Recordings")
    "GG_False_Knight": "polbat_GG_False_Knight", "GG_Ghost_Gorb": "polbat_GG_Ghost_Gorb", "GG_Ghost_Hu": "polbat_GG_Ghost_Hu",
    "GG_Ghost_Markoth": "polbat_GG_Ghost_Markoth", "GG_Ghost_Marmu": "polbat_GG_Ghost_Marmu", "GG_Ghost_No_Eyes": "polbat_GG_Ghost_No_Eyes",
    "GG_Ghost_Xero": "polbat_GG_Ghost_Xero", "GG_Grimm_Nightmare": "polbat_GG_Grimm_Nightmare", "GG_Gruz_Mother": "polbat_gruz",
    "GG_Gruz_Mother_V": "polbat_GG_Gruz_Mother_V", "GG_Hornet_1": "polbat_hornet", "GG_Hornet_2": "polbat_GG_Hornet_2",
    "GG_Mega_Moss_Charger": "polbat_GG_Mega_Moss_Charger", "GG_Nosk": "polbat_GG_Nosk", "GG_Soul_Master": "polbat_GG_Soul_Master"}
ACTION_RANGES = (3, 3, 8, 2)   # the corpus action vector's value counts (docs/trace-format.md §Corpus file)


def perturbed(corpus, name, rate, seed):
    """The policy corpus with each step replaced, at `rate`, by a uniformly random action (Python's own seeded RNG:
    the game's Random is untouched)."""
    import random
    rng = random.Random(seed)
    steps = [[rng.randrange(n) for n in ACTION_RANGES] if rng.random() < rate else list(s) for s in corpus["steps"]]
    out = dict(corpus, name=name, steps=steps)
    return out


def cmd_record(a):
    """Record the scenes in the game with HK_ORACLE_METHODS=1: `--policy` policy corpora per scene and `--perturbed`
    perturbed ones, `--jobs` instances at once (tags <prefix>0..), into <out>/<scene>/."""
    import threading, queue
    sys.path.insert(0, HERE)
    import run_oracle
    scenes = a.scenes.split(",") if a.scenes else sorted(SCENE_CORPORA)
    out_root = os.path.abspath(a.out)   # the game runs in its install directory: relative trace paths would land there
    work = []
    for sc in scenes:
        cd = os.path.join(ROOT, "analysis", SCENE_CORPORA[sc])
        names = sorted(f[:-len(".corpus.json")] for f in os.listdir(cd) if f.endswith(".corpus.json"))
        od = os.path.join(out_root, sc + ("@T%d" % a.tier if a.tier is not None else ""))
        os.makedirs(od, exist_ok=True)
        for n in names[:a.policy]:
            work.append((sc, os.path.join(cd, n + ".corpus.json"), od, n, a.methods))
        for k in range(a.perturbed):
            base = json.load(open(os.path.join(cd, names[k % len(names)] + ".corpus.json"), encoding="utf-8"))
            n = "pert%d_%s" % (k, names[k % len(names)])
            cp = os.path.join(od, n + ".corpus.json")
            json.dump(perturbed(base, n, a.rate, 1000 + k), open(cp, "w", encoding="utf-8"))
            work.append((sc, cp, od, n, a.methods))
    q = queue.Queue()
    for w in work:
        q.put(w)
    tags = queue.Queue()
    for i in range(a.jobs):
        tags.put("%s%d" % (a.prefix, i))
    lock = threading.Lock()

    def worker():
        while True:
            try:
                sc, cp, od, n, methods = q.get_nowait()
            except queue.Empty:
                return
            tag = tags.get()
            tr = os.path.join(od, n + ".a.hktrace")
            if os.path.exists(tr) and os.path.getsize(tr) > 100000 and not a.force:
                tags.put(tag)
                continue
            corpus = json.load(open(cp, encoding="utf-8"))
            env = ["HK_ORACLE_SCRIPT=" + cp, "HK_ORACLE_TRACE=" + tr, "HK_ORACLE_CAPTURE_DT=0.02", "HK_ORACLE_SHAKE_FPS0=1",
                   "HK_ORACLE_NOINTERP=1", "HK_ORACLE_SEED=%d" % int(corpus.get("seed") or 0)]
            if methods:
                env.append("HK_ORACLE_METHODS=1")
            if a.tier is not None:
                env.append("HK_ORACLE_TIER=%d" % a.tier)
            t0 = time.time()
            rc = run_oracle.run(tag, env, a.timeout, False, os.path.join(od, "logs"))
            mo = tr[:-len(".hktrace")] + ".methods.jsonl.gz"
            with lock:
                print("record: %s %s rc=%d %.0fs methods=%s" % (sc, n, rc, time.time() - t0,
                                                               "%dKB" % (os.path.getsize(mo) // 1024) if os.path.exists(mo) else "-"), flush=True)
            tags.put(tag)
    ts = [threading.Thread(target=worker) for _ in range(a.jobs)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    return 0


def main(argv=None):
    sys.stdout.reconfigure(encoding="utf-8")   # game strings (localised text, clone names) in the table; a redirected Windows stdout is cp1252
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sp = ap.add_subparsers(dest="cmd", required=True)
    r = sp.add_parser("replay"); r.add_argument("paths", nargs="+"); r.add_argument("--out"); r.add_argument("--jobs", type=int, default=1)
    r.add_argument("--dll"); r.add_argument("--limit", type=int, help="activations per episode (debugging)")
    r.add_argument("--calls", help="a TSV with one row per mismatch / trap and per site one identity / unported (type, file, q, site, first field)")
    w = sp.add_parser("show"); w.add_argument("paths", nargs="+"); w.add_argument("--type", required=True)
    w.add_argument("--verdict", default="mismatch"); w.add_argument("--n", type=int, default=3); w.add_argument("--dll")
    s = sp.add_parser("selfcheck"); s.add_argument("paths", nargs="+")
    i = sp.add_parser("inert"); i.add_argument("on"); i.add_argument("off")
    d = sp.add_parser("pd"); d.add_argument("paths", nargs="+"); d.add_argument("--dll")
    v = sp.add_parser("coverage"); v.add_argument("report", help="replay --out JSON"); v.add_argument("--dll")
    c = sp.add_parser("record"); c.add_argument("--out", default=os.path.join(ROOT, "analysis", "method_oracle"))
    c.add_argument("--scenes"); c.add_argument("--policy", type=int, default=4); c.add_argument("--perturbed", type=int, default=2)
    c.add_argument("--rate", type=float, default=0.25); c.add_argument("--jobs", type=int, default=4); c.add_argument("--prefix", default="moracle")
    c.add_argument("--timeout", type=float, default=1500); c.add_argument("--force", action="store_true")
    c.add_argument("--tier", type=int, help="HK_ORACLE_TIER: BossLevel k, recorded under <scene>@T<k> and replayed on that level key")
    c.add_argument("--no-methods", dest="methods", action="store_false", help="record without the recorder (the inertness reference)")
    a = ap.parse_args(argv)
    return {"replay": cmd_replay, "show": cmd_show, "selfcheck": cmd_selfcheck, "inert": cmd_inert, "pd": cmd_pd, "coverage": cmd_coverage, "record": cmd_record}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
