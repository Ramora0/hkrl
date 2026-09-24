"""Properties of the PPO update (ppo.py), on synthetic rollouts in a
RolloutStore -- no sim needed.

  1. The vectorized GAE is bit-identical to a per-env scalar loop (both
     float32, same order of operations).
  2. Capturing the update leaves no trace: the warm-up's real Adam steps are
     undone, so weights and optimizer state are bit-identical afterwards, for
     an optimizer that already has state and for a fresh one.
  3. The captured update trains like the same steps run eagerly, with the
     same permutations and mirror coins: weights and metrics agree, with even
     minibatches and a ragged last one. Rows are padded to a bucket (16) the
     eager steps do not see.
  4. A second row bucket's capture never moves .grad (every captured update
     has those addresses baked in), and graph == eager holds across buckets.
  5. A checkpoint round-trips the policy and the normalizers exactly.

    python tests/train/test_ppo.py      (or pytest tests/train)
"""
import os
import tempfile

import numpy as np
import torch

import _paths  # noqa: F401
from config import Config
from model import ACT_KEYS
from observation import GS, Observation, mirror_stats
from ppo import MB_METRICS, PPO
from rollout import RolloutStore
from train import make_boss_state

DEV = torch.device("cuda")
BOSS = "GG_Hornet_1"


def mk_cfg(**kw):
    return Config(**{"seq_len": 8, "chunks_per_batch": 8, "train_iters": 2,
                     "boss_levels": BOSS, **kw})


def mk_rollout(cfg, T, N, seed=0, c_range=(1, 7)):
    """(roll, store): a rollout's step results and a sealed store of already
    normalized observations, row counts varying per step."""
    rng = np.random.default_rng(seed)
    shapes = {"combat_hb": ((cfg.cap_combat, 14), torch.float32),
              "combat_mask": ((cfg.cap_combat,), torch.float32),
              "combat_kind_ids": ((cfg.cap_combat,), torch.int64),
              "combat_parent_ids": ((cfg.cap_combat,), torch.int64),
              "terrain_hb": ((cfg.cap_terrain_view, 8), torch.float32),
              "terrain_mask": ((cfg.cap_terrain_view,), torch.float32),
              "global_state": ((cfg.global_state_dim,), torch.float32),
              "hx": ((cfg.gru_dim,), torch.float32)}
    (store,), _ = RolloutStore.bank(1, T, N, shapes, DEV)
    max_c = max_t = 1
    for t in range(T):
        C, K = int(rng.integers(*c_range)), int(rng.integers(1, 9))
        max_c, max_t = max(max_c, C), max(max_t, K)
        cm = (rng.random((N, C)) < 0.8).astype(np.float32)
        cm[:, 0] = 1
        tm = (rng.random((N, K)) < 0.9).astype(np.float32)
        gs = rng.standard_normal((N, cfg.global_state_dim)).astype(np.float32)
        gs[:, 6:] = 0.0
        gs[:, 6:GS.COMMIT_LOCKED] = 1.0          # abilities + every action valid
        o = {"combat_hb": rng.standard_normal((N, C, 14)).astype(np.float32) * cm[..., None],
             "combat_mask": cm,
             "combat_kind_ids": (rng.integers(1, 50, (N, C)) * cm).astype(np.int64),
             "combat_parent_ids": (rng.integers(1, 50, (N, C)) * cm).astype(np.int64),
             "terrain_hb": rng.standard_normal((N, K, 8)).astype(np.float32) * tm[..., None],
             "terrain_mask": tm, "global_state": gs}
        for k, v in o.items():
            dst = store.buf[k][t]
            dst[(slice(None), slice(0, v.shape[1])) if v.ndim > 1 else slice(None)] = \
                torch.from_numpy(v).to(DEV)
        store.hx[t] = torch.from_numpy(
            (0.1 * rng.standard_normal((N, cfg.gru_dim))).astype(np.float32)).to(DEV)
    store.seal(max_c, max_t)
    sparse = lambda p: (rng.random((T, N)) * (rng.random((T, N)) < p)).astype(np.float32)  # noqa: E731
    roll = {"actions": {"movement": rng.integers(0, 3, (T, N)),
                        "direction": rng.integers(0, 3, (T, N)),
                        "action": rng.integers(0, 8, (T, N)),
                        "jump": rng.integers(0, 2, (T, N))},
            "lp": (-5.0 + 0.3 * rng.standard_normal((T, N))).astype(np.float32),
            "lp_a": (-2.0 + 0.3 * rng.standard_normal((T, N))).astype(np.float32),
            "dmg": sparse(0.05) * 10, "hit": sparse(0.03),
            "heal": np.zeros((T, N), np.float32),
            "v_atk": (0.1 * rng.standard_normal((T + 1, N))).astype(np.float32),
            "v_def": (0.1 * rng.standard_normal((T + 1, N))).astype(np.float32),
            "done": rng.random((T, N)) < 0.01, "committed": rng.random((T, N)) < 0.2}
    return roll, store


