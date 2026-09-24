"""Properties of the rollout queue (rollout.py).

  1. The worker's substep (sim_worker.Worker.step_subset), on the real sim:
     stepping a subset of envs changes only those envs' rows, step / done
     slots and seq -- every other env's rows stay bit-identical -- and each
     stepped env's rows equal those of a lockstep pool (same seed, same
     actions) after the same number of steps. Random subsets spanning both
     workers, 30 steps per env, so the envs of one worker drift apart and the
     sims left out of a substep provably did not move.
  2. The zero-copy hand-off under stress. Probe workers (probe_worker.
     ProbeWorker) stamp each observation with its serial number in five
     places, written with random pauses, at random per-env step times, while
     a random GPU delay (torch.cuda._sleep) is queued before every replay.
     In every stored step all copies agree (no row was read mid-overwrite);
     serial numbers run consecutively per env inside and across stores
     (every result forwarded exactly once, none skipped); each step's result
     is the one that produced the next observation.
  3. The stores. Exactly T steps per env; rerunning the policy version that
     the queue recorded for each step (a fake learner nudges the weights on
     every "update") on the stored observation and hx-before-forward
     reproduces the recorded action, log-probs and values, and the next
     step's stored hx (zeros after a done), across store boundaries too; each
     store's bootstrap value IS the next store's step-0 value (one forward);
     the running statistics counted every forwarded row exactly once and no
     padding row.
  4. The real loop (Hornet, 4 workers, a greedy eval in the middle,
     checkpoints): the weights move, metrics are finite, the actor equals
     the learner at every handover, the checkpoints load.

    python tests/train/test_queue.py      (or pytest tests/train)
"""
import os
import tempfile
import threading
import time

import numpy as np
import torch

import _paths  # noqa: F401
import probe_worker
import train as train_mod
from config import Config
from model import ACT_KEYS
from observation import Observation
from ppo import NO_UPDATE, PPO
from rollout import RAW_KEYS, Actor, RolloutQueue, RolloutStore
from sim_env import SimPool

perf = time.perf_counter


def mk_cfg(**kw):
    return Config(**{"boss_levels": "GG_Hornet_1", "n_envs": 8, "pool_workers": 2,
                     "seq_len": 16, "chunks_per_batch": 8, "wandb": False,
                     "game_eval": False, **kw})


# ==========================================================================
# 1. substep == lockstep, per env
# ==========================================================================
def step_subset(env, envs, timeout=60.0):
    """Step global envs `envs` (sorted) through the substep path, with the
    actions already in act_arr, and wait until every one has published."""
    envs = np.asarray(envs, np.int64)
    target = env.seq_arr[envs] + 1
    wm0 = env.wmsg_arr[env.w_lo].copy()
    for w in np.unique(env.env_worker[envs]):
        sel = envs[env.env_worker[envs] == w]
        env.submit_subset(int(w), sel - env.w_lo[w])
    deadline = time.monotonic() + timeout
    while (env.seq_arr[envs] < target).any():
        wm = env.wmsg_arr[env.w_lo]
        for w in np.flatnonzero(wm != wm0):
            env.serve_messages(int(w), wm[w] - wm0[w])
            wm0[w] = wm[w]
        assert time.monotonic() < deadline, "workers did not publish in time"
        env.wait_signal(0.05)


def _snap(env):
    return {k: env.arrays[k].copy() for k in RAW_KEYS}


def _same_env_rows(a, rb, e, i2s_a, i2s_b, tag):
    """Env e's rows in snapshot a and B's rows rb: floats and counts
    bit-identical, vocab ids equal as strings (each pool numbers late
    strings in its own order)."""
    for k in RAW_KEYS:
        if k in ("combat_kind", "combat_parent"):
            n = int(a["n_combat"][e])
            sa = [i2s_a[i] for i in a[k][e, :n]]
            sb = [i2s_b[i] for i in rb[k][:n]]
            assert sa == sb, (tag, k)
        else:
            assert np.array_equal(a[k][e], rb[k]), (tag, k)


