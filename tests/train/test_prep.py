"""The actor's preprocessing kernels (rollout.PrepCuda) against the host path
the evals use (sim_env.make_obs + PPO._prepare), on the same raw buffers,
over a run of frames so the running statistics accumulate:

  * masks, ids, the masked combat rows, the compacted terrain rows, the
    committed flag and the unknown-id count bit for bit; the z-scores to
    float32 rounding; the statistics to 1e-12 relative (and back through the
    pull into the numpy normalizers). The frames include zero live rows (the
    Welford update is skipped), terrain entirely out of view, stale garbage
    past the live counts, and unknown ids.
  * An overflow frame (more terrain rows in view than the fixed width) is
    counted, not raised, and the rows kept are the first ones in order.

    python tests/train/test_prep.py      (or pytest tests/train)
"""
import numpy as np
import torch

import _paths  # noqa: F401
from config import Config
from observation import CB, GS, TR, VIEW_H, VIEW_W
from ppo import PPO
from rollout import RAW_KEYS, DeviceNorms, PrepCuda
from sim_env import make_obs
from sim_worker import layout, views

DEV = torch.device("cuda")


def mk_cfg():
    return Config(n_envs=16, cap_combat=16, cap_terrain=48, cap_terrain_view=24)


class RawFrames:
    """Raw buffers in the pool's layout, filled with seeded synthetic frames:
    0-cap live combat rows, terrain in and out of view, the odd 0 id, commit
    flags, and finite garbage past every live count."""

    def __init__(self, cfg, seed):
        self.cfg = cfg
        self.n = int(cfg.n_envs)
        lay, nbytes = layout(self.n, cfg.cap_combat, cfg.cap_terrain)
        self.arrays = views(np.zeros(nbytes, np.uint8), lay)
        self.rng = np.random.default_rng(seed)

    def fill(self, kind="normal"):
        """kind: normal | empty (no live rows) | out_of_view | overflow (every
        live terrain row in view)."""
        a, r, n, cfg = self.arrays, self.rng, self.n, self.cfg
        a["combat"][:] = r.normal(0, 30, a["combat"].shape)
        a["combat_kind"][:] = r.integers(0, 60, a["combat_kind"].shape)
        a["combat_parent"][:] = r.integers(0, 60, a["combat_parent"].shape)
        a["terrain"][:] = r.normal(0, 30, a["terrain"].shape)
        nc = r.integers(0, cfg.cap_combat + 1, n)
        nt = r.integers(0, cfg.cap_terrain + 1, n)
        if kind == "empty":
            nc[:] = 0
            nt[:] = 0
        a["n_combat"][:] = nc
        a["n_terrain"][:] = nt
        for i in range(n):
            k = int(nc[i])
            a["combat"][i, :k] = r.normal(3, 5, (k, 14))
            a["combat"][i, :k, 11:13] = r.uniform(-2, 900, (k, 2))   # hp: log1p'd
            a["combat_kind"][i, :k] = r.integers(1, 60, k) * (r.random(k) > 0.05)
            a["combat_parent"][i, :k] = r.integers(1, 60, k)
            m = int(nt[i])
            t = r.normal(0, 4, (m, 8))
            t[:, TR.NPX] = r.uniform(-25, 25, m)
            t[:, TR.NPY] = r.uniform(-14, 14, m)
            if kind == "out_of_view":
                t[:, TR.NPX] = 40.0
            if kind == "overflow":
                t[:, TR.NPX] = t[:, TR.NPY] = 0.0
            else:          # keep the in-view rows within the fixed width
                seen = np.nonzero((np.abs(t[:, TR.NPX]) <= VIEW_W / 2)
                                  & (np.abs(t[:, TR.NPY]) <= VIEW_H / 2))[0]
                t[seen[cfg.cap_terrain_view:], TR.NPX] = 40.0
            a["terrain"][i, :m] = t
        gs = r.normal(0, 3, (n, cfg.global_state_dim))
        gs[:, 6:] = r.random((n, cfg.global_state_dim - 6)) < 0.3
        a["global_state"][:] = gs

    def device(self):
        return {k: torch.from_numpy(np.ascontiguousarray(self.arrays[k])).to(DEV)
                for k in RAW_KEYS}


def _pad(x, width):
    out = np.zeros((x.shape[0], width) + x.shape[2:], x.dtype)
    out[:, :x.shape[1]] = x
    return out


def _ulps(a, b):
    """Largest distance in float32 units in the last place."""
    a, b = np.asarray(a, np.float32), np.asarray(b, np.float32)
    ia = a.view(np.int32).astype(np.int64)
    ib = b.view(np.int32).astype(np.int64)
    ia = np.where(ia < 0, np.int64(-2 ** 31) - ia, ia)       # monotone order
    ib = np.where(ib < 0, np.int64(-2 ** 31) - ib, ib)
    return int(np.abs(ia - ib).max()) if a.size else 0