def mk_agent(cfg, seed=0):
    torch.manual_seed(seed)
    a = PPO(cfg)
    a.reset_hidden(4)
    # a non-trivial mirror: normalizer means away from 0
    a.obs_normalizer.mean[:] = 0.3
    a.terrain_normalizer.mean[:] = -0.4
    return a


def job(agent, roll, store):
    N = roll["dmg"].shape[1]
    return dict(roll=roll, store=store, D_per_env=np.ones(N, np.float32),
                boss_per_env=[BOSS] * N,
                mstats=mirror_stats(agent.obs_normalizer, agent.combat_normalizer,
                                    agent.terrain_normalizer))


def graph_update(agent, roll, store, vstate, seed):
    np.random.seed(seed)
    return agent.train_on_rollout(value_var_state=vstate, **job(agent, roll, store))


def eager_update(agent, roll, store, vstate, seed):
    """The same update step by step, outside any graph: the same rollout
    tensors (unpadded), permutations and mirror coins."""
    np.random.seed(seed)
    j = job(agent, roll, store)
    src, _ = agent.rollout_src(roll, store, j["D_per_env"], j["boss_per_env"], vstate)
    n = src["adv"].shape[0]
    perms, coins = agent.draw_update(n)
    CPB = agent.config.chunks_per_batch
    acc = torch.zeros(len(MB_METRICS), device=DEV)
    steps = 0
    for it in range(perms.shape[0]):
        for k, start in enumerate(range(0, n, CPB)):
            idx = torch.from_numpy(perms[it, start:start + CPB]).to(DEV)
            mb = agent._mirror_mb(agent._gather_mb(src, idx), j["mstats"],
                                  torch.tensor(coins[it, k], device=DEV))
            acc += agent._step_eager(mb)
            steps += 1
    return dict(zip(MB_METRICS, (acc / steps).tolist()))


def params_of(agent):
    return [p.detach().clone() for p in agent.policy.parameters()]


def state_of(agent):
    return {id(p): {k: v.clone() for k, v in agent.optimizer.state[p].items()}
            for p in agent.policy.parameters() if p in agent.optimizer.state}


def rel_diff(pa, pb, p0):
    step = torch.sqrt(sum(((x - y) ** 2).sum() for x, y in zip(pa, p0)))
    return float(torch.sqrt(sum(((x - y) ** 2).sum() for x, y in zip(pa, pb))) / step)


