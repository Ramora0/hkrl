"""Runtime pool growth: a Spawn with no free clone Instantiates one (ObjectPool.cs:503-517;
sim/fsm/runtime/world.c world_instantiate), and hksim_reset rebuilds the scene-load world whatever
the previous episode grew.

GG_False_Knight, seed 2, under tools/fingerprint.py's action stream (wire mode) runs a pool dry within STEPS, so
a pool grows (the test asserts it does).
"""
import ctypes, hashlib, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)
from hkpy import sim_driver as sd

DLL = os.environ.get("HKSIM_DLL") or os.path.join(ROOT, "sim", "build", "hksim.dll")
SCENE, SEED, STEPS = b"GG_False_Knight", 2, 600


def _actions(seed, n):   # tools/fingerprint.py actions()
    x = (seed * 2654435761 + 12345) & 0xFFFFFFFF
    for _ in range(n):
        x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
        yield (x >> 8) % 3, (x >> 12) % 3, (x >> 16) % 8, (x >> 20) % 2


def _lib():
    lib = sd.load(DLL)
    lib.hksim_get_value.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_double)]
    lib.hksim_get_value.restype = ctypes.c_int
    return lib


def _n_gos(lib, s):
    v = ctypes.c_double()
    assert lib.hksim_get_value(s, b"fsm.n_gos", ctypes.byref(v)) == 0
    return int(v.value)


def _episode(lib, s, steps=STEPS):
    """One episode from reset(SEED): (trace hash, GameObjects at reset, GameObjects at the end)."""
    h = hashlib.sha256()
    assert lib.hksim_reset(s, SEED) == 0, lib.hksim_last_error(s)
    n0 = _n_gos(lib, s)
    h.update(sd.drain(lib, s))
    res = sd.StepResult(); act = (ctypes.c_int32 * 4)()
    for i, a in enumerate(_actions(SEED, steps)):
        act[:] = a
        assert lib.hksim_step(s, act, ctypes.byref(res)) == 0, "step %d: %s" % (i, lib.hksim_last_error(s))
        h.update(bytes(res)); h.update(sd.drain(lib, s))
        if res.done:
            break
    return h.hexdigest(), n0, _n_gos(lib, s)


def test_pool_growth_is_deterministic_across_resets():
    lib = _lib()
    cfg = sd.Config(SCENE, 1, SEED, 1)
    a, b, c = (lib.hksim_create(ctypes.byref(cfg)) for _ in range(3))
    try:
        h1, n0, n1 = _episode(lib, a)
        assert n1 > n0, "the pool never grew: the test no longer exercises runtime Instantiate"
        assert _episode(lib, b) == (h1, n0, n1)      # a second instance in the same process
        # The reset observation carries the reset count (TrainingEnv.ResetCount), so a second episode is
        # compared with another second episode: after the grown one, and after one that never grew.
        _, _, nc = _episode(lib, c, steps=100)
        assert nc == n0
        h3, n0_after_growth, n3 = _episode(lib, a)
        assert n0_after_growth == n0, "reset kept runtime clones"
        assert _episode(lib, c) == (h3, n0, n3)
        assert n3 == n1
    finally:
        for s in (a, b, c):
            lib.hksim_destroy(s)
