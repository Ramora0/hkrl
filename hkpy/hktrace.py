"""Reader/writer for .hktrace v1 (docs/trace-format.md)."""
import hashlib
import json
import struct

MAGIC = b"HKTR"
VERSION = 2
# v2 adds ENTITY.rot (Rigidbody2D.rotation in degrees).  v1 traces stay readable and simply lack it --
# the protected analysis/traces/p0 corpus is v1.
READABLE_VERSIONS = (1, 2)

KINDS = {1: "FRAME", 2: "FIXED", 3: "HC_FIXED_PRE", 4: "HC_FIXED_POST", 5: "HC_UPDATE_PRE",
         6: "HC_UPDATE_POST", 7: "HC_LATE_PRE", 8: "HC_LATE_POST", 9: "OBS", 0x10: "EVENT"}
KIND_OF = {v: k for k, v in KINDS.items()}
POSE_KINDS = frozenset([2, 3, 4, 5, 6, 7, 8])  # FIXED + HC_*: identical payload

# ev code -> (name, [(field, code)]); codes: s=str16 u32 i32 u8 b(u8 as bool) 4i32
EVENTS = {
    0: ("SCENE_LOADED", [("scene", "s")]),
    1: ("STEP", [("step", "u32"), ("action", "4i32"), ("committed", "b")]),
    2: ("HERO_DAMAGE", [("source", "s"), ("amount", "i32"), ("hazard_type", "i32"), ("hp_after", "i32")]),
    3: ("ENEMY_DAMAGE", [("owner", "s"), ("attack_type", "i32"), ("damage", "i32"), ("hp_after", "i32")]),
    4: ("FSM_TRANSITION", [("owner", "s"), ("fsm", "s"), ("from", "s"), ("to", "s")]),
    5: ("FSM_EVENT", [("owner", "s"), ("fsm", "s"), ("event", "s")]),
    6: ("SPAWN", [("name", "s")]),
    7: ("DESPAWN", [("name", "s")]),
    8: ("RNG_SEED", [("seed", "i32")]),
    9: ("LOG", [("text", "s")]),
    10: ("EPISODE_END", [("info", "s")]),
    11: ("RESET_BEGIN", [("level_requested", "s")]),
    12: ("SCENE_READY", [("level", "s")]),
}
EV_OF = {v[0]: k for k, v in EVENTS.items()}

# InputDeviceShim.KeyNames (oracle/Game/ProxyController.cs:26-27)
DEFAULT_INPUTS = ["left", "right", "up", "down", "jump", "attack", "dash", "cast",
                  "dream_nail", "super_dash", "focus"]

_FLOAT_T = {"float", "single", "f32", "float32"}
_INT_T = {"int", "int32", "sbyte", "byte", "short", "int16", "uint", "uint32",
          "ushort", "uint16"}
_BOOL_T = {"bool", "boolean"}
_REJECT_T = {"double", "long", "int64", "ulong", "uint64", "decimal", "string", "char",
             "float64", "intptr"}


class TraceError(Exception):
    pass


def _code(type_str, where):
    t = str(type_str).strip().lower()
    t = t.rsplit(".", 1)[-1] if t.startswith("system.") else t
    if t in _FLOAT_T:
        return "f"
    if t in _BOOL_T:
        return "b"
    if t in _INT_T:
        return "i"
    if t in _REJECT_T:
        raise TraceError(f"{where}: type {type_str!r} is not one of float/int/bool/enum "
                         "(docs/trace-format.md #Header); the wire has no encoding for it")
    return "i"  # enum -> i32 (docs/trace-format.md #Header)