# ==========================================================================
def gae_reference(cfg, damage_landed, hits_taken, hp_healed, values_atk, values_def, D,
                  dones):
    """One env's decomposed GAE as a scalar loop."""
    T = len(damage_landed)
    gamma, lam = cfg.gamma, cfg.gae_lambda
    out = [np.empty(T, np.float32) for _ in range(5)]
    g_atk = g_def = g_atk_pol = g_def_pol = 0.0
    for t in reversed(range(T)):
        if dones[t]:
            next_vatk = next_vdef = 0.0
            g_atk = g_def = g_atk_pol = g_def_pol = 0.0
        else:
            next_vatk, next_vdef = values_atk[t + 1], values_def[t + 1]
        delta_atk = damage_landed[t] + gamma * next_vatk - values_atk[t]
        delta_def = hits_taken[t] + gamma * next_vdef - values_def[t]
        g_atk_pol = cfg.attack_weight * delta_atk / D + cfg.heal_coef * hp_healed[t] \
            + gamma * lam * g_atk_pol
        g_def_pol = -delta_def + gamma * lam * g_def_pol
        g_atk = delta_atk + gamma * lam * g_atk
        g_def = delta_def + gamma * lam * g_def
        for arr, v in zip(out, (g_atk_pol + g_def_pol, g_atk_pol, g_def_pol,
                                g_atk + values_atk[t], g_def + values_def[t])):
            arr[t] = v
    return out


def test_gae_vectorized_is_identical():
    cfg = mk_cfg()
    a = PPO(cfg)
    rng = np.random.default_rng(0)
    T, N = 64, 37
    dmg = (rng.random((T, N)) * (rng.random((T, N)) < 0.1)).astype(np.float32)
    hit = (rng.random((T, N)) < 0.05).astype(np.float32)
    heal = (rng.random((T, N)) < 0.02).astype(np.float32)
    va = rng.standard_normal((T + 1, N)).astype(np.float32)
    vd = rng.standard_normal((T + 1, N)).astype(np.float32)
    D = rng.uniform(0.5, 5.0, N).astype(np.float32)
    done = rng.random((T, N)) < 0.03
    vec = a._gae_all(dmg, hit, heal, va, vd, D, done)
    for e in range(N):
        ref = gae_reference(cfg, dmg[:, e], hit[:, e], heal[:, e], va[:, e], vd[:, e],
                            float(D[e]), done[:, e])
        for x, y in zip(vec, ref):
            assert np.array_equal(x[:, e], y), f"env {e}: max|diff| {np.abs(x[:, e] - y).max()}"
    print(f"  GAE: vectorized == per-env loop, bit for bit ({T}x{N}, {int(done.sum())} ends)")


def test_capture_leaves_no_trace():
    cfg = mk_cfg()
    roll, store = mk_rollout(cfg, T=16, N=8)
    # (a) an optimizer that already has state
    a = mk_agent(cfg)
    graph_update(a, roll, store, make_boss_state(cfg), 1)
    src, _ = a.rollout_src(roll, store, np.ones(8, np.float32), [BOSS] * 8,
                           make_boss_state(cfg))
    p0, s0 = params_of(a), state_of(a)
    a._capture_update_graph(src)
    torch.cuda.synchronize()
    assert all(torch.equal(x, y) for x, y in zip(p0, params_of(a))), "capture moved weights"
    s1 = state_of(a)
    assert s0.keys() == s1.keys()
    for pid in s0:
        for k in s0[pid]:
            assert torch.equal(s0[pid][k], s1[pid][k]), f"capture changed Adam {k}"
    # (b) a fresh optimizer: whatever state the warm-up created reads as fresh
    b = mk_agent(cfg)
    p0 = params_of(b)
    b._capture_update_graph(src)
    torch.cuda.synchronize()
    assert all(torch.equal(x, y) for x, y in zip(p0, params_of(b))), "capture moved weights"
    for st in state_of(b).values():
        for k, v in st.items():
            assert not v.any(), f"fresh Adam {k} not zero after capture"
    print("  capture: weights and Adam state bit-identical afterwards (warm, fresh)")