def test_prep_matches_host():
    cfg = mk_cfg()
    C, K = cfg.cap_combat, cfg.cap_terrain_view
    # columns _prepare passes through untouched: they pin masking and compaction
    cb_raw = [j for j in range(cfg.combat_feature_dim)
              if j >= cfg.combat_normalized_dims and not CB.HP_RAW <= j <= CB.HP_MAX_RAW]
    tr_raw = list(range(cfg.terrain_normalized_dims, cfg.terrain_feature_dim))
    frames = RawFrames(cfg, seed=1)
    host = PPO(cfg)                       # numpy normalizers: the reference
    dev = DeviceNorms(PPO(cfg), DEV)
    dev.push_if_changed()
    prep = PrepCuda(cfg, C, K, cfg.n_envs, DEV)
    counters = {"unknown_rows": 0}
    unknown_gpu, worst = 0, 0
    kinds = ["normal"] * 3 + ["empty", "normal", "out_of_view", "normal", "normal"]
    for kind in kinds:
        frames.fill(kind)
        obs = make_obs(cfg, frames.arrays, counters)
        gs, chb, thb = host._prepare(obs, update=True)
        o, aux = prep(frames.device(), dev)
        assert int(aux["overflow"]) == 0
        unknown_gpu += int(aux["unknown"])
        g = {k: v.cpu().numpy() for k, v in vars(o).items()}
        for k in ("combat_mask", "combat_kind_ids", "combat_parent_ids"):
            assert np.array_equal(g[k], _pad(getattr(obs, k), C)), (kind, k)
        assert np.array_equal(g["terrain_mask"], _pad(obs.terrain_mask, K)), kind
        assert np.array_equal(g["combat_hb"][..., cb_raw], _pad(obs.combat_hb, C)[..., cb_raw]), kind
        assert np.array_equal(g["terrain_hb"][..., tr_raw], _pad(obs.terrain_hb, K)[..., tr_raw]), kind
        committed = ((obs.global_state[:, GS.COMMIT_LOCKED] > 0.5)
                     | (obs.global_state[:, GS.COMMIT_RELEASING] > 0.5))
        assert np.array_equal(aux["committed"].cpu().numpy(), committed), kind
        worst = max(worst, _ulps(g["global_state"], gs),
                    _ulps(g["combat_hb"], _pad(chb, C)), _ulps(g["terrain_hb"], _pad(thb, K)))
        for i, n in enumerate((host.obs_normalizer, host.combat_normalizer,
                               host.terrain_normalizer)):
            d = n.mean.shape[0]
            for name, ref, got in (("mean", n.mean, dev.mean[i, :d]),
                                   ("var", n.var, dev.var[i, :d])):
                rel = np.abs(got.cpu().numpy() - ref) / np.maximum(np.abs(ref), 1e-300)
                assert rel.max() < 1e-12, f"{kind}: normalizer {i} {name} rel {rel.max():.2e}"
            assert abs(float(dev.count[i]) - n.count) <= 1e-12 * n.count, (kind, i)
    assert unknown_gpu == counters["unknown_rows"] > 0, (unknown_gpu, counters)
    assert worst <= 2, f"normalized values differ by {worst} ulp"
    # the pull into the numpy objects (what mirror_stats / checkpoints read)
    dev.pull_start()
    torch.cuda.synchronize()
    dev.pull_finish()
    for n_dev, n_host in zip(dev._norms(), (host.obs_normalizer, host.combat_normalizer,
                                             host.terrain_normalizer)):
        assert np.allclose(n_dev.mean, n_host.mean, rtol=1e-12, atol=0)
        assert np.allclose(n_dev.var, n_host.var, rtol=1e-12, atol=0)
        assert n_dev.count == n_host.count
    assert not dev._changed() and not dev.push_if_changed()
    print(f"  PrepCuda == make_obs + _prepare over {len(kinds)} frames (incl. empty, out of "
          f"view): masks / ids / rows / unknown ids ({unknown_gpu}) exact, z-scores within "
          f"{worst} ulp, statistics within 1e-12")


def test_overflow_is_counted():
    cfg = mk_cfg()
    K = cfg.cap_terrain_view
    fr = RawFrames(cfg, seed=2)
    fr.fill("overflow")
    nt = fr.arrays["n_terrain"].copy()
    obs = make_obs(cfg, fr.arrays, {"unknown_rows": 0})
    dev = DeviceNorms(PPO(cfg), DEV)
    dev.push_if_changed()
    o, aux = PrepCuda(cfg, cfg.cap_combat, K, cfg.n_envs, DEV)(fr.device(), dev)
    over = int((nt > K).sum())
    assert over > 0 and int(aux["overflow"]) == over, (over, int(aux["overflow"]))
    tr_raw = list(range(cfg.terrain_normalized_dims, cfg.terrain_feature_dim))
    th = o.terrain_hb.cpu().numpy()
    assert np.array_equal(th[..., tr_raw], obs.terrain_hb[:, :K][..., tr_raw]), \
        "overflow: kept rows not the first K"
    assert np.array_equal(o.terrain_mask.cpu().numpy(), obs.terrain_mask[:, :K])
    print(f"  overflow: {over} envs over {K} terrain rows counted, the first {K} kept "
          f"rows exactly the host's")


if __name__ == "__main__":
    test_prep_matches_host()
    test_overflow_is_counted()
    print("ALL TESTS PASSED")