def test_substep_matches_lockstep():
    cfg = mk_cfg()
    # Long enough for episode ends (none in the first 400 steps of 8 envs on
    # Hornet): the auto-reset inside a substep. Pool A steps in lockstep up to
    # 24 steps ahead; B's envs catch up in random subsets and each is compared
    # the moment it reaches a step A has a snapshot of.
    N, K, AHEAD = cfg.n_envs, 1500, 24
    rng = np.random.default_rng(0)
    acts = np.stack([rng.integers(0, n, (K, N)) for n in
                     (cfg.movement_n, cfg.direction_n, cfg.action_n, cfg.jump_n)],
                    -1).astype(np.int32)
    A = SimPool(cfg, seed=11)
    B = SimPool(cfg, seed=11)
    try:
        A.reset()
        B.reset()
        ref = {0: _snap(A)}
        kA = 0
        cnt = np.zeros(N, np.int64)
        s0 = _snap(B)
        for e in range(N):
            _same_env_rows(ref[0], {k: v[e] for k, v in s0.items()}, e,
                           A.vocab_i2s(), B.vocab_i2s(), f"env {e} reset")
        n_sub, n_done, n_cmp = 0, 0, N
        while cnt.min() < K:
            while kA < K and kA - cnt.min() < AHEAD:
                A.step(acts[kA])
                kA += 1
                ref[kA] = _snap(A)
            cand = np.flatnonzero(cnt < kA)
            S = np.sort(rng.choice(cand, size=int(rng.integers(1, len(cand) + 1)),
                                   replace=False))
            B.act_arr[S] = acts[cnt[S], S]
            before, seq0 = _snap(B), B.seq_arr.copy()
            step_subset(B, S)
            after = _snap(B)
            others = np.setdiff1d(np.arange(N), S)
            for k in RAW_KEYS:
                assert np.array_equal(after[k][others], before[k][others]), \
                    f"substep {n_sub}: {k} changed outside the subset"
            dseq = B.seq_arr - seq0
            assert (dseq[S] == 1).all() and (dseq[others] == 0).all(), dseq
            cnt[S] += 1
            n_done += int(after["done"][S].sum())
            i2s_a, i2s_b = A.vocab_i2s(), B.vocab_i2s()
            for e in S:
                _same_env_rows(ref[int(cnt[e])], {k: v[e] for k, v in after.items()}, e,
                               i2s_a, i2s_b, f"env {e} step {cnt[e]}")
                n_cmp += 1
            for k in [k for k in ref if k < cnt.min()]:
                del ref[k]
            n_sub += 1
        assert n_done > 0, "no episode ended: the substep's auto-reset went untested"
    finally:
        A.close()
        B.close()
    print(f"  substep == lockstep: {n_sub} random substeps over 2 workers, {N} envs x "
          f"{K} steps ({n_done} episode ends, auto-reset inside a substep); rows "
          f"outside a substep bit-identical, {n_cmp} env-steps == the lockstep "
          f"pool's at the same step")


# ==========================================================================
# 2 + 3. the queue on probe workers
# ==========================================================================
class FakeLearner:
    """AsyncLearner's interface, without training. An update finishes after
    a random delay and nudges the learner's weights, so that every policy
    version is a distinct set of weights (kept in `snap`)."""

    def __init__(self, agent, seed):
        self.agent, self.rng = agent, np.random.default_rng(seed)
        self.busy, self.on_done, self.t_done = False, None, 0.0
        self.version, self.jobs = 0, 0
        self.snap = {0: {k: v.clone() for k, v in agent.policy.state_dict().items()}}

    def submit(self, job, ready):
        assert not self.busy, "one update at a time"
        self.busy = True
        self.jobs += 1
        delay = float(self.rng.uniform(0.0, 0.03))
        self.t_done = perf() + delay
        threading.Timer(delay, lambda: self.on_done and self.on_done()).start()

    def finished(self):
        return perf() >= self.t_done

    def wait(self):
        while perf() < self.t_done:
            time.sleep(0.001)
        self.busy = False
        with torch.no_grad():
            g = torch.Generator(device="cpu").manual_seed(self.version)
            for p in self.agent.policy.parameters():
                p.add_(1e-2 * torch.randn(p.shape, generator=g).to(p.device))
        self.version += 1
        self.snap[self.version] = {k: v.clone()
                                   for k, v in self.agent.policy.state_dict().items()}
        return dict(NO_UPDATE), 0.0

    def close(self):
        pass