class _R:
    """Cursor with byte-offset/record-index error reporting."""
    __slots__ = ("b", "i", "rec", "off", "ver")

    def __init__(self, b, ver=VERSION):
        self.b, self.i, self.rec, self.off = b, 0, -1, 0
        self.ver = ver

    def need(self, n):
        if self.i + n > len(self.b):
            raise TraceError(
                f"truncated record: record index {self.rec} starting at byte offset {self.off} "
                f"wants {n} more byte(s) at offset {self.i}, but the file ends at {len(self.b)}")

    def _u(self, fmt, n):
        self.need(n)
        v = struct.unpack_from(fmt, self.b, self.i)[0]
        self.i += n
        return v

    def u8(self): return self._u("<B", 1)
    def u16(self): return self._u("<H", 2)
    def u32(self): return self._u("<I", 4)
    def i32(self): return self._u("<i", 4)
    def u64(self): return self._u("<Q", 8)
    def f32(self): return self._u("<f", 4)
    def boo(self): return self._u("<B", 1) != 0

    def blob(self, n):
        self.need(n)
        v = bytes(self.b[self.i:self.i + n])
        self.i += n
        return v

    def s16(self):
        n = self.u16()
        at = self.i
        raw = self.blob(n)
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError as e:
            raise TraceError(f"invalid UTF-8 in str16 at byte offset {at} "
                             f"(record index {self.rec}): {e}") from None


def _w(out, fmt, v):
    out += struct.pack(fmt, v)


def _ws(out, s):
    raw = s.encode("utf-8")
    if len(raw) > 0xFFFF:
        raise TraceError(f"str16 overflow: {len(raw)} bytes")
    out += struct.pack("<H", len(raw)) + raw


class Schema:
    def __init__(self, hero=(), cstate=(), playerdata=(), inputs=None, hero_go="Knight",
                 capture=None):
        self.hero = [(n, t) for n, t in hero]
        self.playerdata = [(n, t) for n, t in playerdata]
        self.cstate = list(cstate)
        self.inputs = list(DEFAULT_INPUTS if inputs is None else inputs)
        self.hero_go = hero_go
        self.capture = dict(capture or {})
        if len(self.cstate) > 64:
            raise TraceError(f"fields.cstate has {len(self.cstate)} names; FRAME.cstate is u64")
        self.hero_codes = [_code(t, f"fields.hero[{n}]") for n, t in self.hero]
        self.pd_codes = [_code(t, f"fields.playerdata[{n}]") for n, t in self.playerdata]
        self.hero_names = [n for n, _ in self.hero]
        self.pd_names = [n for n, _ in self.playerdata]

    @classmethod
    def from_header(cls, h):
        if not isinstance(h, dict) or "fields" not in h:
            raise TraceError("header JSON has no 'fields' object (docs/trace-format.md #Header)")
        f = h["fields"]
        for k in ("hero", "cstate", "playerdata", "input", "hero_go"):
            if k not in f:
                raise TraceError(f"header fields.{k} missing (docs/trace-format.md #Header)")

        def pairs(key):
            out = []
            for j, e in enumerate(f[key]):
                if not isinstance(e, dict) or "name" not in e or "type" not in e:
                    raise TraceError(f"header fields.{key}[{j}] is not {{name,type}}: {e!r}")
                out.append((e["name"], e["type"]))
            return out

        return cls(pairs("hero"), list(f["cstate"]), pairs("playerdata"), list(f["input"]),
                   f["hero_go"], h.get("capture"))

    def header(self):
        return {"capture": self.capture,
                "fields": {"hero": [{"name": n, "type": t} for n, t in self.hero],
                           "cstate": list(self.cstate),
                           "playerdata": [{"name": n, "type": t} for n, t in self.playerdata],
                           "input": list(self.inputs),
                           "hero_go": self.hero_go}}

    def same_layout(self, o):
        return (self.hero_names == o.hero_names and self.hero_codes == o.hero_codes
                and self.cstate == o.cstate and self.pd_names == o.pd_names
                and self.pd_codes == o.pd_codes)


