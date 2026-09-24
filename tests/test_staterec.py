"""hkpy/staterec.py against a fixture written by the mod's encoder (oracle/Record/StateWriter.cs via
tests/fixtures/staterec/gen/Program.cs). The expected values below are the ones Program.cs writes."""
import os
import struct

import pytest

from hkpy import staterec

FIXTURE = os.path.join(os.path.dirname(__file__), "fixtures", "staterec", "known.hkstate")


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def test_known_answer():
    rec = staterec.StateRecord(FIXTURE)
    assert rec.header == {"fixture": "known", "level": "TEST"}
    frames = []
    it = rec.frames()

    fr = next(it)
    frames.append(fr)
    assert (fr.index, fr.frame, fr.step, fr.fixed_count, fr.flags, fr.live) == (0, 100, -1, 7, 1, True)
    assert fr.notes == ["hello"]
    (a,) = rec.find("Obj")
    (b,) = rec.find("Bag")
    assert (a.key, a.parent, b.key, b.parent) == ("A", 0, "B", a.eid)
    assert a.as_dict() == {"x": 1.5, "n": -3, "ok": True, "name": "alpha", "ref": -12345,
                           "big": -1234567890123, "t": f32(0.02), "self": a.eid}
    assert a.raw("x") == 0x3FC00000
    assert b["fl"] == [0.25, -2.0]
    assert b["il"] == [1, -2, 300000]
    assert b["sl"] == ["p", "", "q"]
    assert b["ol"] == [-5, 7]
    assert b["el"] == [a.eid]
    assert b["ll"] == [-(2 ** 63), 5]
    assert sorted(fr.born) == sorted([a.eid, b.eid]) and fr.died == []

    fr = next(it)
    assert (fr.frame, fr.step, fr.fixed_count, fr.live) == (101, 0, 8, False)
    assert a["n"] == 4 and a["x"] == 1.5 and a["extra"] == 9
    assert a.cls.fields[-1] == "extra"
    changed = {a.cls.fields[i] for i in fr.changed[a.eid]}
    assert changed == {"n", "extra"}, "an unchanged field was re-sent or a changed one dropped"
    assert [e.eid for e in fr.died] == [b.eid] and rec.entity(b.eid) is None
    assert fr.died[0]["il"] == [1, -2, 300000]
    (c,) = rec.find("Native")
    assert (c.key, c["v"]) == ("C", 1)

    fr = next(it)
    assert fr.frame == 102
    assert a.eid not in fr.changed, "a frame with no change must not emit a SET"
    assert [e.key for e in fr.died] == ["C"]
    (d,) = rec.find("Native")
    assert d.key == "D" and d.eid != c.eid and d["v"] == 1

    with pytest.raises(StopIteration):
        next(it)
    assert rec.trailer == {"frames": 3}


def test_rejects_other_files(tmp_path):
    p = tmp_path / "x.hkstate"
    import gzip
    with gzip.open(p, "wb") as f:
        f.write(b"HKTR\x01\x00\x00\x00")
    with pytest.raises(ValueError):
        staterec.StateRecord(str(p))


def test_compare_tool():
    """tools/staterec_check.py --compare accepts a record against itself and rejects the variant fixture
    (A.n = 5 instead of 4 from frame 1 on)."""
    import importlib.util
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    spec = importlib.util.spec_from_file_location("staterec_check", os.path.join(root, "tools", "staterec_check.py"))
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    variant = os.path.join(os.path.dirname(FIXTURE), "variant.hkstate")
    assert tool.compare(FIXTURE, FIXTURE) is True
    assert tool.compare(FIXTURE, variant) is False


def test_compare_across_processes():
    """--compare accepts two processes' records of one episode (clockA/clockB: other frame counts, clocks, instance
    ids, asset ids and counters) and still rejects a real divergence in one of them (clockC: Mono.timer 0.52 at the
    last frame)."""
    import importlib.util
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    spec = importlib.util.spec_from_file_location("staterec_check", os.path.join(root, "tools", "staterec_check.py"))
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    d = os.path.dirname(FIXTURE)
    a, b, c = (os.path.join(d, n + ".hkstate") for n in ("clockA", "clockB", "clockC"))
    assert tool.compare(a, b) is True
    assert tool.compare(b, a) is True
    assert tool.compare(a, c) is False


def test_slots_fast_path():
    """Program.cs "slots": puts by slot and from a scratch buffer, an unchanged string reference not re-put, a
    class with no fields (declared all the same), written by the background thread."""
    rec = staterec.StateRecord(os.path.join(os.path.dirname(FIXTURE), "slots.hkstate"))
    frames = list(rec.frames())
    assert [f.frame for f in frames] == [10, 11, 12]
    (a,) = rec.find("Obj")
    (z,) = rec.find("Empty")
    assert z.parent == a.eid and z.as_dict() == {}
    assert (a["x"], a["l"], a["s"]) == (3.5, [7], "one")
    ix, il, is_ = (a.cls.index[n] for n in ("x", "l", "s"))
    assert frames[0].changed[a.eid] == {ix, il, is_}
    assert frames[1].changed[a.eid] == {ix}      # same list, same string reference: nothing re-sent
    assert frames[2].changed[a.eid] == {il}
    assert rec.trailer == {"frames": 3}
