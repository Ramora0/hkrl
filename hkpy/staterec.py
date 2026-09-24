"""Reader for .hkstate, the per-frame full-state record (docs/state-record.md).

    rec = StateRecord(path)
    for fr in rec.frames():            # state is applied through the end of `fr`
        hero = rec.find("HeroController")[0]
        knight = rec.entity(hero.parent)                      # the GameObject entity
        cstate = rec.entity(hero["cState"])                   # plain:HeroControllerStates
        print(fr.frame, knight["wp.x"], knight["wp.y"], cstate["onGround"])

The writer is oracle/Record/StateWriter.cs; tests/test_staterec.py pins this reader to it with a fixture the
C# writer produced.
"""
import gzip
import json
import struct

T_STR, T_CLASS, T_FRAME, T_BORN, T_DIED, T_SET, T_END, T_NOTE, T_TRAILER = range(1, 10)
SCALAR, LIST = "fdilbsoe", "ILFSOE"
VERSION = 1


class Class:
    __slots__ = ("id", "name", "fields", "types", "index")

    def __init__(self, cid, name):
        self.id, self.name, self.fields, self.types, self.index = cid, name, [], [], {}


class Entity:
    """One entity: its class, parent eid and key are fixed at birth; `vals` holds raw values by field index
    (f32/f64 as their bit patterns, strings as ids, lists as tuples)."""
    __slots__ = ("eid", "cls", "parent", "key", "vals", "_rec", "born_frame")

    def __init__(self, rec, eid, cls, parent, key, born_frame):
        self._rec, self.eid, self.cls, self.parent, self.key, self.born_frame = rec, eid, cls, parent, key, born_frame
        self.vals = []

    def raw(self, name):
        i = self.cls.index[name]
        return self.vals[i] if i < len(self.vals) else _default(self.cls.types[i])

    def __contains__(self, name):
        return name in self.cls.index

    def __getitem__(self, name):
        i = self.cls.index[name]
        return self._rec._decode(self.cls.types[i], self.vals[i] if i < len(self.vals) else _default(self.cls.types[i]))

    def get(self, name, default=None):
        return self[name] if name in self.cls.index else default

    def as_dict(self):
        return {n: self[n] for n in self.cls.fields}

    def __repr__(self):
        return "Entity(%d %s %r)" % (self.eid, self.cls.name, self.key)


class Frame:
    """One recorded frame: born = eids born, died = the Entity objects that died (their last state),
    changed = {eid: set of field indices set}, notes = NOTE texts."""
    __slots__ = ("index", "frame", "step", "fixed_count", "flags", "born", "died", "changed", "notes")

    def __init__(self, index, frame, step, fixed_count, flags):
        self.index, self.frame, self.step, self.fixed_count, self.flags = index, frame, step, fixed_count, flags
        self.born, self.died, self.changed, self.notes = [], [], {}, []

    @property
    def live(self):
        return bool(self.flags & 1)


def _default(t):
    return () if t in LIST else 0


class _Buf:
    __slots__ = ("b", "p")

    def __init__(self, b):
        self.b, self.p = b, 0

    def var(self):
        b, p, v, s = self.b, self.p, 0, 0
        while True:
            x = b[p]
            p += 1
            v |= (x & 0x7F) << s
            if x < 0x80:
                break
            s += 7
        self.p = p
        return v

    def zig(self):
        v = self.var()
        return (v >> 1) ^ -(v & 1)

    def u8(self):
        x = self.b[self.p]
        self.p += 1
        return x

    def u32(self):
        v = struct.unpack_from("<I", self.b, self.p)[0]
        self.p += 4
        return v

    def u64(self):
        v = struct.unpack_from("<Q", self.b, self.p)[0]
        self.p += 8
        return v

    def raw(self, n):
        v = self.b[self.p:self.p + n]
        self.p += n
        return v


