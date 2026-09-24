"""PPO: running normalizers, decomposed GAE, the clipped surrogate over
truncated-BPTT chunks as one captured CUDA graph per minibatch, the learner
thread, checkpoints.

The advantage is

    adv_t = attack_weight * delta_atk_t / D  +  heal_coef * heal_t  -  delta_def_t

with delta_atk = damage_landed + gamma*V_atk' - V_atk and delta_def =
hits_taken + gamma*V_def' - V_def. Two critics learn stationary returns; D (the
adaptive difficulty, train.py) scales the attack term at advantage time only.
There is no terminal win/loss bonus and no shaping term.
"""
import copy
import os
import queue
import threading
import time
import traceback

import numpy as np
import torch
import torch.nn as nn

from model import ACT_KEYS, Policy
from observation import CB, MirrorStats, Observation, mirror_movement, mirror_observation
from rollout import GRAPH_LOCK

# A minibatch is one flat dict of device tensors, leading axis = BPTT chunk.
OBS_KEYS = ("combat_hb", "combat_mask", "combat_kind_ids", "combat_parent_ids",
            "terrain_hb", "terrain_mask", "global_state")
MB_KEYS = OBS_KEYS + ACT_KEYS + ("hx", "committed", "forced", "old_lp", "old_lp_a", "adv",
                                 "atk_ret", "def_ret", "atk_var", "def_var")
# MirrorStats' offsets, in the order the captured update's static tensor holds them.
MIRROR_OFFSETS = ("gs_vel_x", "cb_rel_x", "cb_vel_x", "tr_mx", "tr_npx",
                  "tr_hdy", "tr_hdx_zero")
# Per-minibatch metrics, accumulated on the device and read once per update.
MB_METRICS = ("surrogate", "value_atk", "value_def", "entropy", "kl", "gru_norm",
              "hent_move", "hent_dir", "hent_act", "hent_jump")
# The update metrics before the first update has landed.
NO_UPDATE = {**{k: 0.0 for k in MB_METRICS + ("ev_atk", "ev_def", "atk_return_var",
                                              "def_return_var")},
             "n_updates": 0, "adv_std_by_boss": {}}


class RunningNormalizer:
    """Welford online normalizer for observation vectors."""

    def __init__(self, shape, clip=5.0):
        self.mean = np.zeros(shape, dtype=np.float64)
        self.var = np.ones(shape, dtype=np.float64)
        self.count = 1e-4
        self.clip = clip

    def update(self, batch):
        # The batch moments in float64, like the running ones, so the device
        # statistics (rollout.PrepCuda) agree with these to ~1e-15.
        batch = np.asarray(batch, dtype=np.float64)
        bm, bv, bc = batch.mean(axis=0), batch.var(axis=0), batch.shape[0]
        delta = bm - self.mean
        total = self.count + bc
        self.mean = self.mean + delta * bc / total
        self.var = ((self.var * self.count + bv * bc
                     + delta ** 2 * self.count * bc / total) / total)
        self.count = total

    def normalize(self, x):
        return np.clip((x - self.mean.astype(np.float32))
                       / np.sqrt(self.var.astype(np.float32) + 1e-8),
                       -self.clip, self.clip).astype(np.float32)

    def state_dict(self):
        return {"mean": self.mean, "var": self.var, "count": self.count}

    def load_state_dict(self, s):
        self.mean, self.var, self.count = s["mean"], s["var"], s["count"]


