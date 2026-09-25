"""Properties of the hitless objective (train_hitless.py), on synthetic
rollouts -- no sim needed.

  1. The vectorized GAE equals a per-env scalar loop with a terminal at every
     episode end, and a hit is not one.
  2. With lambda 1 and zero values the return is the damage landed from each
     step up to the knight's death, the killing step's own damage excluded.
  3. The soft reward: the entropy bonus is zero at the target, hard-commit
     steps leave the action head out, and alpha moves toward the target.

    python tests/train/test_hitless.py      (or pytest tests/train)
"""
import numpy as np

import _paths  # noqa: F401
from train_hitless import HitlessConfig, HitlessPPO

BOSS = "GG_Hornet_1"


def mk(**kw):
    return HitlessPPO(HitlessConfig(**{"seq_len": 8, "chunks_per_batch": 8, "boss_levels": BOSS,
                                       **kw}))


def gae_reference(cfg, reward, hits, values, dones):
    T = len(reward)
    gl = cfg.gamma * cfg.gae_lambda
    adv, ret = np.empty(T, np.float32), np.empty(T, np.float32)
    g = 0.0
    for t in reversed(range(T)):
        if dones[t]:
            next_v, g = 0.0, 0.0
        else:
            next_v = values[t + 1]
        g = reward[t] + cfg.gamma * next_v - values[t] + gl * g
        adv[t], ret[t] = g, g + values[t]
    return adv, ret


def test_gae_matches_scalar_loop():
    a = mk()
    rng = np.random.default_rng(0)
    T, N = 64, 29
    rew = rng.standard_normal((T, N)).astype(np.float32)
    hit = (rng.random((T, N)) < 0.05).astype(np.float32)
    v = rng.standard_normal((T + 1, N)).astype(np.float32)
    done = rng.random((T, N)) < 0.03
    adv, adv_atk, adv_def, ret, def_ret = a._gae_all(rew, hit, None, v, None, None, done)
    assert np.array_equal(adv, adv_atk) and not adv_def.any() and not def_ret.any()
    for e in range(N):
        ra, rr = gae_reference(a.config, rew[:, e], hit[:, e], v[:, e], done[:, e])
        assert np.allclose(adv[:, e], ra, atol=1e-5) and np.allclose(ret[:, e], rr, atol=1e-5), e
    print(f"  GAE: vectorized == per-env loop ({T}x{N}, {int(hit.sum())} hits, {int(done.sum())} ends)")


def test_return_is_damage_before_death():
    a = mk(gae_lambda=1.0, target_entropy=0.0)
    dmg = np.array([1, 2, 0, 5, 3, 4, 6, 7], np.float32)[:, None]
    hit = np.array([0, 0, 0, 1, 0, 0, 1, 0], np.float32)[:, None]
    done = np.array([0, 0, 0, 0, 0, 0, 1, 0], bool)[:, None]
    roll = {"dmg": dmg, "hit": hit, "done": done, "lp": np.zeros((8, 1), np.float32),
            "lp_a": np.zeros((8, 1), np.float32), "committed": np.zeros((8, 1), bool)}
    r = a.soft_reward(roll, alpha=0.0)
    _, _, _, ret, _ = a._gae_all(r, hit, None, np.zeros((9, 1), np.float32), None, None, done)
    # the hit at 3 is survived (its 5 counts); the one at 6 kills (its 6 is a
    # trade); 7 starts the next episode and bootstraps from V = 0
    assert ret[:, 0].tolist() == [15, 14, 12, 12, 7, 4, 0, 7], ret[:, 0].tolist()
    print(f"  return = damage before death: {ret[:, 0].tolist()}")


def test_soft_reward_and_alpha():
    a = mk(target_entropy=1.5)
    roll = {"dmg": np.zeros((2, 2), np.float32), "hit": np.zeros((2, 2), np.float32),
            "done": np.zeros((2, 2), bool),
            "lp": np.array([[-1.5, -3.0], [-2.5, -1.0]], np.float32),
            "lp_a": np.array([[0.0, -1.5], [-1.0, 0.0]], np.float32),
            "committed": np.array([[False, True], [True, False]])}
    r = a.soft_reward(roll, alpha=2.0)
    # effective -log pi: 1.5, 1.5 (3.0 less the committed action head's 1.5), 1.5, 1.0
    assert np.allclose(r, [[0.0, 0.0], [0.0, -1.0]]), r
    lo = a.log_alpha
    a.adapt_alpha(1.0)          # below target: alpha grows
    assert a.log_alpha > lo
    a.adapt_alpha(3.0)
    a.adapt_alpha(3.0)          # above target: it shrinks back past the start
    assert a.log_alpha < lo
    print("  soft reward: zero at the target, commit steps drop the action head, alpha tracks")


if __name__ == "__main__":
    test_gae_matches_scalar_loop()
    test_return_is_damage_before_death()
    test_soft_reward_and_alpha()
    print("PASS")