class Anim:
    __slots__ = ("clip", "frame", "clip_time", "playing", "clip_fps")

    def __init__(self, clip="", frame=0, clip_time=0.0, playing=False, clip_fps=0.0):
        self.clip, self.frame, self.clip_time = clip, frame, clip_time
        self.playing, self.clip_fps = playing, clip_fps

    @staticmethod
    def read(r):
        return Anim(r.s16(), r.i32(), r.f32(), r.boo(), r.f32())

    def encode(self, out):
        _ws(out, self.clip)
        out += struct.pack("<ifBf", self.frame, self.clip_time, 1 if self.playing else 0,
                           self.clip_fps)

    def flat(self, d, p):
        d[p + ".clip"] = self.clip
        d[p + ".frame"] = self.frame
        d[p + ".clip_time"] = self.clip_time
        d[p + ".playing"] = self.playing
        d[p + ".clip_fps"] = self.clip_fps


class Collider:
    __slots__ = ("type", "enabled", "off_x", "off_y", "size_x", "size_y")

    def __init__(self, type="BoxCollider2D", enabled=True, off_x=0.0, off_y=0.0,
                 size_x=0.0, size_y=0.0):
        self.type, self.enabled = type, enabled
        self.off_x, self.off_y, self.size_x, self.size_y = off_x, off_y, size_x, size_y

    @staticmethod
    def read(r):
        return Collider(r.s16(), r.boo(), r.f32(), r.f32(), r.f32(), r.f32())

    def encode(self, out):
        _ws(out, self.type)
        out += struct.pack("<Bffff", 1 if self.enabled else 0, self.off_x, self.off_y,
                           self.size_x, self.size_y)


class FsmVar:
    """value on the wire is always f32; `type` says how to read it."""
    __slots__ = ("name", "type", "raw")
    TYPES = {"float": 0, "int": 1, "bool": 2}

    def __init__(self, name, type=0, raw=0.0):
        self.name = name
        self.type = FsmVar.TYPES[type] if isinstance(type, str) else int(type)
        if self.type not in (0, 1, 2):
            raise TraceError(f"fsm var {name!r}: type {self.type} not in 0/1/2")
        self.raw = float(raw)

    @property
    def value(self):
        if self.type == 2:
            return self.raw != 0.0
        if self.type == 1 and float(self.raw).is_integer():
            return int(self.raw)
        return self.raw


class Fsm:
    __slots__ = ("owner_path", "name", "state", "enabled", "vars")

    def __init__(self, owner_path="", name="", state="", enabled=True, vars=()):
        self.owner_path, self.name, self.state = owner_path, name, state
        self.enabled, self.vars = enabled, list(vars)

    @staticmethod
    def read(r):
        f = Fsm(r.s16(), r.s16(), r.s16(), r.boo())
        for _ in range(r.u16()):
            n = r.s16()
            t = r.u8()
            if t not in (0, 1, 2):
                raise TraceError(f"fsm var {n!r} has type byte {t} (expected 0/1/2) at offset "
                                 f"{r.i - 1}, record index {r.rec}")
            f.vars.append(FsmVar(n, t, r.f32()))
        return f

    def encode(self, out):
        _ws(out, self.owner_path)
        _ws(out, self.name)
        _ws(out, self.state)
        out += struct.pack("<BH", 1 if self.enabled else 0, len(self.vars))
        for v in self.vars:
            _ws(out, v.name)
            out += struct.pack("<Bf", v.type, v.raw)


