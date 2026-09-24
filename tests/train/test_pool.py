"""The sim pool is one environment whatever its worker count: with the same
seed and actions, 1 worker and 4 workers produce bit-identical raw buffers
-- kind/parent ids included -- and the same canonical vocab id space.

Four bosses over four workers, so each worker meets its own strings in its
own order: exactly the case where independent workers would hand one string
different ids. The vocab coordinator (sim_worker.py) is what makes the ids
agree; the ids are compared as raw integers, so any divergence fails.

    python tests/train/test_pool.py      (or pytest tests/train)
"""
import numpy as np

import _paths  # noqa: F401
from config import Config
from rollout import RAW_KEYS
from sim_env import SimPool

BOSSES = "GG_Hornet_1,GG_Gruz_Mother,GG_False_Knight,GG_Mega_Moss_Charger"


def run(workers, n_envs=8, steps=150, seed=1234):
    cfg = Config(boss_levels=BOSSES, n_envs=n_envs, pool_workers=workers)
    env = SimPool(cfg, seed=seed)
    rng = np.random.default_rng(7)
    frames = []
    try:
        env.reset()
        frames.append({k: env.arrays[k].copy() for k in RAW_KEYS})
        for _ in range(steps):
            acts = np.stack([rng.integers(0, n, n_envs) for n in (3, 3, 8, 2)], 1)
            env.step(acts)
            frames.append({k: env.arrays[k].copy() for k in RAW_KEYS})
        return frames, env.vocab_i2s(), env.unknown_id_rows()
    finally:
        env.close()


def test_workers_do_not_change_the_env():
    f1, v1, u1 = run(1)
    f4, v4, u4 = run(4)
    assert v1 == v4, "the canonical id space depends on the worker count"
    for t, (a, b) in enumerate(zip(f1, f4)):
        for k in RAW_KEYS:
            assert np.array_equal(a[k], b[k]), f"step {t}: {k} differs between 1 and 4 workers"
    assert u1 == u4 == 0, (u1, u4)
    print(f"  1 worker == 4 workers: {len(f1) - 1} steps x 8 envs on 4 bosses bit-identical, "
          f"ids included; {len(v1)} vocab ids, identical")


if __name__ == "__main__":
    test_workers_do_not_change_the_env()
    print("ALL TESTS PASSED")