class PPO:
    def __init__(self, config):
        if not torch.cuda.is_available():
            raise RuntimeError("the trainer needs CUDA")
        self.config = config
        self.device = torch.device("cuda")
        self.policy = Policy(config).to(self.device)
        # The copy the actor runs, refreshed by sync_actor() while the learner
        # is idle, so the actor never reads a half-stepped network.
        self.actor_policy = copy.deepcopy(self.policy)
        self.actor_policy.requires_grad_(False)
        # deepcopy leaves the GRU weights outside one contiguous block, which
        # makes cuDNN repack them on every call.
        self.actor_policy.gru.flatten_parameters()
        self.hx = None   # (N, gru_dim) numpy: the GRU state between rollouts / in eval

        # Only the continuous columns are z-scored: binary flags pass raw so
        # a sparse flag is not amplified by a tiny variance; hp gets log1p.
        self.obs_normalizer = RunningNormalizer(
            config.global_state_dim - config.n_binary_flags)
        self.combat_normalizer = RunningNormalizer(config.combat_normalized_dims)
        self.terrain_normalizer = RunningNormalizer(config.terrain_normalized_dims)

        # Fused (one kernel per step) and capturable (step counters on the
        # GPU): what lets the whole training step be a CUDA graph.
        self.optimizer = torch.optim.Adam(self.policy.parameters(), lr=config.lr,
                                          fused=True, capturable=True)
        # Captured updates, one per (combat rows, terrain rows, chunks) bucket.
        self._update_graphs = {}
        # Set by AsyncLearner: sync after every minibatch graph, so the learner
        # never has more than one minibatch queued ahead of an actor forward.
        self.shallow_queue = False

    # ---------------------------------------------- host inference (evals)
    def _prepare(self, obs: Observation, update=False):
        """PrepCuda's normalization on the host: z-score the leading
        continuous columns (flags pass raw), log1p the combat hp columns,
        re-zero padded rows. update=True folds the batch into the statistics
        first (tests use it as the reference)."""
        n = self.config.global_state_dim - self.config.n_binary_flags
        gs = np.asarray(obs.global_state, np.float32)
        if update:
            self.obs_normalizer.update(gs.reshape(-1, gs.shape[-1])[:, :n])
        gs_out = gs.copy()
        gs_out[..., :n] = self.obs_normalizer.normalize(gs[..., :n])

        def hitboxes(hb, mask, normalizer):
            k = normalizer.mean.shape[0]
            if update:
                real = hb.reshape(-1, hb.shape[-1])[mask.reshape(-1) > 0, :k]
                if len(real):
                    normalizer.update(real)
            out = np.array(hb, dtype=np.float32, copy=True)
            out[..., :k] = normalizer.normalize(hb[..., :k])
            return out * mask[..., None]

        chb = hitboxes(obs.combat_hb, obs.combat_mask, self.combat_normalizer)
        hp = chb[..., CB.HP_RAW:CB.HP_MAX_RAW + 1]
        np.maximum(hp, 0, out=hp)
        np.log1p(hp, out=hp)
        thb = hitboxes(obs.terrain_hb, obs.terrain_mask, self.terrain_normalizer)
        return gs_out, chb, thb

    def reset_hidden(self, n_envs):
        self.hx = np.zeros((n_envs, self.config.gru_dim), dtype=np.float32)

    def reset_hidden_for(self, mask):
        """Zero the GRU state where `mask` is True (a new episode)."""
        self.hx[np.asarray(mask, dtype=bool)] = 0.0

    def act_greedy(self, obs: Observation, env_slice=None):
        """Argmax actions with frozen normalizers: the sim greedy eval and the
        in-run game eval. `env_slice` picks the rows of self.hx this batch
        belongs to. Returns {head: (B,) int array}."""
        return self.act(obs, env_slice, deterministic=True)

    @torch.no_grad()
    def act(self, obs: Observation, env_slice=None, deterministic=True):
        """act_greedy, or with deterministic=False actions sampled from the
        policy (frozen normalizers either way)."""
        sl = slice(None) if env_slice is None else env_slice
        gs, chb, thb = self._prepare(obs)
        t = lambda a, dt: torch.from_numpy(np.ascontiguousarray(a)).to(self.device, dt)  # noqa: E731
        o = Observation(combat_hb=t(chb, torch.float32),
                        combat_mask=t(obs.combat_mask, torch.float32),
                        combat_kind_ids=t(obs.combat_kind_ids, torch.int64),
                        combat_parent_ids=t(obs.combat_parent_ids, torch.int64),
                        terrain_hb=t(thb, torch.float32),
                        terrain_mask=t(obs.terrain_mask, torch.float32),
                        global_state=t(gs, torch.float32))
        acts, *_, hx_new, _, _ = self.actor_policy.get_action_and_value(
            o, hx=t(self.hx[sl], torch.float32), deterministic=deterministic)
        self.hx[sl] = hx_new.cpu().numpy()
        return {k: v.cpu().numpy() for k, v in acts.items()}

    def sync_actor(self):
        """Copy the learner's weights into the actor's copy, on the current
        (actor) stream. Only while the learner is idle."""
        with torch.no_grad():
            torch._foreach_copy_(list(self.actor_policy.parameters()),
                                 list(self.policy.parameters()))
        self.actor_policy.refresh_shadow()

    # ---------------------------------------------------------------- GAE
    def _gae_all(self, damage_landed, hits_taken, hp_healed, values_atk,
                 values_def, D_per_env, dones):
        """Decomposed GAE for every env at once: (T, N) in (values (T+1, N)),
        (adv, adv_atk, adv_def, atk_ret, def_ret) out, all float32. Values
        learn stationary returns (no D, no heal); dones[t] bootstraps to 0."""
        cfg = self.config
        T, N = damage_landed.shape
        gamma, gl = np.float32(cfg.gamma), np.float32(cfg.gamma * cfg.gae_lambda)
        aw, hc = np.float32(cfg.attack_weight), np.float32(cfg.heal_coef)
        D = np.asarray(D_per_env, np.float32)
        z = np.float32(0.0)
        adv, adv_atk, adv_def, atk_ret, def_ret = (
            np.empty((T, N), np.float32) for _ in range(5))
        g_atk = g_def = g_atk_pol = g_def_pol = np.zeros(N, np.float32)
        for t in reversed(range(T)):
            done = np.asarray(dones[t], bool)
            next_vatk = np.where(done, z, values_atk[t + 1])
            next_vdef = np.where(done, z, values_def[t + 1])
            g_atk, g_def = np.where(done, z, g_atk), np.where(done, z, g_def)
            g_atk_pol = np.where(done, z, g_atk_pol)
            g_def_pol = np.where(done, z, g_def_pol)

            delta_atk = damage_landed[t] + gamma * next_vatk - values_atk[t]
            delta_def = hits_taken[t] + gamma * next_vdef - values_def[t]
            g_atk_pol = aw * delta_atk / D + hc * hp_healed[t] + gl * g_atk_pol
            g_def_pol = -delta_def + gl * g_def_pol
            adv_atk[t], adv_def[t] = g_atk_pol, g_def_pol
            adv[t] = g_atk_pol + g_def_pol

            g_atk = delta_atk + gl * g_atk
            atk_ret[t] = g_atk + values_atk[t]
            g_def = delta_def + gl * g_def
            def_ret[t] = g_def + values_def[t]
        return adv, adv_atk, adv_def, atk_ret, def_ret

    # ------------------------------------------------------- training step
    @staticmethod
    def _mb_obs(mb):
        return Observation(**{k: mb[k] for k in OBS_KEYS})

    def _minibatch_loss(self, mb):
        """PPO loss for one minibatch, plus its metrics as one device tensor
        (no host sync)."""
        cfg = self.config
        (new_lp, entropy, v_atk, v_def, info, new_lp_a, ent_a) = \
            self.policy.forward_sequence(self._mb_obs(mb), mb["hx"],
                                         {k: mb[k] for k in ACT_KEYS})
        flat = lambda x: x.reshape(-1)  # noqa: E731
        committed_f = flat(mb["committed"])
        # Hard-commit masking: on committed steps the agent did not choose
        # action[2], so the action head's log-prob and entropy are taken out
        # of the ratio and the bonus. Movement / direction / jump stay free.
        new_lp_eff = flat(new_lp) - committed_f * flat(new_lp_a)
        old_lp_eff = flat(mb["old_lp"]) - committed_f * flat(mb["old_lp_a"])
        entropy_eff = flat(entropy) - committed_f * flat(ent_a)

        # A forced step (a search demo the worker replayed, sim_worker.HardStarts)
        # was not sampled from the old policy: it trains at ratio 1, unclipped,
        # a log-likelihood push toward the demo's action weighted by its
        # advantage, and only a positive one. Pushing a log-prob up saturates as
        # the action becomes likely; pushing one down never does (its logit's
        # gradient stays ~1 as the probability goes to 0), and unclipped that
        # runs the logits off.
        # A demo action the policy's validity gate forbids at that step (the
        # search does not know the gates; the sim just ignores it) has log-prob
        # ~-1e4 and a gradient that never saturates, so it trains nothing.
        forced = flat(mb["forced"]) > 0.5
        gated = new_lp_eff.detach() < -50.0
        old_lp_eff = torch.where(forced, new_lp_eff.detach(), old_lp_eff)
        adv_f = flat(mb["adv"])
        adv_f = torch.where(forced, torch.where(gated, torch.zeros_like(adv_f),
                                                adv_f.clamp(min=0.0)), adv_f)
        log_ratio = new_lp_eff - old_lp_eff
        ratio = torch.exp(log_ratio)
        clipped = torch.clamp(ratio, 1 - cfg.clip_eps, 1 + cfg.clip_eps)
        surrogate = -torch.min(ratio * adv_f, clipped * adv_f).mean()

        atk_vloss = (flat(v_atk) - flat(mb["atk_ret"])).pow(2)
        def_vloss = (flat(v_def) - flat(mb["def_ret"])).pow(2)
        # Each channel's squared error over that channel's (per-boss, EMA)
        # return variance, so atk and def pull with comparable force.
        avf = mb["atk_var"].unsqueeze(-1).expand_as(v_atk).reshape(-1)
        dvf = mb["def_var"].unsqueeze(-1).expand_as(v_def).reshape(-1)
        value_loss = ((atk_vloss / avf).mean()
                      + cfg.def_value_coeff * (def_vloss / dvf).mean())
        entropy_loss = -entropy_eff.mean()
        loss = (surrogate + cfg.value_coeff * value_loss
                + cfg.entropy_coeff * entropy_loss)

        with torch.no_grad():
            kl = ((ratio - 1) - log_ratio).mean()
            he = info["head_ent"]
            mvec = torch.stack([t.detach().float() for t in (
                surrogate, atk_vloss.mean(), def_vloss.mean(), entropy_loss, kl,
                info["gru_norm"], he["move"], he["dir"], he["act"], he["jump"])])
        return loss, mvec

    def _step_eager(self, mb):
        """One training step outside a graph: the capture's warm-up (and the
        tests' reference)."""
        loss, mvec = self._minibatch_loss(mb)
        # NOT set_to_none: every captured update has the .grad addresses baked
        # in, and freeing them here would leave the older buckets' graphs
        # writing into memory the allocator has handed to something else.
        self.optimizer.zero_grad(set_to_none=False)
        loss.backward()
        nn.utils.clip_grad_norm_(self.policy.parameters(), self.config.max_grad_norm)
        self.optimizer.step()
        self.policy.refresh_shadow()
        return mvec

    @staticmethod
    def _gather_mb(src, idx):
        """Chunks `idx` of every rollout tensor."""
        return {k: torch.index_select(v, 0, idx) for k, v in src.items()}

    def _mirror_mb(self, mb, mstats, flag):
        """The horizontal mirror of a minibatch where the 0-dim device tensor
        `flag` > 0.5 (a select, so the graph decides per minibatch)."""
        o = mirror_observation(self._mb_obs(mb), mstats)
        m = {"global_state": o.global_state, "combat_hb": o.combat_hb,
             "terrain_hb": o.terrain_hb, "movement": mirror_movement(mb["movement"])}
        on = flag > 0.5
        return {**mb, **{k: torch.where(on, v, mb[k]) for k, v in m.items()}}

    @staticmethod
    def _row_bucket(n, cap):
        """Smallest of 16, 32, 64, ... (clipped to cap) holding n rows."""
        assert n <= cap, f"{n} rows over the cap {cap}"
        b = 16
        while b < n and b < cap:
            b *= 2
        return min(b, cap)

    def _update_graph_for(self, src):
        """The captured update for this rollout. Rows are padded (in place,
        in `src`) to a bucket of 16/32/64/... up to the caps, captured lazily
        once per bucket; padded rows are masked, so the loss is the one at
        the rollout's own width."""
        cfg = self.config
        C, K = src["combat_hb"].shape[2], src["terrain_hb"].shape[2]
        cC = self._row_bucket(C, int(cfg.cap_combat))
        cK = self._row_bucket(K, int(cfg.cap_terrain_view))
        pad = torch.nn.functional.pad
        for k, n, cap in (("combat_hb", C, cC), ("terrain_hb", K, cK)):
            src[k] = pad(src[k], (0, 0, 0, cap - n))
        for k, n, cap in (("combat_mask", C, cC), ("combat_kind_ids", C, cC),
                          ("combat_parent_ids", C, cC), ("terrain_mask", K, cK)):
            src[k] = pad(src[k], (0, cap - n))
        key = (cC, cK, src["adv"].shape[0])
        entry = self._update_graphs.get(key)
        if entry is None:
            t = time.perf_counter()
            entry = self._capture_update_graph(src)
            print(f"  [cuda_graphs] captured the update: {entry['n_steps']} steps of "
                  f"{cfg.chunks_per_batch}x{cfg.seq_len} chunks, combat<={cC} "
                  f"terrain<={cK} ({time.perf_counter() - t:.1f}s)", flush=True)
            self._update_graphs[key] = entry
        return entry

    def _capture_update_graph(self, src):
        """Warm up, then capture a whole update -- train_iters passes over the
        rollout, every minibatch gathered, mirrored, trained and stepped -- as
        one graph per minibatch over static buffers: the rollout, the
        permutations, the mirror coins, the mirror offsets and a metric
        accumulator. A ragged last minibatch is simply a smaller step.

        The warm-up steps are real Adam steps (they also create Adam's state,
        which must exist before capture), so the weights and optimizer state
        are snapshotted first and put back afterwards: capturing leaves no
        trace in training."""
        cfg = self.config
        dev = self.device
        CPB, iters = cfg.chunks_per_batch, cfg.train_iters
        n = src["adv"].shape[0]
        n_mb = -(-n // CPB)
        st = {k: v.clone() for k, v in src.items()}
        perm = torch.arange(n, device=dev).repeat(iters, 1)
        flags = torch.zeros((iters, n_mb), device=dev)
        moff = torch.zeros(len(MIRROR_OFFSETS), device=dev)
        acc = torch.zeros(len(MB_METRICS), device=dev)
        mstats = MirrorStats(**{f: moff[i] for i, f in enumerate(MIRROR_OFFSETS)},
                             clip=float(self.terrain_normalizer.clip))

        params = list(self.policy.parameters())
        snap_p = [p.detach().clone() for p in params]
        snap_s = {id(p): {k: v.clone() for k, v in self.optimizer.state[p].items()
                          if torch.is_tensor(v)}
                  for p in params if p in self.optimizer.state}
        try:
            s = torch.cuda.Stream()
            s.wait_stream(torch.cuda.current_stream())
            with torch.cuda.stream(s):
                mb0 = self._gather_mb(st, perm[0, :CPB])
                for _ in range(2):
                    self._step_eager(self._mirror_mb(mb0, mstats, flags[0, 0]))
            torch.cuda.current_stream().wait_stream(s)
        finally:
            torch.cuda.synchronize()
            with torch.no_grad():
                for p, v in zip(params, snap_p):
                    p.copy_(v)
                for p in params:
                    old = snap_s.get(id(p), {})
                    for k, v in (self.optimizer.state.get(p) or {}).items():
                        if torch.is_tensor(v):
                            if k in old:
                                v.copy_(old[k])
                            else:
                                v.zero_()      # a fresh Adam's state
                self.policy.refresh_shadow()
            torch.cuda.synchronize()
        del snap_p, snap_s

        # The captured steps accumulate into .grad, so it must exist at a
        # fixed address and be zeroed inside every step.
        self.optimizer.zero_grad(set_to_none=False)
        # One graph per minibatch, sharing one memory pool (safe: they always
        # replay in capture order). One graph for the whole update would queue
        # ~35k kernels at once, and an actor forward issued behind that
        # backlog waits for all of it. thread_local: this runs on the learner
        # thread while the actor launches its own work.
        pool = torch.cuda.graph_pool_handle()
        graphs = []
        for it in range(iters):
            for j in range(n_mb):
                g = torch.cuda.CUDAGraph()
                with GRAPH_LOCK.capture(), torch.cuda.graph(
                        g, pool=pool, capture_error_mode="thread_local"):
                    if not graphs:
                        acc.zero_()
                    mb = self._gather_mb(st, perm[it, j * CPB:(j + 1) * CPB])
                    mb = self._mirror_mb(mb, mstats, flags[it, j])
                    loss, mvec = self._minibatch_loss(mb)
                    self.optimizer.zero_grad(set_to_none=False)
                    loss.backward()
                    nn.utils.clip_grad_norm_(self.policy.parameters(), cfg.max_grad_norm)
                    self.optimizer.step()
                    self.policy.refresh_shadow()
                    acc += mvec
                graphs.append(g)
                del mb, loss, mvec
        return {"graphs": graphs, "src": st, "perm": perm, "flags": flags,
                "moff": moff, "acc": acc, "n_steps": iters * n_mb}

    # ------------------------------------------------------------- update
    def rollout_src(self, roll, store, D_per_env, boss_per_env, value_var_state):
        """Everything a rollout's update trains on, on the device: one row per
        BPTT chunk (chunk index = block * N + env) under MB_KEYS. `roll` holds
        (T, N) actions / lp / lp_a / dmg / hit / heal / done / committed and
        (T + 1, N) v_atk / v_def (the extra row is the bootstrap); the
        observations and chunk-start hx come from `store`. Advantages are
        normalized per boss, and each boss's return variance is tracked as an
        EMA in value_var_state (PopArt-lite). Returns (src, stats)."""
        cfg = self.config
        T, N = roll["dmg"].shape
        L = cfg.seq_len
        n_blocks = T // L
        T_used = n_blocks * L
        n_chunks = n_blocks * N
        assert n_chunks > 0, f"rollout T={T} shorter than seq_len={L}"

        adv_a, _, _, atk_ret_a, def_ret_a = self._gae_all(
            roll["dmg"], roll["hit"], roll["heal"], roll["v_atk"], roll["v_def"],
            D_per_env, roll["done"])

        def _ev(returns, values):
            var = returns.var()
            return float(1.0 - (returns - values).var() / var) if var > 1e-8 else 0.0

        def chunk_tn(a):
            return a[:T_used].reshape(n_blocks, L, N).transpose(0, 2, 1).reshape(-1, L)

        adv_c = chunk_tn(adv_a)

        # ---- per-boss advantage normalization. The pooled std floors each
        # boss's: a boss whose rollout carried no reward events has an adv std
        # made of critic error only, and dividing by it would scale that noise
        # up to full-size policy updates.
        env_per_chunk = np.arange(n_chunks) % N
        boss_arr = np.array(boss_per_env)
        boss_per_chunk = boss_arr[env_per_chunk]
        bosses = list(dict.fromkeys(boss_per_env))
        adv_mean = np.zeros(n_chunks, np.float32)
        adv_std = np.ones(n_chunks, np.float32)
        pooled_std = float(adv_c.std()) if adv_c.size > 1 else 0.0
        adv_std_by_boss = {}
        for b in bosses:
            m = boss_per_chunk == b
            s = max(float(adv_c[m].std()), 0.25 * pooled_std)
            adv_mean[m] = float(adv_c[m].mean())
            adv_std[m] = s
            adv_std_by_boss[b] = s
        adv_c = (adv_c - adv_mean[:, None]) / (adv_std[:, None] + 1e-8)

        # ---- per-boss return-variance EMA, weighted by the boss's share of
        # the rollout
        beta = float(cfg.value_var_ema)
        atk_var_env = np.zeros(N, np.float32)
        def_var_env = np.zeros(N, np.float32)
        fair_share = max(1.0, (T * N) / max(1, len(bosses)))
        for b in bosses:
            m = boss_arr == b
            av, dv = float(atk_ret_a[:, m].var()), float(def_ret_a[:, m].var())
            slot = value_var_state[b]
            if slot["atk_var_ema"] is None:
                slot["atk_var_ema"], slot["def_var_ema"] = av, dv
            else:
                be = beta ** (float(m.sum() * T) / fair_share)
                slot["atk_var_ema"] = be * slot["atk_var_ema"] + (1 - be) * av
                slot["def_var_ema"] = be * slot["def_var_ema"] + (1 - be) * dv
            atk_var_env[m], def_var_env[m] = slot["atk_var_ema"], slot["def_var_ema"]

        dev = self.device
        f = lambda a: torch.from_numpy(np.ascontiguousarray(a)).float().to(dev)  # noqa: E731
        obs_t = store.chunked(n_blocks, L)
        src = {k: getattr(obs_t, k) for k in OBS_KEYS}
        src.update({k: torch.from_numpy(chunk_tn(roll["actions"][k])).long().to(dev)
                    for k in ACT_KEYS})
        src.update(hx=store.chunk_hx(n_blocks, L),
                   committed=f(chunk_tn(roll["committed"].astype(np.float32))),
                   forced=f(chunk_tn(roll.get("forced", np.zeros_like(roll["committed"]))
                                     .astype(np.float32))),
                   old_lp=f(chunk_tn(roll["lp"])), old_lp_a=f(chunk_tn(roll["lp_a"])),
                   adv=f(adv_c), atk_ret=f(chunk_tn(atk_ret_a)),
                   def_ret=f(chunk_tn(def_ret_a)),
                   atk_var=f(np.maximum(atk_var_env[env_per_chunk], 0.0) + 1e-3),
                   def_var=f(np.maximum(def_var_env[env_per_chunk], 0.0) + 1e-3))
        stats = {"ev_atk": _ev(atk_ret_a, roll["v_atk"][:T]),
                 "ev_def": _ev(def_ret_a, roll["v_def"][:T]),
                 "adv_std_by_boss": adv_std_by_boss,
                 "atk_return_var": float(atk_ret_a.var()),
                 "def_return_var": float(def_ret_a.var())}
        return src, stats

    def draw_update(self, n_chunks):
        """The update's randomness, drawn on the host: a chunk permutation per
        pass, then a mirror coin per minibatch."""
        n_mb = -(-n_chunks // self.config.chunks_per_batch)
        perms, coins = [], []
        for _ in range(self.config.train_iters):
            perms.append(np.random.permutation(n_chunks))
            coins.append([np.random.rand() < 0.5 for _ in range(n_mb)])
        return np.stack(perms), np.array(coins, np.float32)

    def train_on_rollout(self, roll, store, mstats, D_per_env, boss_per_env,
                         value_var_state):
        """One PPO update on a completed store. `mstats` is the MirrorStats of
        the statistics the rollout ended on (the mirror runs on z-scores, so
        it has to undo the mean). One graph replay per minibatch."""
        src, stats = self.rollout_src(roll, store, D_per_env, boss_per_env,
                                      value_var_state)
        entry = self._update_graph_for(src)
        perms, coins = self.draw_update(src["adv"].shape[0])
        for k, v in src.items():
            entry["src"][k].copy_(v)
        entry["perm"].copy_(torch.from_numpy(perms))
        entry["flags"].copy_(torch.from_numpy(coins))
        entry["moff"].copy_(torch.tensor([getattr(mstats, f) for f in MIRROR_OFFSETS],
                                         dtype=torch.float32))
        stream = torch.cuda.current_stream()
        for g in entry["graphs"]:
            with GRAPH_LOCK:
                g.replay()
            if self.shallow_queue:
                stream.synchronize()
        # One device->host read for the whole update.
        out = dict(zip(MB_METRICS, (entry["acc"] / entry["n_steps"]).tolist()))
        out["n_updates"] = entry["n_steps"]
        out.update(stats)
        return out

    # -------------------------------------------------------- checkpoints
    # A checkpoint is a policy to evaluate, never a run to resume: the
    # weights, the normalizers and the vocab id space they were trained with.
    def save_checkpoint(self, path, vocab_i2s, boss_state, env_steps):
        """Atomic (a sibling .tmp, then a rename). A failed save is reported,
        never raised: it must not take down a healthy run."""
        tmp = path + ".tmp"
        try:
            torch.save({"model": self.policy.state_dict(),
                        "obs_normalizer": self.obs_normalizer.state_dict(),
                        "combat_normalizer": self.combat_normalizer.state_dict(),
                        "terrain_normalizer": self.terrain_normalizer.state_dict(),
                        "kind_vocab_i2s": list(vocab_i2s),
                        "D": {b: float(s["D"]) for b, s in boss_state.items()},
                        "env_steps": int(env_steps)}, tmp)
            os.replace(tmp, path)
            return True
        except Exception as exc:                        # noqa: BLE001
            if os.path.exists(tmp):
                try:
                    os.remove(tmp)
                except OSError:
                    pass
            print(f"  [checkpoint] save FAILED ({exc!r}) - training continues")
            return False

    def load_checkpoint(self, path):
        """Weights and normalizers, for evaluation. Returns the checkpoint dict
        (its kind_vocab_i2s is the id space the weights expect)."""
        ck = torch.load(path, map_location=self.device, weights_only=False)
        self.policy.load_state_dict(ck["model"])
        self.policy.refresh_shadow()
        self.obs_normalizer.load_state_dict(ck["obs_normalizer"])
        self.combat_normalizer.load_state_dict(ck["combat_normalizer"])
        self.terrain_normalizer.load_state_dict(ck["terrain_normalizer"])
        self.sync_actor()
        return ck


class AsyncLearner:
    """PPO updates on a background thread and its own CUDA stream, one store
    behind the actor. A thread, not a process: actor and learner share the GPU
    and the weights, both are GPU-bound or waiting (every wait releases the
    GIL), and the actor runs its own copy of the weights, refreshed only
    while this thread is idle. PPO's ratio is taken against the log-probs
    recorded at collection, so the lag is accounted for."""

    def __init__(self, agent):
        self.agent = agent
        agent.shallow_queue = True
        self.stream = torch.cuda.Stream()
        self._jobs = queue.Queue()
        self._done = threading.Event()
        self._out = None
        self.busy = False
        # Called on this thread when an update finishes: the rollout queue
        # sets it to wake its server.
        self.on_done = None
        self._thread = threading.Thread(target=self._run, name="learner", daemon=True)
        self._thread.start()

    def _run(self):
        torch.cuda.set_stream(self.stream)
        while True:
            item = self._jobs.get()
            if item is None:
                return
            job, ready = item
            t = time.perf_counter()
            try:
                # everything the actor did before handing over is ordered first
                self.stream.wait_event(ready)
                self._out = ("ok", self.agent.train_on_rollout(**job),
                             time.perf_counter() - t)
            except BaseException:                           # noqa: BLE001
                self._out = ("err", traceback.format_exc(), 0.0)
            self._done.set()
            cb = self.on_done
            if cb is not None:
                cb()

    def finished(self):
        """True when the update in flight is done (wait() will not block)."""
        return self._done.is_set()

    def submit(self, job, ready):
        assert not self.busy, "one update in flight at a time"
        self.busy = True
        self._done.clear()
        self._jobs.put((job, ready))

    def wait(self):
        """(metrics, seconds) of the update in flight, once it has finished."""
        self._done.wait()
        self.busy = False
        kind, m, dt = self._out
        if kind == "err":
            raise RuntimeError("learner thread failed:\n" + m)
        return m, dt

    def close(self):
        self._jobs.put(None)
        self._thread.join(timeout=30.0)