class Entity:
    __slots__ = ("name", "instance_id", "active", "hp", "is_dead", "invincible",
                 "pos_x", "pos_y", "scale_x", "vel_x", "vel_y", "rot", "rot_t", "anim", "fsms")

    def __init__(self, name="", instance_id=0, active=True, hp=0, is_dead=False,
                 invincible=False, pos_x=0.0, pos_y=0.0, scale_x=1.0, vel_x=0.0, vel_y=0.0,
                 rot=0.0, rot_t=0.0, anim=None, fsms=()):
        self.name, self.instance_id, self.active = name, instance_id, active
        self.hp, self.is_dead, self.invincible = hp, is_dead, invincible
        self.pos_x, self.pos_y, self.scale_x = pos_x, pos_y, scale_x
        self.vel_x, self.vel_y = vel_x, vel_y
        self.rot = rot          # v2+: Rigidbody2D.rotation deg (what the solver sees)
        self.rot_t = rot_t      # v2+: transform.eulerAngles.z (what the colliders follow)
        self.anim = anim or Anim()
        self.fsms = list(fsms)

    @staticmethod
    def read(r):
        e = Entity(r.s16(), r.i32(), r.boo(), r.i32(), r.boo(), r.boo(),
                   r.f32(), r.f32(), r.f32(), r.f32(), r.f32())
        if r.ver >= 2:                              # v2 ENTITY.rot / rot_t
            e.rot = r.f32()
            e.rot_t = r.f32()
        e.anim = Anim.read(r)
        for _ in range(r.u16()):
            e.fsms.append(Fsm.read(r))
        return e

    def encode(self, out):
        _ws(out, self.name)
        out += struct.pack("<iBiBBfffffff", self.instance_id, 1 if self.active else 0, self.hp,
                           1 if self.is_dead else 0, 1 if self.invincible else 0,
                           self.pos_x, self.pos_y, self.scale_x, self.vel_x, self.vel_y,
                           self.rot, self.rot_t)
        self.anim.encode(out)
        out += struct.pack("<H", len(self.fsms))
        for f in self.fsms:
            f.encode(out)


class Hero:
    __slots__ = ("pos_x", "pos_y", "scale_x", "rb_pos_x", "rb_pos_y", "rb_vel_x", "rb_vel_y",
                 "rb_gravity", "rb_kinematic", "cstate", "fields", "pd", "anim", "cols")

    def __init__(self, pos_x=0.0, pos_y=0.0, scale_x=1.0, rb_pos_x=0.0, rb_pos_y=0.0,
                 rb_vel_x=0.0, rb_vel_y=0.0, rb_gravity=0.0, rb_kinematic=False, cstate=0,
                 fields=None, pd=None, anim=None, cols=()):
        self.pos_x, self.pos_y, self.scale_x = pos_x, pos_y, scale_x
        self.rb_pos_x, self.rb_pos_y = rb_pos_x, rb_pos_y
        self.rb_vel_x, self.rb_vel_y = rb_vel_x, rb_vel_y
        self.rb_gravity, self.rb_kinematic = rb_gravity, rb_kinematic
        self.cstate = cstate
        self.fields = dict(fields or {})
        self.pd = dict(pd or {})
        self.anim = anim or Anim()
        self.cols = list(cols)


def _read_vals(r, codes):
    out = []
    for c in codes:
        out.append(r.f32() if c == "f" else (r.i32() != 0 if c == "b" else r.i32()))
    return out


def _write_vals(out, codes, names, d, where):
    for c, n in zip(codes, names):
        if n not in d:
            raise TraceError(f"{where}: no value for field {n!r}")
        v = d[n]
        out += struct.pack("<f", v) if c == "f" else struct.pack("<i", int(v))


class Record:
    __slots__ = ()
    kind = 0

    @property
    def name(self):
        return KINDS[self.kind]


