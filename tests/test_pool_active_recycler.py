"""ObjectPool's ActiveRecycler path (HK/ObjectPool.cs:232-262, :471-563) on GG_Hive_Knight@T1's `Strike Nail R`, the hit
effect a Bee Dropper's `Recoil` FSM spawns on NAIL HIT.  An ActiveRecycler clone is never switched off: CreatePool makes
it active and parks it at (-20,-20), Spawn sends its FSMs "A SPAWN" instead of SetActive(true), and Recycle parks it
again and sends "A RECYCLE" (the prefab's `Strike` FSM: global A RECYCLE -> Reset -> Dormant, Dormant -A SPAWN->
Vibrate).  So the second spawn reuses the same clone, woken from Dormant by the event.
"""
import ctypes as C
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)
from hkpy import sim_driver as sd

DLL = os.environ.get("HKSIM_DLL") or os.path.join(ROOT, "sim", "build", "hksim.dll")
CLONE = b"_GameManager/GlobalPool/Strike Nail R(Clone)"


def _lib():
    lib = sd.load(DLL)
    P = C.c_void_p
    for name, res, args in (("hksim_fsm_world", P, [P]),
                            ("hkfsm_go_find", C.c_int32, [P, C.c_char_p]),
                            ("hkfsm_go_count", C.c_int32, [P]),
                            ("hkfsm_go_path", C.c_char_p, [P, C.c_int32]),
                            ("hkfsm_go_info", C.c_int, [P, C.c_int32, C.POINTER(C.c_int32)]),
                            ("hkfsm_fsm_find", C.c_int32, [P, C.c_char_p, C.c_char_p]),
                            ("hkfsm_fsm_state", C.c_char_p, [P, C.c_int32]),
                            ("hkfsm_send_event", C.c_int, [P, C.c_int32, C.c_char_p])):
        getattr(lib, name).restype = res
        getattr(lib, name).argtypes = args
    return lib


def _info(lib, w, go):
    o = (C.c_int32 * 8)()
    lib.hkfsm_go_info(w, go, o)
    return list(o)


def _parent(lib, w, go):
    return _info(lib, w, go)[0]


def _clones(lib, w):
    """The pool's copies: the prefab template the tables carry has the same path but is an asset (go_info[1])."""
    return [g for g in range(lib.hkfsm_go_count(w)) if lib.hkfsm_go_path(w, g) == CLONE and not _info(lib, w, g)[1]]


def test_strike_nail_active_recycler():
    lib = _lib()
    s = lib.hksim_create(C.byref(sd.Config(b"GG_Hive_Knight@T1", 1, 3, 1)))
    assert s, lib.hksim_last_error(None)
    try:
        assert lib.hksim_reset(s, 3) == 0, lib.hksim_last_error(s)
        sd.drain(lib, s)
        w = lib.hksim_fsm_world(s)
        recoil = lib.hkfsm_fsm_find(w, b"Battle Scene/Droppers/Bee Dropper", b"Recoil")
        pool = lib.hkfsm_go_find(w, b"_GameManager/GlobalPool")
        assert recoil >= 0 and pool >= 0 and lib.hkfsm_fsm_state(w, recoil) == b"Idle"
        assert lib.hkfsm_fsm_find(w, CLONE, b"Strike") < 0            # no pool for it at the dump
        res, idle = sd.StepResult(), (C.c_int32 * 4)(2, 2, 7, 1)

        def step():
            assert lib.hksim_step(s, idle, C.byref(res)) == 0, lib.hksim_last_error(s)
            sd.drain(lib, s)

        lib.hkfsm_send_event(w, recoil, b"NAIL HIT")                     # Recoil: SpawnObjectFromGlobalPool
        step()
        strike = lib.hkfsm_fsm_find(w, CLONE, b"Strike")
        assert strike >= 0
        assert len(_clones(lib, w)) == 1
        clone = _clones(lib, w)[0]
        assert _parent(lib, w, clone) == -1                              # :486 spawned: parent null
        assert lib.hkfsm_fsm_state(w, strike) in (b"Vibrate", b"Effect")
        for _ in range(300):                                            # Effect -> End: RecycleSelf
            step()
            if lib.hkfsm_fsm_state(w, strike) == b"Dormant":
                break
        assert lib.hkfsm_fsm_state(w, strike) == b"Dormant"             # A RECYCLE -> Reset -> Dormant: still running
        assert _parent(lib, w, clone) == pool                            # :249 back under the pool
        assert lib.hkfsm_fsm_state(w, recoil) == b"Idle"
        lib.hkfsm_send_event(w, recoil, b"NAIL HIT")
        step()
        assert lib.hkfsm_fsm_state(w, strike) in (b"Vibrate", b"Effect")   # A SPAWN woke the same clone
        assert _parent(lib, w, clone) == -1
        assert _clones(lib, w) == [clone]
    finally:
        lib.hksim_destroy(s)
