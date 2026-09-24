"""The observer's membership is a predicate on the live world (sim/fsm/runtime/observer.c, the mod's
HitboxObserver.Classify), so a collider is observed however its object came to exist.

- GG_Ghost_Markoth's `Markoth Shield(Clone)/Shield` colliders (DamageHero, layer 11) are created after the scene
  loads and never pass through a registration route; they are Enemy rows that hurt, from the first step.
- GG_Grimm_Nightmare seed 1 under an invulnerable random policy (tests/test_checkpoint.py's rollout) spawns more
  `Nightmare Firebat`s at once than the dump's pool holds, so ObjectPool.Spawn Instantiates the rest: the most
  firebat rows seen at once exceeds the dumped `Nightmare Firebat(Clone)` count.
"""
import ctypes, gzip, json, os, sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
sys.path.insert(0, ROOT)
from hkpy import sim_driver as sd

DLL = os.environ.get("HKSIM_DLL") or os.path.join(ROOT, "sim", "build", "hksim.dll")
GIVES_DAMAGE = 7   # combat column (docs/sim-api.md)


def _lib():
    lib = sd.load(DLL)
    lib.hksim_get_value.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_double)]
    lib.hksim_get_value.restype = ctypes.c_int
    return lib


def _n_gos(lib, s):
    v = ctypes.c_double()
    assert lib.hksim_get_value(s, b"fsm.n_gos", ctypes.byref(v)) == 0
    return int(v.value)


def _rollout(lib, scene, seed, steps, kind):
    """Per step: (GameObjects in the world, rows of `kind`, of which damaging)."""
    cfg = sd.Config(scene, 1, 0, 0)
    s = lib.hksim_create(ctypes.byref(cfg))
    lib.hksim_set_obs_mode(s, sd.HKSIM_OBS_BATCH)
    vocab = lib.hksim_vocab_create(4096)
    kid = lib.hksim_vocab_intern(vocab, kind)
    buf = sd.BatchBuffers(1, cap_combat=255, cap_terrain=512)
    res = sd.StepResult(); act = (ctypes.c_int32 * 4)()
    rng = np.random.default_rng(seed)
    out = []
    try:
        assert lib.hksim_reset(s, seed) == 0, lib.hksim_last_error(s)
        n0 = _n_gos(lib, s)
        for _ in range(steps):
            assert lib.hksim_set_value(s, b"hero.cstate.invulnerable", 1) == 0
            act[:] = [int(rng.integers(3)), int(rng.integers(3)), int(rng.integers(8)), int(rng.integers(2))]
            assert lib.hksim_step(s, act, ctypes.byref(res)) == 0, lib.hksim_last_error(s)
            sd.obs_batch(lib, [s], vocab, buf)
            nc = min(int(buf["n_combat"][0]), 255)
            mine = buf["combat_kind"][0][:nc] == kid
            out.append((_n_gos(lib, s), int(mine.sum()), int((buf["combat"][0][:nc, GIVES_DAMAGE][mine] > 0.5).sum())))
            if res.done:
                break
    finally:
        lib.hksim_vocab_destroy(vocab)
        lib.hksim_destroy(s)
    return n0, out


def test_markoth_shield_is_observed():
    _, out = _rollout(_lib(), b"GG_Ghost_Markoth", 1, 60, b"Shield")
    assert all(n == 2 and d == 2 for _, n, d in out[1:]), "Markoth's two Shield colliders are not damaging Enemy rows: %r" % out[:5]


def test_runtime_clone_is_observed():
    hier = os.path.join(ROOT, "analysis", "dumps", "GG_Grimm_Nightmare", "hierarchy.json.gz")
    with gzip.open(hier, "rt", encoding="utf-8") as fh:
        pooled = sum(1 for o in json.load(fh)["objects"] if (o.get("path") or "").endswith("Nightmare Firebat(Clone)"))
    assert pooled > 0
    n0, out = _rollout(_lib(), b"GG_Grimm_Nightmare", 1, 3600, b"Nightmare Firebat")
    assert out[-1][0] > n0, "no pool grew: the test no longer exercises runtime Instantiate"
    most = max(o[1] for o in out)
    assert most > pooled, "at most %d firebat rows at once, the dump pools %d: no Instantiated firebat observed" % (most, pooled)