class Frame(Record):
    kind = 1
    __slots__ = ("schema", "frame", "fixed_count", "time", "dt", "unscaled_dt", "fixed_time",
                 "time_scale", "rng", "input", "step", "hero", "entities")

    def __init__(self, schema, frame=0, fixed_count=0, time=0.0, dt=0.0, unscaled_dt=0.0,
                 fixed_time=0.0, time_scale=1.0, rng=(0, 0, 0, 0), input=0, step=0,
                 hero=None, entities=()):
        self.schema, self.frame, self.fixed_count = schema, frame, fixed_count
        self.time, self.dt, self.unscaled_dt = time, dt, unscaled_dt
        self.fixed_time, self.time_scale = fixed_time, time_scale
        self.rng = tuple(rng)
        if len(self.rng) != 4:
            raise TraceError("FRAME.rng must have 4 words")
        self.input, self.step = input, step
        self.hero = hero if hero is not None else make_hero(schema)
        self.entities = list(entities)

    @staticmethod
    def read(r, sc):
        f = Frame(sc, r.u32(), r.u32(), r.f32(), r.f32(), r.f32(), r.f32(), r.f32(),
                  (r.u32(), r.u32(), r.u32(), r.u32()), r.u32(), r.u32())
        h = f.hero
        (h.pos_x, h.pos_y, h.scale_x, h.rb_pos_x, h.rb_pos_y, h.rb_vel_x, h.rb_vel_y,
         h.rb_gravity) = (r.f32() for _ in range(8))
        h.rb_kinematic = r.boo()
        h.cstate = r.u64()
        h.fields = dict(zip(sc.hero_names, _read_vals(r, sc.hero_codes)))
        h.pd = dict(zip(sc.pd_names, _read_vals(r, sc.pd_codes)))
        h.anim = Anim.read(r)
        h.cols = [Collider.read(r) for _ in range(r.u8())]
        f.entities = [Entity.read(r) for _ in range(r.u16())]
        return f

    def encode(self, out, sc):
        out += struct.pack("<IIfffff", self.frame, self.fixed_count, self.time, self.dt,
                           self.unscaled_dt, self.fixed_time, self.time_scale)
        out += struct.pack("<4I", *self.rng)
        out += struct.pack("<II", self.input, self.step)
        h = self.hero
        out += struct.pack("<ffffffffBQ", h.pos_x, h.pos_y, h.scale_x, h.rb_pos_x, h.rb_pos_y,
                           h.rb_vel_x, h.rb_vel_y, h.rb_gravity,
                           1 if h.rb_kinematic else 0, h.cstate)
        _write_vals(out, sc.hero_codes, sc.hero_names, h.fields, "FRAME hero fields")
        _write_vals(out, sc.pd_codes, sc.pd_names, h.pd, "FRAME playerdata fields")
        h.anim.encode(out)
        if len(h.cols) > 0xFF:
            raise TraceError("n_col is u8")
        out += struct.pack("<B", len(h.cols))
        for c in h.cols:
            c.encode(out)
        out += struct.pack("<H", len(self.entities))
        for e in self.entities:
            e.encode(out)

    def flat(self):
        sc, h = self.schema, self.hero
        d = {"frame": self.frame, "fixed_count": self.fixed_count, "time": self.time,
             "dt": self.dt, "unscaled_dt": self.unscaled_dt, "fixed_time": self.fixed_time,
             "time_scale": self.time_scale}
        for i, v in enumerate(self.rng):
            d["rng%d" % i] = v
        d["input"] = self.input
        d["step"] = self.step
        for k in ("pos_x", "pos_y", "scale_x", "rb_pos_x", "rb_pos_y", "rb_vel_x", "rb_vel_y",
                  "rb_gravity", "rb_kinematic"):
            d["hero." + k] = getattr(h, k)
        for i, n in enumerate(sc.cstate):
            d["hero.cstate." + n] = bool(h.cstate >> i & 1)
        for i in range(len(sc.cstate), 64):  # unnamed bits: only reported when set
            if h.cstate >> i & 1:
                d["hero.cstate.bit%d" % i] = True
        for n in sc.hero_names:
            d["hero.f." + n] = h.fields[n]
        for n in sc.pd_names:
            d["pd." + n] = h.pd[n]
        h.anim.flat(d, "hero.anim")
        d["hero.n_col"] = len(h.cols)
        for i, c in enumerate(h.cols):
            p = "hero.col[%d]." % i
            for k in ("type", "enabled", "off_x", "off_y", "size_x", "size_y"):
                d[p + k] = getattr(c, k)
        d["n_ent"] = len(self.entities)
        seen = {}
        for e in self.entities:
            k = seen.get(e.name, 0)
            seen[e.name] = k + 1
            p = "ent[%s#%d]" % (e.name, k)
            for a in ("active", "hp", "is_dead", "invincible", "pos_x", "pos_y", "scale_x",
                      "vel_x", "vel_y"):
                d[p + "." + a] = getattr(e, a)
            e.anim.flat(d, p + ".anim")
            d[p + ".n_fsm"] = len(e.fsms)
            fseen = {}
            for fs in e.fsms:
                key = "%s/%s" % (fs.owner_path, fs.name)
                j = fseen.get(key, 0)
                fseen[key] = j + 1
                fp = "%s.fsm[%s%s]" % (p, key, "" if j == 0 else "#%d" % j)
                d[fp + ".state"] = fs.state
                d[fp + ".enabled"] = fs.enabled
                for v in fs.vars:
                    d[fp + ".var." + v.name] = v.value
        return d


