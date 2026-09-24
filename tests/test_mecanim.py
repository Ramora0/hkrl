"""The Animator runtime (sim/fsm/runtime/mecanim.c) on GG_Grimm_Nightmare's flare pillar.

analysis/native_specs/native-animator.md §7 derives the pillar's damage window from the asset data alone: the Animator
counts live frames from its activation (dt 0 in a frozen frame, N-AN-2; first advance at the first
DirectorUpdateAnimationBegin after the activation, N-AN-3), and the clip's BoxCollider2D.m_Enabled curve (1 from
0.5 s, 0 from 0.7222 s, keys inclusive) is written enabled at the advance that takes the float32 normalized time to
0.50000006 (the 25th) and disabled at the one that takes it to 0.73999983 (the 37th).  A re-activation clears the
controller state and restarts the clip.

The pillar is spawned between agent steps (after the frame's end), so its first advance is the next step's frozen
frame (dt 0) and, at frames_per_wait 1, live frame k is step k's.
"""
import ctypes as C
import os
import sys

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
                            ("hkfsm_pool_spawn", C.c_int32, [P, C.c_char_p, C.POINTER(C.c_float)]),
                            ("hkfsm_col_state", C.c_int, [P, C.c_int32, C.c_int32, C.POINTER(C.c_int32)]),
                            ("hkfsm_go_set_active", C.c_int, [P, C.c_int32, C.c_int32]),
                            ("hkfsm_go_count", C.c_int32, [P]), ("hkfsm_go_path", C.c_char_p, [P, C.c_int32]),
                            ("hkfsm_go_info", C.c_int, [P, C.c_int32, C.POINTER(C.c_int32)]),
                            ("hkfsm_last_error", C.c_char_p, [])):
        getattr(lib, name).restype = res
        getattr(lib, name).argtypes = args
    return lib


def _pillar_child(lib, w, clone):
    for g in range(lib.hkfsm_go_count(w)):
        o = (C.c_int32 * 8)()
        lib.hkfsm_go_info(w, g, o)
        if o[0] == clone and lib.hkfsm_go_path(w, g).decode().endswith("/Pillar"):
            return g
    raise AssertionError("no Pillar child under the spawned clone")


def _enabled_steps(lib, s, w, pillar, n):
    """Steps 1..n (one live frame each, the knight idle): those after which the Pillar's BoxCollider2D is enabled."""
    on, st, res, idle = [], (C.c_int32 * 2)(), sd.StepResult(), (C.c_int32 * 4)(0, 0, 0, 0)
    for k in range(1, n + 1):
        assert lib.hksim_step(s, idle, C.byref(res)) == 0, lib.hksim_last_error(s)
        sd.drain(lib, s)
        lib.hkfsm_col_state(w, pillar, 0, st)
        if st[0]:
            on.append(k)
    return on


def test_flare_pillar_window():
    lib = _lib()
    cfg = sd.Config(b"GG_Grimm_Nightmare", 1, SEED, 1)
    s = lib.hksim_create(C.byref(cfg))
    assert s, lib.hksim_last_error(None)
    try:
        assert lib.hksim_reset(s, SEED) == 0, lib.hksim_last_error(s)
        sd.drain(lib, s)
        w = lib.hksim_fsm_world(s)
        clone = lib.hkfsm_pool_spawn(w, b"Grimm_flare_pillar(Clone)", (C.c_float * 3)(80.0, 20.0, 0.0))
        assert clone >= 0, lib.hkfsm_last_error()
        pillar = _pillar_child(lib, w, clone)
        st = (C.c_int32 * 2)()
        lib.hkfsm_col_state(w, pillar, 0, st)
        assert st[0] == 0 and st[1] == 1
        assert _enabled_steps(lib, s, w, pillar, 45) == list(range(25, 37))
        # deactivate + reactivate between steps: the clip starts over
        assert lib.hkfsm_go_set_active(w, clone, 0) == 0 and lib.hkfsm_go_set_active(w, clone, 1) == 0
        assert _enabled_steps(lib, s, w, pillar, 45) == list(range(25, 37))
    finally:
        lib.hksim_destroy(s)
