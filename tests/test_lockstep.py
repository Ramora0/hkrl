"""hkpy/lockstep.py and sim/core/lockstep_api.c against a real recording: the first four frames of
GG_Hornet_1 ph_ep00 (SceneReady, step 1's frozen frame and its two live frames), cut from
analysis/corpora_v2/GG_Hornet_1/state/ph_ep00.a.hkstate with hkpy.staterec.truncate(src, dst, 4)."""
import os

import pytest

from hkpy import lockstep as LS

FIXTURE = os.path.join(os.path.dirname(__file__), "fixtures", "lockstep", "ph_ep00_f4.hkstate")
ACTIONS = [[1, 2, 0, 0]]   # ph_ep00.corpus.json steps[0]
HORNET = "Boss Holder/Hornet Boss 1#0"


def entry(L, owner, comp, field):
    hits = [i for i in range(L.n) if L.comp[i] == comp and L.field[i] == field
            and (owner is None or L.go_label(int(L.go[i])) == owner)]
    assert len(hits) == 1, (owner, comp, field, len(hits))
    return hits[0]


@pytest.fixture(scope="module")
def run():
    lib = LS.load_lib()
    L = LS.Lockstep(lib, FIXTURE, ACTIONS, log=lambda s: None)
    left = L.start()
    i = entry(L, HORNET, "Rigidbody2D", "position.y")
    frames, fall = [], []
    for _ in range(3):
        frames.append(L.advance())
        fall.append((int(L.stepped[i]), int(L.rec_val[i])))
    tail = L.advance()
    yield L, left, frames, tail, fall
    L.close()


def test_pairing(run):
    L = run[0]
    assert L.n > 100000 and L.mapped.sum() > 0.9 * L.n, "most sim entries pair with a recorded field"
    i = entry(L, HORNET, "Rigidbody2D", "gravityScale")
    assert L.mapped[i] and L.slot[i][2] == "raw"


def test_import_is_exact(run):
    """After the import of SceneReady, no importable field of a gameplay object still differs."""
    L, left, *_x = run
    left = [(i, a, b) for i, a, b in left if L.importable(i, b) and "[ui]" not in L.kind_of(i)]
    assert left == [], [L.label(i) for i, _a, _b in left[:10]]


def test_frame_kinds_and_end(run):
    _L, _left, frames, tail, _fall = run
    assert [(fr.frame, LS.KIND_NAMES[k]) for fr, k, *_ in frames] == \
        [(21873, "step"), (21874, "live"), (21875, "live_last")]
    assert all(trap is None for *_x, trap in frames)
    assert tail is None


def test_frozen_step_frame_is_exact(run):
    """Step 1's frozen frame applies the action and changes nothing the recording holds differently."""
    _L, _left, frames, *_x = run
    assert frames[0][3] == []


def test_free_fall_known_answer(run):
    """Hornet drops from rest (Rigidbody2D gravityScale 1.5, Physics2D gravity -60): one fixed step of semi-implicit
    Euler, v = -90 * 0.02 and y += v * 0.02, lands on the game's bits; a wrong import of the body or a wrong frame
    kind would not."""
    fall = run[4]
    assert [LS.f32(g) for _s, g in fall] == [44.59476089477539, 44.55876159667969, 44.48676300048828]
    assert [s for s, _g in fall] == [g for _s, g in fall]


def test_comparison_is_bitwise(run):
    """A one-ulp difference and the sign of a zero are divergences; a recorded value equal to the sim's is not."""
    L = run[0]
    sim_val, structs = L.export()
    base = {i for i, _a, _b in L.compare(sim_val, structs)}
    i = entry(L, "Knight#0", "Rigidbody2D", "position.x")
    j = entry(L, HORNET, "Rigidbody2D", "velocity.x")
    assert i not in base and j not in base
    saved = L.rec_val[i], L.rec_val[j]
    try:
        L.rec_val[i] = int(sim_val[i]) + 1
        L.rec_val[j] = int(sim_val[j]) ^ 0x80000000
        rows = [r for r in L.compare(sim_val, structs) if r[0] not in base]
    finally:
        L.rec_val[i], L.rec_val[j] = saved
    assert sorted(r[0] for r in rows) == sorted([i, j])
    got = {r[0]: (r[1], r[2]) for r in rows}
    assert got[i] == (int(sim_val[i]), int(sim_val[i]) + 1)
    assert got[j] == (int(sim_val[j]), int(sim_val[j]) ^ 0x80000000)