class Pose(Record):
    """FIXED and the six HC_* records: identical payload."""
    __slots__ = ("kind", "frame", "fixed_count", "fixed_time", "pos_x", "pos_y",
                 "rb_pos_x", "rb_pos_y", "rb_vel_x", "rb_vel_y")

    def __init__(self, kind=2, frame=0, fixed_count=0, fixed_time=0.0, pos_x=0.0, pos_y=0.0,
                 rb_pos_x=0.0, rb_pos_y=0.0, rb_vel_x=0.0, rb_vel_y=0.0):
        self.kind = KIND_OF[kind] if isinstance(kind, str) else kind
        if self.kind not in POSE_KINDS:
            raise TraceError(f"kind {self.kind} is not FIXED/HC_*")
        self.frame, self.fixed_count, self.fixed_time = frame, fixed_count, fixed_time
        self.pos_x, self.pos_y = pos_x, pos_y
        self.rb_pos_x, self.rb_pos_y = rb_pos_x, rb_pos_y
        self.rb_vel_x, self.rb_vel_y = rb_vel_x, rb_vel_y

    @staticmethod
    def read(r, kind):
        return Pose(kind, r.u32(), r.u32(), r.f32(), r.f32(), r.f32(), r.f32(), r.f32(),
                    r.f32(), r.f32())

    def encode(self, out, sc):
        out += struct.pack("<IIfffffff", self.frame, self.fixed_count, self.fixed_time,
                           self.pos_x, self.pos_y, self.rb_pos_x, self.rb_pos_y,
                           self.rb_vel_x, self.rb_vel_y)

    def flat(self):
        d = {"frame": self.frame, "fixed_count": self.fixed_count, "fixed_time": self.fixed_time}
        for k in ("pos_x", "pos_y", "rb_pos_x", "rb_pos_y", "rb_vel_x", "rb_vel_y"):
            d["hero." + k] = getattr(self, k)
        return d


class Obs(Record):
    kind = 9
    __slots__ = ("which", "reset_index", "step", "frame", "payload")

    def __init__(self, which=0, reset_index=0, step=0, frame=0, payload=b""):
        self.which = {"reset": 0, "step": 1}[which] if isinstance(which, str) else which
        self.reset_index, self.step, self.frame = reset_index, step, frame
        self.payload = bytes(payload)

    @staticmethod
    def read(r):
        which, reset_index, step, frame, n = r.u8(), r.u32(), r.u32(), r.u32(), r.u32()
        return Obs(which, reset_index, step, frame, r.blob(n))

    def encode(self, out, sc):
        out += struct.pack("<BIIII", self.which, self.reset_index, self.step, self.frame,
                           len(self.payload)) + self.payload

    def flat(self):
        return {"which": self.which, "reset_index": self.reset_index, "step": self.step,
                "frame": self.frame, "len": len(self.payload),
                "payload_sha256": hashlib.sha256(self.payload).hexdigest()}


