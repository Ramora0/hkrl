"""The game client's parser against messages laid out as the oracle mod packs
them (oracle/Net/BinaryProtocol.cs Pack), trailers included.

    python tests/train/test_game_client.py      (or pytest tests/train)
"""
import struct

import numpy as np

import _paths  # noqa: F401
import game_client as gc


def _s8(s):
    b = s.encode()
    return struct.pack("B", len(b)) + b


def _s16(s):
    b = s.encode()
    return struct.pack("<H", len(b)) + b


def pack(kind, combat, terrain, gs, kinds, parents, step=None, info=""):
    """What BinaryProtocol.Pack writes for a "reset" or "step" message."""
    out = struct.pack("B", 2 if kind == "step" else 1)
    out += struct.pack("<HH", len(combat), len(terrain))
    out += np.asarray(combat, "<f4").tobytes() + np.asarray(terrain, "<f4").tobytes()
    out += np.asarray(gs, "<f4").tobytes()
    if kind == "step":
        landed, hits, healed, done = step
        out += struct.pack("<fffffBB", landed, hits, 0.04, 0.01, healed, done, 1)
    out += b"".join(_s8(k) for k in kinds) + b"".join(_s8(p) for p in parents)
    out += b"".join(_s16(f"terrain/{i}") for i in range(len(terrain)))
    if kind == "step":
        out += struct.pack("<HHHif", 3, 1, len(terrain), 17, 12.5)       # diag
    else:
        out += struct.pack("<B" + "fH" * 7, 1, *([1.0, 2] * 7))           # reset phases
    out += struct.pack("<H", 2) + _s16("a|b|c|d") + _s16("e|f|g|h")      # FSM snapshots
    if kind == "step":
        out += _s8(info)
    return out


def test_parse_matches_the_mod_layout():
    rng = np.random.default_rng(0)
    combat = rng.standard_normal((3, 14)).astype(np.float32)
    terrain = rng.standard_normal((5, 8)).astype(np.float32)
    gs = rng.standard_normal(33).astype(np.float32)
    kinds, parents = ["Grimm Boss|Idle", "Bat", "Spike"], ["Grimm Boss", "", "Grimm Boss"]

    r = gc.unpack_reset(pack("reset", combat, terrain, gs, kinds, parents))
    assert np.array_equal(r[0], combat) and np.array_equal(r[1], terrain)
    assert np.array_equal(r[2], gs) and r[3] == kinds and r[4] == parents

    s = gc.unpack_step(pack("step", combat, terrain, gs, kinds, parents,
                            step=(12.5, 1.0, 0.0, 1), info="win"))
    assert np.array_equal(s[0], combat) and np.array_equal(s[1], terrain)
    assert np.array_equal(s[2], gs) and s[3] == kinds and s[4] == parents
    assert (s[5], s[6], s[7], s[8], s[9]) == (12.5, 1.0, 0.0, True, "win")

    e = gc.unpack_step(pack("step", np.zeros((0, 14)), np.zeros((0, 8)), gs, [], [],
                            step=(0.0, 0.0, 1.0, 0)))
    assert e[0].shape == (0, 14) and e[1].shape == (0, 8) and e[8] is False and e[9] == ""
    assert gc.pack_reset("GG_Hornet_1", 1) == struct.pack(
        "<BiiBBH11s", 1, 1, 1, 0, 1, 11, b"GG_Hornet_1")
    print("  reset and step messages (with every trailer) parse to what was packed")


if __name__ == "__main__":
    test_parse_matches_the_mod_layout()
    print("ALL TESTS PASSED")