class StateRecord:
    def __init__(self, path):
        with gzip.open(path, "rb") as f:
            data = f.read()
        if data[:4] != b"HKST":
            raise ValueError("%s: not an .hkstate file" % path)
        version, jlen = struct.unpack_from("<iI", data, 4)
        if version != VERSION:
            raise ValueError("%s: version %d, reader knows %d" % (path, version, VERSION))
        self.header = json.loads(data[12:12 + jlen].decode("utf-8"))
        self._b = _Buf(data)
        self._b.p = 12 + jlen
        self.strings = [""]
        self.classes = {}
        self.entities = {}
        self.by_class = {}      # class name -> {eid: Entity}
        self.by_parent = {}     # parent eid -> {eid: Entity}
        self.trailer = None
        self._consumed = False

    # ------------------------------------------------------------------ decoding
    def _decode(self, t, v):
        if t == "f":
            return struct.unpack("<f", struct.pack("<I", v))[0]
        if t == "d":
            return struct.unpack("<d", struct.pack("<q", v))[0]
        if t == "b":
            return bool(v)
        if t == "s":
            return self.strings[v]
        if t == "F":
            return [struct.unpack("<f", struct.pack("<I", x))[0] for x in v]
        if t == "S":
            return [self.strings[x] for x in v]
        return list(v) if t in LIST else v

    def _value(self, t):
        b = self._b
        if t == "f":
            return b.u32()
        if t == "d":
            return struct.unpack("<q", struct.pack("<Q", b.u64()))[0]
        if t == "b":
            return b.u8()
        if t in "se":
            return b.var()
        if t in "iol":
            return b.zig()
        n = b.var()
        if t == "F":
            return tuple(b.u32() for _ in range(n))
        if t in "SE":
            return tuple(b.var() for _ in range(n))
        return tuple(b.zig() for _ in range(n))

    # ------------------------------------------------------------------ the stream
    def frames(self):
        """Apply the stream frame by frame; yields a Frame after each frame's END, with self.entities current."""
        if self._consumed:
            raise RuntimeError("frames() can be iterated once; open the file again")
        self._consumed = True
        b, n = self._b, len(self._b.b)
        fr, index = None, 0
        while b.p < n:
            tag = b.u8()
            if tag == T_STR:
                sid = b.var()
                s = b.raw(b.var()).decode("utf-8")
                if sid != len(self.strings):
                    raise ValueError("string id %d out of order (have %d)" % (sid, len(self.strings)))
                self.strings.append(s)
            elif tag == T_CLASS:
                cid, name, start, cnt = b.var(), self.strings[b.var()], b.var(), b.var()
                c = self.classes.get(cid)
                if c is None:
                    c = self.classes[cid] = Class(cid, name)
                if start != len(c.fields):
                    raise ValueError("class %s extended at %d, has %d fields" % (name, start, len(c.fields)))
                for _ in range(cnt):
                    fname, t = self.strings[b.var()], chr(b.u8())
                    c.index[fname] = len(c.fields)
                    c.fields.append(fname)
                    c.types.append(t)
            elif tag == T_FRAME:
                fr = Frame(index, b.var(), b.zig(), b.var(), b.u8())
            elif tag == T_BORN:
                eid, cid, parent, key = b.var(), b.var(), b.var(), self.strings[b.var()]
                e = self.entities[eid] = Entity(self, eid, self.classes[cid], parent, key, fr.frame)
                self.by_class.setdefault(e.cls.name, {})[eid] = e
                self.by_parent.setdefault(parent, {})[eid] = e
                fr.born.append(eid)
            elif tag == T_DIED:
                e = self.entities.pop(b.var())
                del self.by_class[e.cls.name][e.eid]
                del self.by_parent[e.parent][e.eid]
                fr.died.append(e)
            elif tag == T_SET:
                e = self.entities[b.var()]
                vals, types, i = e.vals, e.cls.types, -1
                ch = fr.changed.setdefault(e.eid, set())
                while True:
                    d = b.var()
                    if d == 0:
                        break
                    i += d
                    if i >= len(vals):
                        vals.extend(_default(t) for t in types[len(vals):i + 1])
                    vals[i] = self._value(types[i])
                    ch.add(i)
            elif tag == T_NOTE:
                fr.notes.append(self.strings[b.var()])
            elif tag == T_END:
                index += 1
                yield fr
            elif tag == T_TRAILER:
                self.trailer = json.loads(self.strings[b.var()])
            else:
                raise ValueError("unknown record tag %d at byte %d" % (tag, b.p - 1))

    # ------------------------------------------------------------------ queries on the current state
    def entity(self, eid):
        return self.entities.get(eid)

    def find(self, cls=None, key=None):
        """Live entities whose class name is `cls` (or starts with `cls + '@'`) and whose key equals `key`."""
        if cls is None:
            pool = self.entities.values()
        else:
            pool = [e for name, d in self.by_class.items() if name == cls or name.startswith(cls + "@") for e in d.values()]
        return [e for e in pool if key is None or e.key == key]

    def children(self, eid):
        return list(self.by_parent.get(eid, {}).values())


def last_state(path):
    """(record, frames) with the record's state at the end of the file."""
    rec = StateRecord(path)
    frames = list(rec.frames())
    return rec, frames


def truncate(src, dst, n_frames):
    """Write the first n_frames frames of a recording to dst (no trailer): a fixture cut from a real recording."""
    rec = StateRecord(src)
    it = rec.frames()
    for _ in range(n_frames):
        next(it)
    with gzip.open(dst, "wb", compresslevel=9) as f:
        f.write(rec._b.b[:rec._b.p])