class Event(Record):
    kind = 0x10
    __slots__ = ("ev", "frame", "fixed_count", "phase", "args")

    def __init__(self, ev=0, frame=0, fixed_count=0, phase=0, **args):
        self.ev = EV_OF[ev] if isinstance(ev, str) else ev
        if self.ev not in EVENTS:
            raise TraceError(f"unknown EVENT ev code {self.ev}")
        self.frame, self.fixed_count, self.phase = frame, fixed_count, phase
        want = [n for n, _ in EVENTS[self.ev][1]]
        missing = [n for n in want if n not in args]
        extra = [n for n in args if n not in want]
        if missing or extra:
            raise TraceError(f"EVENT {self.ev_name}: missing {missing}, unexpected {extra}; "
                             f"expected {want}")
        self.args = dict(args)

    @property
    def ev_name(self):
        return EVENTS[self.ev][0]

    @staticmethod
    def read(r):
        ev, frame, fixed_count, phase = r.u8(), r.u32(), r.u32(), r.u8()
        if ev not in EVENTS:
            raise TraceError(f"unknown EVENT ev code {ev} at byte offset {r.i - 10} "
                             f"(record index {r.rec}); payload length is unknowable, cannot skip")
        args = {}
        for n, c in EVENTS[ev][1]:
            args[n] = (r.s16() if c == "s" else r.u32() if c == "u32" else r.i32() if c == "i32"
                       else r.boo() if c == "b" else [r.i32() for _ in range(4)])
        return Event(ev, frame, fixed_count, phase, **args)

    def encode(self, out, sc):
        out += struct.pack("<BIIB", self.ev, self.frame, self.fixed_count, self.phase)
        for n, c in EVENTS[self.ev][1]:
            v = self.args[n]
            if c == "s":
                _ws(out, v)
            elif c == "u32":
                out += struct.pack("<I", v)
            elif c == "i32":
                out += struct.pack("<i", v)
            elif c == "b":
                out += struct.pack("<B", 1 if v else 0)
            else:
                if len(v) != 4:
                    raise TraceError(f"EVENT {self.ev_name}.{n} must have 4 ints")
                out += struct.pack("<4i", *v)

    def flat(self):
        d = {"ev": self.ev, "ev_name": self.ev_name, "frame": self.frame,
             "fixed_count": self.fixed_count, "phase": self.phase}
        for n, c in EVENTS[self.ev][1]:
            if c == "4i32":
                for i, x in enumerate(self.args[n]):
                    d["%s.%d" % (n, i)] = x
            else:
                d[n] = self.args[n]
        return d


class Trace:
    def __init__(self, header, records, schema=None, header_bytes=None, path=None):
        self.header = header
        self.records = records
        self.schema = schema or Schema.from_header(header)
        self.header_bytes = header_bytes
        self.path = path

    def frames(self):
        return [r for r in self.records if r.kind == 1]


def read_trace(path):
    with open(path, "rb") as fh:
        buf = fh.read()
    if len(buf) < 12:
        raise TraceError(f"{path}: {len(buf)} bytes is shorter than the 12-byte fixed header")
    if buf[:4] != MAGIC:
        raise TraceError(f"{path}: bad magic {buf[:4]!r} at offset 0, expected {MAGIC!r}")
    version, json_len = struct.unpack_from("<II", buf, 4)
    if version not in READABLE_VERSIONS:
        raise TraceError(f"{path}: trace schema version {version}, this reader implements "
                         f"versions {READABLE_VERSIONS} (docs/trace-format.md)")
    if 12 + json_len > len(buf):
        raise TraceError(f"{path}: header json_len={json_len} at offset 8 overruns the file "
                         f"({len(buf)} bytes)")
    hb = buf[12:12 + json_len]
    try:
        header = json.loads(hb.decode("utf-8"))
    except Exception as e:
        raise TraceError(f"{path}: header JSON at offset 12 is unreadable: {e}") from None
    sc = Schema.from_header(header)

    r = _R(buf, version)
    r.i = 12 + json_len
    records = []
    while r.i < len(buf):
        r.rec = len(records)
        r.off = r.i
        k = r.u8()
        if k not in KINDS:
            raise TraceError(f"{path}: unknown record kind 0x{k:02x} at byte offset {r.off} "
                             f"(record index {r.rec}); payload length is unknowable, cannot skip")
        if k == 1:
            records.append(Frame.read(r, sc))
        elif k in POSE_KINDS:
            records.append(Pose.read(r, k))
        elif k == 9:
            records.append(Obs.read(r))
        else:
            records.append(Event.read(r))
    if r.i != len(buf):
        raise TraceError(f"{path}: {len(buf) - r.i} trailing byte(s) after record index "
                         f"{len(records) - 1} at offset {r.i}")
    return Trace(header, records, sc, hb, path)