def compare_paths(N, label):
    cfg = mk_cfg(lr=1e-3)
    roll, store = mk_rollout(cfg, T=32, N=N, seed=1)
    ae, ag = mk_agent(cfg), mk_agent(cfg)
    ve, vg = make_boss_state(cfg), make_boss_state(cfg)
    p_init = params_of(ae)
    for ep in range(3):
        me = eager_update(ae, roll, store, ve, 100 + ep)
        mg = graph_update(ag, roll, store, vg, 100 + ep)
    rel = rel_diff(params_of(ae), params_of(ag), p_init)
    mdiff = max(abs(me[k] - mg[k]) / (abs(me[k]) + 1e-6) for k in MB_METRICS)
    assert rel < 2e-3, f"{label}: graph and eager weights diverged, rel {rel:.2e}"
    assert mdiff < 1e-3, f"{label}: metrics diverged, rel {mdiff:.2e}"
    assert all(np.isfinite(mg[k]) for k in MB_METRICS)
    print(f"  graph == eager ({label}): 3 updates x {mg['n_updates']} steps, "
          f"|dW|/|update| {rel:.1e}, metrics rel {mdiff:.1e}")


def test_graph_matches_eager():
    compare_paths(N=8, label="even minibatches")        # 32 chunks / 8
    compare_paths(N=9, label="ragged last minibatch")   # 36 chunks / 8


def test_new_bucket_keeps_grads():
    cfg = mk_cfg(lr=1e-3)
    narrow = mk_rollout(cfg, T=32, N=8, seed=5)
    wide = mk_rollout(cfg, T=32, N=8, seed=6, c_range=(17, 25))
    ae, ag = mk_agent(cfg), mk_agent(cfg)
    ve, vg = make_boss_state(cfg), make_boss_state(cfg)
    p_init, ptrs = params_of(ae), None
    for i, (roll, store) in enumerate([narrow, wide, narrow, wide]):
        eager_update(ae, roll, store, ve, 300 + i)
        graph_update(ag, roll, store, vg, 300 + i)
        now = [p.grad.data_ptr() for p in ag.policy.parameters()]
        ptrs = ptrs or now
        assert now == ptrs, f"update {i}: a capture moved .grad"
    assert len(ag._update_graphs) == 2, list(ag._update_graphs)
    rel = rel_diff(params_of(ae), params_of(ag), p_init)
    assert rel < 2e-3, f"graph and eager diverged across buckets, rel {rel:.2e}"
    print(f"  two buckets (16, 32) alternating: .grad never moved, graph == eager "
          f"|dW|/|update| {rel:.1e}")


def test_checkpoint_roundtrip():
    cfg = mk_cfg()
    roll, store = mk_rollout(cfg, T=16, N=8)
    a = mk_agent(cfg)
    graph_update(a, roll, store, make_boss_state(cfg), 2)
    a.obs_normalizer.update(np.random.default_rng(3).standard_normal((50, 6)))
    obs = Observation(**{k: v[0] for k, v in store.buf.items()})
    hx = store.hx[0]

    def fwd(agent):
        with torch.no_grad():
            out = agent.actor_policy.get_action_and_value(obs, hx=hx, actions={
                k: torch.from_numpy(roll["actions"][k][0]).to(DEV) for k in ACT_KEYS})
        return [x for x in out[1:] if torch.is_tensor(x)]

    with tempfile.TemporaryDirectory() as d:
        ck = os.path.join(d, "a.pth")
        assert a.save_checkpoint(ck, ["unknown", "terrain", "x"], make_boss_state(cfg), 123)
        b = mk_agent(cfg, seed=1)
        got = b.load_checkpoint(ck)
    assert got["env_steps"] == 123 and got["kind_vocab_i2s"][-1] == "x"
    a.sync_actor()
    assert all(torch.equal(x, y) for x, y in zip(fwd(a), fwd(b))), "forward differs"
    for n in ("obs_normalizer", "combat_normalizer", "terrain_normalizer"):
        x, y = getattr(a, n), getattr(b, n)
        assert np.array_equal(x.mean, y.mean) and np.array_equal(x.var, y.var) \
            and x.count == y.count, n
    print("  checkpoint: policy forward and normalizers identical after a round trip")


if __name__ == "__main__":
    test_gae_vectorized_is_identical()
    test_capture_leaves_no_trace()
    test_graph_matches_eager()
    test_new_bucket_keeps_grads()
    test_checkpoint_roundtrip()
    print("ALL TESTS PASSED")
