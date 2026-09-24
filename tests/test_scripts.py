"""Ported MonoBehaviours (sim/fsm/components/scripts.c) against their decompiled source.

Breakable (Breakable.cs:225-441): the first nail hit breaks it -- its trigger turns off, so the knight's down slash can
pogo off it once (NailSlash.cs bounces off an Interactive Object's trigger) -- and its debris activates; a second hit
finds it broken.

GG_Soul_Master's `Roar Wave Emitter(Clone)` is dumped mid-roar, with DisableAfterTime.disableTime 43.689 at Time.time
32.429 (restored by scr_restore) and referenced by the Mage Lord's restored `Roar Emitter` variable.  The Mage Lord's
`Roar End` sends to it, so the emitter leaves `Emit` on the step the roar ends -- in the game on the same frame
(analysis/corpora_v2/GG_Soul_Master/sm_ep00.a.hktrace frame 27826: emitter Emit -> End, Mage Lord Roar -> Roar End)
-- well before its DisableAfterTime timer (about 11.26 s in) would.
"""
import ctypes as C
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)
from hkpy import sim_driver as sd

DLL = os.environ.get("HKSIM_DLL") or os.path.join(ROOT, "sim", "build", "hksim.dll")
SEED = 3


def _lib():
    lib = sd.load(DLL)
    P = C.c_void_p
    for name, res, args in (("hksim_fsm_world", P, [P]),
                            ("hkfsm_hit", C.c_int, [P, C.c_int32, C.c_int32, C.c_int32, C.c_int32, C.c_float, C.c_float]),
                            ("hkfsm_col_state", C.c_int, [P, C.c_int32, C.c_int32, C.POINTER(C.c_int32)]),
                            ("hkfsm_go_find", C.c_int32, [P, C.c_char_p]),
                            ("hkfsm_fsm_find", C.c_int32, [P, C.c_char_p, C.c_char_p]),
                            ("hkfsm_fsm_state", C.c_char_p, [P, C.c_int32]),
                            ("hkfsm_last_error", C.c_char_p, [])):
        getattr(lib, name).restype = res
        getattr(lib, name).argtypes = args
    return lib


def _sim(lib, scene):
    s = lib.hksim_create(C.byref(sd.Config(scene, 1, SEED, 1)))
    assert s, lib.hksim_last_error(None)
    assert lib.hksim_reset(s, SEED) == 0, lib.hksim_last_error(s)
    sd.drain(lib, s)
    return s


def _col(lib, w, go):
    st = (C.c_int32 * 2)()
    assert lib.hkfsm_col_state(w, go, 0, st) == 0
    return st[0], st[1]


def _break_grave(lib, s):
    """Breakable.Hit twice on GG_Ghost_Xero's grave_stone_break: it breaks on the first hit only."""
    w = lib.hksim_fsm_world(s)
    grave = lib.hkfsm_go_find(w, b"grave_stone_break")
    rocks = [lib.hkfsm_go_find(w, b"grave_stone_break/Simple Rock L2"), lib.hkfsm_go_find(w, b"grave_stone_break/Simple Rock L4")]
    knight = lib.hkfsm_go_find(w, b"Knight")
    assert grave >= 0 and min(rocks) >= 0 and knight >= 0
    assert _col(lib, w, grave) == (1, 1)                        # its trigger, before the hit
    assert all(_col(lib, w, r)[1] == 0 for r in rocks)          # debris inactive
    # a nail down slash (AttackTypes.Nail, direction 270)
    assert lib.hkfsm_hit(w, grave, knight, 0, 13, 270.0, 1.0) == 0, lib.hkfsm_last_error()
    assert _col(lib, w, grave)[0] == 0                          # Break: bodyCollider.enabled = false
    assert all(_col(lib, w, r) == (1, 1) for r in rocks)        # debrisParts SetActive(true)
    assert lib.hkfsm_hit(w, grave, knight, 0, 13, 270.0, 1.0) == 0, lib.hkfsm_last_error()
    assert _col(lib, w, grave)[0] == 0


def test_breakable_breaks_once():
    lib = _lib()
    s = _sim(lib, b"GG_Ghost_Xero")
    try:
        _break_grave(lib, s)
    finally:
        lib.hksim_destroy(s)


def test_breakable_debris_steps():
    lib = _lib()
    s = _sim(lib, b"GG_Ghost_Xero")
    try:
        _break_grave(lib, s)
        res, idle = sd.StepResult(), (C.c_int32 * 4)(0, 0, 0, 0)
        for _ in range(50):                                         # the debris falls and bounces without a trap
            assert lib.hksim_step(s, idle, C.byref(res)) == 0, lib.hksim_last_error(s)
            sd.drain(lib, s)
    finally:
        lib.hksim_destroy(s)

def test_roar_emitter_ends_with_roar():
    lib = _lib()
    s = _sim(lib, b"GG_Soul_Master")
    try:
        w = lib.hksim_fsm_world(s)
        f = lib.hkfsm_fsm_find(w, b"Roar Wave Emitter(Clone)", b"emitter")
        ml = lib.hkfsm_fsm_find(w, b"Mage Lord", b"Mage Lord")
        assert f >= 0 and ml >= 0
        res, idle = sd.StepResult(), (C.c_int32 * 4)(0, 0, 0, 0)
        assert lib.hkfsm_fsm_state(w, f) == b"Emit" and lib.hkfsm_fsm_state(w, ml) == b"Roar"
        ended_at = None
        for k in range(1, 700):
            assert lib.hksim_step(s, idle, C.byref(res)) == 0, lib.hksim_last_error(s)
            sd.drain(lib, s)
            roaring = lib.hkfsm_fsm_state(w, ml) == b"Roar"
            if lib.hkfsm_fsm_state(w, f) != b"Emit":
                ended_at = k
                assert not roaring
                break
            assert roaring and not res.done
        # the DisableAfterTime timer would end it at ~563 (11.26 s, one 0.02 s frame per step)
        assert ended_at is not None and ended_at < 555, ended_at
    finally:
        lib.hksim_destroy(s)