def read_trace_retry(path, tries=3, wait_s=2.0):
    """read_trace, retried: the shared corpus store can fail a read while another process writes to it.
    None when every try failed -- a caller reports an unreadable trace, never a divergence."""
    import time
    for k in range(tries):
        try:
            return read_trace(path)
        except (OSError, TraceError):
            if k + 1 < tries:
                time.sleep(wait_s)
    return None


def write_trace(path, header, records):
    """header: a Schema, the header dict, or raw header JSON bytes."""
    if isinstance(header, Schema):
        sc, hb = header, None
    elif isinstance(header, (bytes, bytearray)):
        hb = bytes(header)
        sc = Schema.from_header(json.loads(hb.decode("utf-8")))
    else:
        sc, hb = Schema.from_header(header), json.dumps(header, separators=(",", ":"),
                                                        sort_keys=False).encode("utf-8")
    if hb is None:
        hb = json.dumps(sc.header(), separators=(",", ":")).encode("utf-8")
    out = bytearray(MAGIC + struct.pack("<II", VERSION, len(hb)) + hb)
    for i, rec in enumerate(records):
        if isinstance(rec, Frame) and not sc.same_layout(rec.schema):
            raise TraceError(f"record {i}: FRAME schema differs from the header schema")
        out += struct.pack("<B", rec.kind)
        rec.encode(out, sc)
    with open(path, "wb") as fh:
        fh.write(out)
    return len(out)


def make_hero(schema, fields=None, pd=None, cstate=0, cols=(), anim=None, **kw):
    f = {n: (0.0 if c == "f" else False if c == "b" else 0)
         for n, c in zip(schema.hero_names, schema.hero_codes)}
    f.update(fields or {})
    p = {n: (0.0 if c == "f" else False if c == "b" else 0)
         for n, c in zip(schema.pd_names, schema.pd_codes)}
    p.update(pd or {})
    if isinstance(cstate, dict):
        bits = 0
        for n, v in cstate.items():
            if n not in schema.cstate:
                raise TraceError(f"cstate {n!r} is not in fields.cstate")
            bits |= (1 if v else 0) << schema.cstate.index(n)
        cstate = bits
    return Hero(cstate=cstate, fields=f, pd=p, anim=anim, cols=cols, **kw)


def make_frame(schema, **kw):
    return Frame(schema, **kw)


def make_pose(kind, **kw):
    return Pose(kind, **kw)


def make_obs(**kw):
    return Obs(**kw)


def make_event(ev, frame=0, fixed_count=0, phase=0, **args):
    return Event(ev, frame, fixed_count, phase, **args)


if __name__ == "__main__":
    import sys
    t = read_trace(sys.argv[1])
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    print(json.dumps(t.header, indent=1)[:2000])
    print("%d records, %d FRAME" % (len(t.records), len(t.frames())))
    for i, rec in enumerate(t.records[:n]):
        f = rec.flat()
        print(i, rec.name, {k: f[k] for k in list(f)[:8]})