def _stored(store, T):
    return {k: v[:T].cpu().numpy() for k, v in store.buf.items()} | \
        {"hx": store.hx[:T].cpu().numpy()}


VOCAB_EVERY = 37


def _check_serials(k, s, roll, prev_last, i2s):
    """Test 2 on one store: returns the last serial per env."""
    ser = s["global_state"][..., 6]                        # (T, N)
    # a late vocab string, reconciled mid-substep: the stored id is canonical
    kid = s["combat_kind_ids"][..., 0]
    for t, e in zip(*np.nonzero((ser % VOCAB_EVERY == 0) & (ser > 0))):
        assert i2s[kid[t, e]] == f"probe_{e}_{int(ser[t, e])}", (k, t, e, i2s[kid[t, e]])
    plain = ser % VOCAB_EVERY != 0
    assert np.array_equal(kid[plain], (1 + ser[plain] % 7).astype(kid.dtype)), k
    # every copy of the serial the probe wrote, as the GPU stored it
    assert (s["combat_mask"][..., :3] == 1).all() and (s["combat_mask"][..., 3:] == 0).all()
    for r in range(3):
        assert np.array_equal(s["combat_hb"][:, :, r, 6], ser), f"store {k}: combat row {r} torn"
    assert (s["terrain_mask"][..., :2] == 1).all()
    for r in range(2):
        assert np.array_equal(s["terrain_hb"][:, :, r, 7], ser), f"store {k}: terrain row {r} torn"
    # consecutive: every published result forwarded exactly once, in order
    assert (np.diff(ser, axis=0) == 1).all(), f"store {k}: serials not consecutive"
    if prev_last is not None:
        assert np.array_equal(ser[0], prev_last + 1), f"store {k}: gap across the boundary"
    # the result of step t is the one that produced observation t + 1
    assert np.array_equal(roll["dmg"], ser + 1), f"store {k}: step results misaligned"
    assert np.array_equal(roll["done"], (ser + 1) % 23 == 22), f"store {k}: done misaligned"
    return ser[-1]


def _check_forwards(cfg, learner, stores, rolls, batches):
    """Test 3: rerun every recorded GPU batch -- the same envs in the same
    order, padded to the same captured size, under the policy version the
    queue recorded -- on the stored observations and hx-before-forward.
    Same batch size, because the fused bf16 kernels pick their tiling by it
    (a row's result does not depend on the other rows, but does on M)."""
    T, K = rolls[0]["lp"].shape[0], len(stores)
    for k in range(K - 1):
        # the bootstrap: one forward, recorded in both stores
        for key in ("v_atk", "v_def"):
            assert np.array_equal(rolls[k][key][T], rolls[k + 1][key][0]), \
                f"store {k}: bootstrap is not the next store's step 0"
    pol = PPO(cfg).policy               # a scratch network to load each version into
    loaded, worst, n_rows = None, 0.0, 0
    for E, p, Q, v in batches:
        k, t = p // T, p % T
        have = k <= K - 2               # its store, and the next one for the hx chain
        if not have.any():
            continue
        if loaded != v:
            pol.load_state_dict(learner.snap[int(v)])
            pol.refresh_shadow()
            loaded = v
        j0 = int(np.flatnonzero(have)[0])
        rows = [(int(k[i]), int(t[i]), int(E[i])) if have[i] else
                (int(k[j0]), int(t[j0]), int(E[j0])) for i in range(E.size)]
        rows += [rows[j0]] * (Q - E.size)
        obs = Observation(**{kk: torch.from_numpy(np.stack([stores[a][kk][b, c]
                                                            for a, b, c in rows])).cuda()
                             for kk in RolloutStore.KEYS})
        hx = torch.from_numpy(np.stack([stores[a]["hx"][b, c] for a, b, c in rows])).cuda()
        with torch.no_grad():
            acts, lp, _e, va, vd, hx_new, lpa, _ea = pol.get_action_and_value(
                obs, hx=hx, deterministic=True)
        out = {"lp": lp, "lp_a": lpa, "v_atk": va, "v_def": vd}
        out = {kk: x.float().cpu().numpy().reshape(-1) for kk, x in out.items()}
        acts = {kk: x.cpu().numpy().reshape(-1) for kk, x in acts.items()}
        hn = hx_new.float().cpu().numpy()
        for i in np.flatnonzero(have):
            a, b, c = rows[i]
            r = rolls[a]
            assert r["_versions"][b, c] == v, (a, b, c)
            for kk in ACT_KEYS:
                assert acts[kk][i] == r["actions"][kk][b, c], (a, b, c, kk)
            for kk in out:
                d = abs(float(out[kk][i]) - float(r[kk][b, c]))
                worst = max(worst, d)
                assert d < 2e-4, f"store {a} step {b} env {c}: {kk} recorded vs rerun {d:.2e}"
            # the next step's hx-before-forward: this hx_new, or zeros after a done
            nxt = stores[a]["hx"][b + 1, c] if b + 1 < T else stores[a + 1]["hx"][0, c]
            want = 0.0 * hn[i] if r["done"][b, c] else hn[i]
            d = float(np.abs(nxt - want).max())
            worst = max(worst, d)
            assert d < 2e-4, f"store {a} step {b} env {c}: hx chain {d:.2e}"
            n_rows += 1
    return worst, n_rows


