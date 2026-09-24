"""Ordering test for the horizontal-mirror augmentation.

The augmentation is applied to tensors that have already been z-scored, but
"mirror" is defined on the raw world-space observation. The two operations
only commute when the running mean of every negated column is exactly zero:

    z            = (x - mu)/sigma
    mirror(z)   != -z                        unless mu == 0
    mirror(z)    = (-x - mu)/sigma = -z - 2*mu/sigma

So the property to assert is that the two orders agree:

    normalize(mirror_raw(x))  ==  mirror(normalize(x))

Plain negation of the z-score is the left-hand side only when
mu == 0; test_plain_negation_fails shows the test catches that.

    python tests/train/test_mirror.py      (or pytest tests/train)
"""
import numpy as np
import torch

import _paths  # noqa: F401
from config import Config
from observation import CB, GS, TR, Observation, mirror_observation, mirror_stats
from ppo import PPO

N, C, K = 6, 5, 9          # batch, combat rows, terrain rows


def mirror_raw(obs: Observation) -> Observation:
    """The definition of the flip, in RAW world space. Deliberately written
    out longhand: it is the reference the z-space implementation must match,
    so it must not share code with it."""
    gs = obs.global_state.copy()
    gs[..., GS.VEL_X] *= -1

    chb = obs.combat_hb.copy()
    chb[..., CB.REL_X] *= -1
    chb[..., CB.VEL_X] *= -1

    thb = obs.terrain_hb.copy()
    thb[..., TR.MX] *= -1
    thb[..., TR.NPX] *= -1
    # hdx is a half-extent, so raw hdx > 0 means "a real segment"; flipping
    # hdy there re-expresses it from the opposite endpoint. Padded rows are
    # all-zero and must stay all-zero.
    flip = thb[..., TR.HDX] > 0
    thb[..., TR.HDY] = np.where(flip, -thb[..., TR.HDY], thb[..., TR.HDY])
    return obs.replace(global_state=gs, combat_hb=chb, terrain_hb=thb)


def mirror_plain_negate(obs, stats):
    """Negating the z-score, right only when mu == 0."""
    gs = obs.global_state.clone()
    gs[..., GS.VEL_X] = -gs[..., GS.VEL_X]
    chb = obs.combat_hb.clone()
    chb[..., CB.REL_X] = -chb[..., CB.REL_X]
    chb[..., CB.VEL_X] = -chb[..., CB.VEL_X]
    thb = obs.terrain_hb.clone()
    thb[..., TR.MX] = -thb[..., TR.MX]
    thb[..., TR.NPX] = -thb[..., TR.NPX]
    thb[..., TR.HDY] = torch.where(thb[..., TR.HDX] > 0,
                                   -thb[..., TR.HDY], thb[..., TR.HDY])
    return obs.replace(global_state=gs, combat_hb=chb, terrain_hb=thb)


def make_raw(rng, cfg, shift):
    """A raw observation whose x-columns sit far from the origin. `shift` is
    in units of the column's own sigma, so shift=0 is the (only) case the old
    formula got right and shift=3 is a 3-sigma offset."""
    def col(scale):
        return (rng.standard_normal((N, C)) * scale + shift * scale)

    chb = rng.standard_normal((N, C, cfg.combat_feature_dim)).astype(np.float32)
    chb[..., CB.REL_X] = col(3.0)
    chb[..., CB.VEL_X] = col(0.4)
    chb[..., CB.HP_RAW] = np.abs(chb[..., CB.HP_RAW]) * 100
    chb[..., CB.HP_MAX_RAW] = 900.0
    cmask = np.ones((N, C), np.float32)
    cmask[:, -2:] = 0.0                     # two padded rows per batch item
    chb *= cmask[..., None]

    thb = rng.standard_normal((N, K, cfg.terrain_feature_dim)).astype(np.float32)
    thb[..., TR.MX] = rng.standard_normal((N, K)) * 6.0 + shift * 6.0
    thb[..., TR.NPX] = rng.standard_normal((N, K)) * 5.0 + shift * 5.0
    # Half-extents are non-negative by construction; keep them away from the
    # exact-zero knife edge so the hdx > 0 branch is not float-fragile.
    thb[..., TR.HDX] = np.abs(rng.standard_normal((N, K))) * 2.0 + 0.25
    thb[..., TR.HDY] = rng.standard_normal((N, K)) * 2.0 + shift * 2.0
    tmask = np.ones((N, K), np.float32)
    tmask[:, -3:] = 0.0
    thb *= tmask[..., None]

    gs = rng.standard_normal((N, cfg.global_state_dim)).astype(np.float32)
    gs[..., GS.VEL_X] = rng.standard_normal(N) * 8.0 + shift * 8.0
    gs[..., 6:] = (gs[..., 6:] > 0).astype(np.float32)   # flags are binary

    return Observation(
        combat_hb=chb.astype(np.float32), combat_mask=cmask,
        combat_kind_ids=np.zeros((N, C), np.int64),
        combat_parent_ids=np.zeros((N, C), np.int64),
        terrain_hb=thb.astype(np.float32), terrain_mask=tmask,
        global_state=gs.astype(np.float32))


def to_torch(obs, prepared):
    gs, chb, thb = prepared
    return Observation(
        combat_hb=torch.from_numpy(chb), combat_mask=torch.from_numpy(obs.combat_mask),
        combat_kind_ids=torch.from_numpy(obs.combat_kind_ids),
        combat_parent_ids=torch.from_numpy(obs.combat_parent_ids),
        terrain_hb=torch.from_numpy(thb), terrain_mask=torch.from_numpy(obs.terrain_mask),
        global_state=torch.from_numpy(gs))


def run(use_old=False):
    """Failure strings over a centred (0 sigma) and a shifted (3 sigma) batch."""
    fails = []
    agent = PPO(Config())       # only the normalizers and _prepare are under test
    rng = np.random.default_rng(20260901)
    cfg = agent.config
    for shift in (0.0, 3.0):
        # Train the running normalizer on its own batches, so mu is a real
        # running mean and not the test batch's exact mean.
        for _ in range(6):
            agent._prepare(make_raw(rng, cfg, shift), update=True)
        raw = make_raw(rng, cfg, shift)
        stats = mirror_stats(agent.obs_normalizer, agent.combat_normalizer,
                             agent.terrain_normalizer)
        # A: mirror in raw space, then normalize.
        ref = to_torch(raw, agent._prepare(mirror_raw(raw)))
        # B: normalize, then mirror in z space (what training does).
        norm = to_torch(raw, agent._prepare(raw))
        got = (mirror_plain_negate if use_old else mirror_observation)(norm, stats)
        for name in ("global_state", "combat_hb", "terrain_hb"):
            d = float((getattr(ref, name) - getattr(got, name)).abs().max())
            if d > 2e-4:
                fails.append(f"shift={shift}: {name} max|diff| {d:.4f}")
    return fails


def test_mirror_commutes_with_normalization():
    fails = run()
    assert not fails, fails
    print("  normalize(mirror(x)) == mirror(normalize(x)) at 0 and 3 sigma offsets")


def test_plain_negation_fails():
    """The test's teeth: plain negation of the z-score must fail it."""
    assert run(use_old=True), "plain negation passed: the test is blind"
    print("  plain negation fails it, as it must")


if __name__ == "__main__":
    test_mirror_commutes_with_normalization()
    test_plain_negation_fails()
    print("ALL TESTS PASSED")