def test_queue_on_probe_workers():
    cfg = mk_cfg(n_envs=16, pool_workers=4, total_steps_per_epoch=16 * 16,
                 queue_batch=4, queue_timeout_us=300, queue_stores=3, queue_split=2)
    # us per env step, us between writes, a late vocab string every N steps
    cfg.probe = (300, 100, VOCAB_EVERY)
    N, T, n_stores = cfg.n_envs, train_mod.rollout_length(cfg), 10
    tf32 = torch.backends.cuda.matmul.allow_tf32, torch.backends.cudnn.allow_tf32
    torch.backends.cuda.matmul.allow_tf32 = torch.backends.cudnn.allow_tf32 = False
    env = SimPool(cfg, seed=5, target=probe_worker.probe_worker_main)
    actor = None
    try:
        env.reset()
        torch.manual_seed(0)
        agent = PPO(cfg)
        agent.reset_hidden(N)
        learner = FakeLearner(agent, 1)
        actor = Actor(agent, env, T, n_stores=cfg.queue_stores, deterministic=True)
        rng = np.random.default_rng(2)
        delays = []

        def gpu_delay():                     # queued ahead of the replay
            c = int(rng.integers(0, 1_500_000))
            delays.append(c)
            torch.cuda._sleep(c)
        rq = RolloutQueue(cfg, env, agent, actor, learner, T, pre_replay=gpu_delay)
        batches, run_batch = [], rq._batch

        def rec_batch(E):                    # (envs, their steps, bucket, version)
            batches.append((E.copy(), rq.pos[E].copy(), actor.bucket(E.size),
                            rq.actor_version))
            return run_batch(E)
        rq._batch = rec_batch
        stores, rolls, last = [], [], None
        for k in range(n_stores):
            roll, store = rq.next_store()
            assert roll["_k"] == k and roll["lp"].shape == (T, N)
            s = _stored(store, T)
            last = _check_serials(k, s, roll, last, env.vocab_i2s())
            stores.append(s)
            rolls.append(roll)
            rq.submit({"store": store}, roll)
        rq.drain()
        forwards = int(rq.pos.sum())         # every env is back: pos = its forwards
        waits = rq.snapshot()
        # the statistics saw each forwarded row once, and no padding row
        actor.norms.pull_start()
        torch.cuda.current_stream().synchronize()
        actor.norms.pull_finish()
        # (RunningNormalizer counts start at 1e-4)
        counts = np.array([agent.obs_normalizer.count, agent.combat_normalizer.count,
                           agent.terrain_normalizer.count]) - 1e-4
        assert np.abs(counts - [forwards, 3 * forwards, 2 * forwards]).max() < 1e-6, \
            (counts, forwards)
        # every late string the probes made went through the coordinator once
        n_late = int(sum(((np.arange(1, int(rq.pos[e]) + 1) % VOCAB_EVERY) == 0).sum()
                         for e in range(N)))
        assert env.late_string_count() == n_late > 0, (env.late_string_count(), n_late)
        rq.finish()
        worst, n_rows = _check_forwards(cfg, learner, stores, rolls, batches)
        # each stored step came from exactly one GPU batch row
        assert n_rows == (n_stores - 1) * T * N, (n_rows, (n_stores - 1) * T * N)
        lag = np.concatenate([r["_versions"].ravel() for r in rolls])
    finally:
        torch.backends.cuda.matmul.allow_tf32, torch.backends.cudnn.allow_tf32 = tf32
        if actor is not None:
            actor.close()
        env.close()
    print(f"  probe workers: {n_stores} stores x {T} steps x {N} envs, "
          f"{len(delays)} GPU steps each behind a random delay (<= ~1 ms); every "
          f"stored row consistent, serials consecutive within and across stores, "
          f"results aligned; {n_late} late vocab strings reconciled mid-substep, "
          f"stored ids canonical")
    print(f"  stores: {n_rows} stored steps (each from exactly one GPU batch row) rerun "
          f"batch by batch under the recorded policy version (versions "
          f"{lag.min()}-{lag.max()}): actions exact, lp / values / hx chain within "
          f"{worst:.1e}; bootstrap == next store's step 0; statistics counted "
          f"{forwards} forwards exactly; {waits['env_waits']} env waits at store bounds")


# ==========================================================================
# 4. the real loop
# ==========================================================================
def test_real_loop():
    with tempfile.TemporaryDirectory() as d:
        cfg = mk_cfg(n_envs=16, pool_workers=4, total_steps_per_epoch=16 * 16,
                     total_epochs=8, eval_every_epochs=4, eval_episodes_per_env=1,
                     eval_max_steps=64, save_every_steps=512, keep_last_n=2,
                     queue_batch=4, queue_timeout_us=500, seed=3,
                     save_path=os.path.join(d, "run"))
        logs, handover, init = [], [], {}
        orig_sync = PPO.sync_actor

        class SnapPPO(PPO):
            def __init__(self, c):
                super().__init__(c)
                init.update({k: v.detach().clone() for k, v in self.policy.state_dict().items()})

        def sync_and_check(self):
            ev = orig_sync(self)
            torch.cuda.synchronize()
            handover.append(all(torch.equal(x, y) for x, y in
                                zip(self.actor_policy.parameters(), self.policy.parameters())))
            return ev
        PPO.sync_actor = sync_and_check
        orig_ppo = train_mod.PPO
        train_mod.PPO = SnapPPO
        try:
            train_mod.train(cfg, on_epoch=logs.append)
        finally:
            PPO.sync_actor = orig_sync
            train_mod.PPO = orig_ppo
        assert len(logs) == cfg.total_epochs
        assert all(handover) and len(handover) >= 4, handover
        assert all(np.isfinite(x["loss/surrogate"]) and np.isfinite(x["metrics/ev_atk"])
                   for x in logs)
        assert logs[-1]["metrics/n_updates"] > 0
        assert all(x["queue/batches"] > 0 for x in logs)
        ck = cfg.save_path + "_final.pth"
        cks = [p for p in os.listdir(d) if p.endswith(".pth")]
        assert os.path.exists(ck) and len(cks) >= 2, cks
        b = PPO(cfg)
        b.load_checkpoint(ck)
        moved = max(float((v.float() - init[k].float()).abs().max())
                    for k, v in b.policy.state_dict().items() if v.is_floating_point())
        assert moved > 0, "weights did not move"
        lag = [x["queue/policy_lag_mean"] for x in logs]
        bm = [x["queue/batch_mean"] for x in logs]
    print(f"  real loop (Hornet, 4 workers, eval at epoch 4): "
          f"{len(logs)} stores, {len(handover)} handovers with actor == learner, "
          f"max|dW| {moved:.1e}, checkpoints load; batch mean {np.mean(bm):.1f}, "
          f"policy lag mean {np.mean(lag):.2f}")


if __name__ == "__main__":
    test_substep_matches_lockstep()
    test_queue_on_probe_workers()
    test_real_loop()
    print("ALL TESTS PASSED")
