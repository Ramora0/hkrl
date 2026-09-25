"""Discovery on one fight: find a hitless line through a boss whose RNG is
fixed, with a sampler fit by flow balance alone (after onezero, C:/Users/Lee/
coding/python/AI/onezero, and sub-trajectory balance, Madan et al. ICML 2023).
No restarts, no stored-line imitation, no policy-gradient loss.

    python train/train_discover.py --boss_levels GG_Grimm_Nightmare --save_path runs/x

The fight. Every reset uses fixed_seed, so the boss's draws depend only on
what the knight did and a line (the knight's actions from the reset) replays
exactly. A line ends at the first step that costs health (end_on_hit); its
return R is the % of boss HP landed before it (damage landed on the step that
ends it is a trade and does not count). A line the boss's death ends is a
hitless kill.

The target: P(line) proportional to exp(beta * R). beta is set by the run as
onezero sets it: flow_scale * (entropy of uniform play over a walk) / (span
of returns seen), uniform play over a step being ln(3*3*8*2), the four heads'
joint (gates not counted).

The loss: sub-trajectory balance. R is a sum of per-step damage d_k, so the
flow through a state factors as exp(beta * damage so far) * F(s), F(s) being
the flow of what can still happen from s -- a function of the state alone
(forward-looking flows, Pan et al. ICML 2023). The network's value head
gives log F(s) = beta * u(s); a line's end has log F = 0. Every piece i -> j
of a walk must balance,

    delta(i, j) = log F(s_i) + sum_{k=i}^{j-1} (log pi(a_k|s_k) - beta d_k) - log F(s_j),

which for i = 0, j = end is trajectory balance of the whole line (log F(s_0)
is log Z). The loss is the lambda^(j-i)-weighted mean of delta^2 over the
pieces of every drawn walk (all of them, in one pass: subtb), so each step
gets credit from pieces around it rather than one gap per line.

The fit. An iteration is flow_iter_stores stores of walks: every walk the
sampler finished in them. flow_steps steps of flow_batch walks drawn
uniformly from the iteration run spread over the next iteration's stores,
beside the rollouts. Each step gathers the drawn walks' steps from a ring of
stored observations into one sequence per walk (the GRU from zero at the
walk's first step, as the actor played it) and recomputes log pi and u under
the network being trained.
"""
import math
import os
import signal
import sys
import time
from dataclasses import dataclass, fields

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # hkpy

import numpy as np                                          # noqa: E402
import torch                                                # noqa: E402

import flow                                                 # noqa: E402
from config import Config                                   # noqa: E402
from model import ACT_KEYS                                  # noqa: E402
from observation import Observation                         # noqa: E402
from ppo import PPO, AsyncLearner                           # noqa: E402
from rollout import Actor, RolloutQueue                     # noqa: E402
from sim_env import SimPool, close_all_pools                # noqa: E402
from train import WandB, make_boss_state, rollout_length, rotate_checkpoints  # noqa: E402

LOG_UNIFORM_STEP = math.log(3 * 3 * 8 * 2)


@dataclass
class DiscoverConfig(Config):
    fixed_seed: int = 1
    end_on_hit: bool = True
    # One knight: health plays no part before the first hit.
    train_max_health: str = "9,9"
    start_health_low_p: float = 0.0
    # The evals play fresh seeds (and the game its own RNG); the record is the run's.
    eval_every_epochs: int = 0
    game_eval: bool = False
    record_keep: int = 16             # best lines saved
    flow_scale: float = 2.0           # beta's greed dial (onezero's default)
    flow_lambda: float = 0.99         # sub-trajectory weight per step of a piece
    flow_steps: int = 12              # optimizer steps per iteration: the fit keeps pace with the rollouts
    flow_batch: int = 256             # walks per step
    flow_micro: int = 8192            # steps on the device at once
    flow_iter_stores: int = 32        # stores of walks per iteration
    flow_ring_stores: int = 96        # stores of observations kept for the fit


def base_config(cfg):
    """The plain Config the sim workers get (spawned processes unpickle it by its module)."""
    return Config(**{f.name: getattr(cfg, f.name) for f in fields(Config)})


# ==========================================================================
# lines and walks
# ==========================================================================
class Line:
    __slots__ = ("actions", "R", "kind")

    def __init__(self, actions, R, kind):
        self.actions = actions            # (L, 4) int8
        self.R = R
        self.kind = kind                  # "hit" or "kill"


class Record:
    """The best lines by R, kept to be saved and replayed."""

    def __init__(self, keep):
        self.keep = keep
        self.lines = []

    def add(self, line):
        if len(self.lines) < self.keep or line.R > self.lines[-1].R:
            self.lines.append(line)
            self.lines.sort(key=lambda x: -x.R)
            del self.lines[self.keep:]

    def best(self):
        return self.lines[0] if self.lines else None

    def save(self, path):
        acts = (np.concatenate([x.actions for x in self.lines]) if self.lines
                else np.zeros((0, 4), np.int8))
        np.savez_compressed(path, ids=np.arange(len(self.lines)),
                            lens=np.array([len(x.actions) for x in self.lines], np.int64),
                            actions=acts, R=np.array([x.R for x in self.lines], np.float32),
                            kill=np.array([x.kind == "kill" for x in self.lines]))


class Walks:
    """Each env's steps into walks. A finished walk's record for the fit:
    (env, first ply of the env's timeline, length, R, log P as sampled)."""

    def __init__(self, n_envs):
        self.acts = [[] for _ in range(n_envs)]
        self.R = np.zeros(n_envs)
        self.logp = np.zeros(n_envs)
        self.first = np.full(n_envs, -1, np.int64)
        self.kills = []

    def feed(self, roll):
        """One store. -> (lines, records) of the walks it finished."""
        T, N = roll["dmg"].shape
        base = int(roll["_k"]) * T
        act = np.stack([roll["actions"][k] for k in ACT_KEYS], -1).astype(np.int8)    # (T, N, 4)
        lp = roll["lp"] - roll["committed"] * roll["lp_a"]
        dmg = step_damage(roll)
        lines, recs = [], []
        for e in range(N):
            for t in range(T):
                if self.first[e] < 0:
                    self.first[e], self.R[e], self.logp[e] = base + t, 0.0, 0.0
                self.acts[e].append(act[t, e])
                self.logp[e] += float(lp[t, e])
                self.R[e] += float(dmg[t, e])
                if roll["done"][t, e]:
                    kind = "hit" if roll["hit"][t, e] > 0 else "kill"
                    line = Line(np.array(self.acts[e], np.int8), float(self.R[e]), kind)
                    lines.append(line)
                    recs.append((e, int(self.first[e]), len(line.actions), line.R, float(self.logp[e])))
                    self.acts[e], self.first[e] = [], -1
        return lines, recs


def step_damage(roll):
    """(T, N) damage each step lands, 0 on the step that ends a line with a hit."""
    return np.where((roll["hit"] > 0) & roll["done"], 0.0, roll["dmg"]).astype(np.float32)


def subtb(lp, u, d, length, beta, lam):
    """Sub-trajectory balance of whole walks, every piece weighted
    lambda^(j-i). lp, u, d: (B, L) per step (log pi of the action taken, the
    flow head, damage), zero past a walk's length. -> (loss per walk (B,),
    whole-line gap (B,)).

    delta(i, j) = X_i - X_j with X_t = log F(s_t) - sum_{k<t} (lp_k - beta d_k),
    so the weighted sum of delta^2 over all pairs i < j <= n is
        sum_j [ Q_j + K_j X_j^2 - 2 X_j C_j ],
    Q_j = sum_{i<j} lam^(j-i) X_i^2, C_j = sum_{i<j} lam^(j-i) X_i,
    K_j = sum_{i<j} lam^(j-i): one pass of discounted prefix sums. In float64,
    X centred per walk (delta does not see a constant), so the expansion keeps
    its digits."""
    B, L = lp.shape
    dev = lp.device
    f64 = torch.float64
    pos = torch.arange(L + 1, device=dev)
    ln = length.view(B, 1)
    live = (pos[None, :L] < ln).to(f64)
    logF = torch.cat([beta * u.to(f64), u.new_zeros(B, 1, dtype=f64)], 1)
    logF = torch.where(pos[None, :] >= ln, torch.zeros_like(logF), logF)        # log F = 0 at the end
    S = torch.cat([lp.new_zeros(B, 1, dtype=f64),
                   torch.cumsum((lp.to(f64) - beta * d.to(f64)) * live, 1)], 1)
    X = logF - S                                                                 # (B, L + 1)
    on = (pos[None, :] <= ln).to(f64)                                            # t = 0..n
    X = (X - ((X * on).sum(1, keepdim=True) / on.sum(1, keepdim=True)).detach()) * on
    g = lam ** pos.to(f64)                                                       # lam^t
    ginv = 1.0 / g

    def disc(v):
        """sum_{i<j} lam^(j-i) v_i at every j: lam^j * exclusive prefix sum of lam^-i v_i."""
        c = torch.cumsum(v * ginv[None, :], 1)
        return g[None, :] * torch.cat([v.new_zeros(B, 1), c[:, :-1]], 1)

    K = lam * (1.0 - g) / (1.0 - lam)                                            # sum_{i<j} lam^(j-i), i >= 0
    after = ((pos[None, :] >= 1) & (pos[None, :] <= ln)).to(f64)                 # j = 1..n
    num = ((disc(X * X) + K[None, :] * X * X - 2.0 * X * disc(X)) * after).sum(1)
    den = (K[None, :] * after).sum(1)
    whole = logF[:, 0] + S.gather(1, length.view(B, 1)).squeeze(1)
    return (num / den.clamp(min=1e-12)).to(lp.dtype), whole.to(lp.dtype)


# ==========================================================================
# the flow learner
# ==========================================================================
class FlowAgent(PPO):
    """PPO's network, normalizers, actor copy and checkpoints; the training
    step is the flow fit. Runs on the learner thread (AsyncLearner): one call
    per completed store."""

    # ring dtypes: what the policy saw, in half the bytes where it loses nothing the bf16 trunk keeps
    RING = {"combat_hb": torch.float16, "combat_mask": torch.uint8, "combat_kind_ids": torch.int16,
            "combat_parent_ids": torch.int16, "terrain_hb": torch.float16,
            "terrain_mask": torch.uint8, "global_state": torch.float32}

    def __init__(self, config):
        super().__init__(config)
        self.ring = None
        self.T = self.N = None
        self.rng = np.random.default_rng(config.seed or None)
        self.batch = []                   # walk records of the iteration being played
        self.stores_in_iter = 0
        self.fit = None                   # the iteration being fit
        self.seen_lo, self.seen_hi = math.inf, -math.inf
        self.last = {}

    # ---------------------------------------------------------------- ring
    def _ingest(self, k, store, roll):
        T, N = roll["dmg"].shape
        if self.ring is None:
            R = self.config.flow_ring_stores
            self.T, self.N = T, N
            self.ring = {key: torch.zeros((R, T) + tuple(store.buf[key].shape[1:]), dtype=dt,
                                          device=self.device) for key, dt in self.RING.items()}
            self.ring["actions"] = torch.zeros((R, T, N, 4), dtype=torch.int8, device=self.device)
            self.ring["committed"] = torch.zeros((R, T, N), dtype=torch.bool, device=self.device)
            self.ring["dmg"] = torch.zeros((R, T, N), dtype=torch.float32, device=self.device)
        slot = k % self.config.flow_ring_stores
        torch.cuda.current_stream().wait_event(store.ready)
        for key, dt in self.RING.items():
            self.ring[key][slot].copy_(store.buf[key][:T].to(dt))
        act = np.stack([roll["actions"][a] for a in ACT_KEYS], -1).astype(np.int8)
        self.ring["actions"][slot].copy_(torch.from_numpy(act))
        self.ring["committed"][slot].copy_(torch.from_numpy(roll["committed"].astype(bool)))
        self.ring["dmg"][slot].copy_(torch.from_numpy(step_damage(roll)))

    def _walk_terms(self, env, first, length):
        """Per step of each walk under the current network, with autograd:
        (log pi of the action taken, flow head u, damage), (B, L), zero past a
        walk's length; the walk's steps gathered into one sequence, GRU from zero."""
        T, N, R = self.T, self.N, self.config.flow_ring_stores
        B, L = len(env), int(length.max())
        dev = self.device
        steps = torch.arange(L, device=dev)
        ln = torch.as_tensor(length, device=dev)
        p = torch.as_tensor(first, device=dev)[:, None] + torch.minimum(steps[None, :], ln[:, None] - 1)
        k = p // T
        idx = (((k % R) * T + (p - k * T)) * N + torch.as_tensor(env, device=dev)[:, None]).reshape(-1)

        def take(key, want, rows=None):
            src = self.ring[key].reshape((R * T * N,) + self.ring[key].shape[3:])
            if rows is not None:
                src = src[:, :rows]
            out = src.index_select(0, idx).to(want)
            return out.reshape((B, L) + tuple(out.shape[1:]))

        # the rows ever live in these steps (both sets are compacted to the
        # front), to PPO's buckets of 16 / 32 / 64
        cm = take("combat_mask", torch.float32)
        tm = take("terrain_mask", torch.float32)
        wc = PPO._row_bucket(max(1, int(cm.sum(-1).max())), cm.shape[-1])
        wt = PPO._row_bucket(max(1, int(tm.sum(-1).max())), tm.shape[-1])
        obs = Observation(combat_hb=take("combat_hb", torch.float32, wc), combat_mask=cm[..., :wc],
                          combat_kind_ids=take("combat_kind_ids", torch.int64, wc),
                          combat_parent_ids=take("combat_parent_ids", torch.int64, wc),
                          terrain_hb=take("terrain_hb", torch.float32, wt), terrain_mask=tm[..., :wt],
                          global_state=take("global_state", torch.float32))
        acts = take("actions", torch.int64)
        actions = {h: acts[..., i] for i, h in enumerate(ACT_KEYS)}
        hx = torch.zeros((B, self.config.gru_dim), device=dev)
        lp, _ent, u, _vd, _info, lp_a, _ea = self.policy.forward_sequence(obs, hx, actions)
        live = (steps[None, :] < ln[:, None]).float()
        lp = (lp - take("committed", torch.float32) * lp_a) * live
        return lp, u * live, take("dmg", torch.float32) * live, ln

    # ------------------------------------------------------------ the fit
    def _begin_fit(self, k_now):
        cfg = self.config
        if not self.batch:
            return
        env, first, length, value, logp = (np.array(c) for c in zip(*self.batch))
        # every walk has to stay in the ring until the fit of this iteration ends
        keep = (first // self.T) >= k_now + cfg.flow_iter_stores - cfg.flow_ring_stores + 1
        dropped = int((~keep).sum())
        env, first, length, value, logp = (a[keep] for a in (env, first, length, value, logp))
        if env.size == 0:
            return
        self.seen_lo = min(self.seen_lo, float(value.min()))
        self.seen_hi = max(self.seen_hi, float(value.max()))
        beta = flow.inverse_temperature(-length * LOG_UNIFORM_STEP, self.seen_lo, self.seen_hi,
                                        cfg.flow_scale)
        self.fit = dict(env=env, first=first, length=length, logp=logp, value=value, beta=beta,
                        draws=self.rng.integers(env.size, size=cfg.flow_steps * cfg.flow_batch),
                        step=0, walks=int(env.size), dropped=dropped)

    def _fit_step(self):
        cfg, f = self.config, self.fit
        j = f["step"]
        f["step"] += 1
        li = f["draws"][j * cfg.flow_batch:(j + 1) * cfg.flow_batch]
        if li.size == 0:
            return None
        t0 = time.perf_counter()
        order = li[np.argsort(f["length"][li])]
        parts, start = [], 0
        while start < order.size:
            n = 1
            while start + n < order.size and (n + 1) * int(f["length"][order[start + n]]) <= cfg.flow_micro:
                n += 1
            parts.append(order[start:start + n])
            start += n
        self.optimizer.zero_grad(set_to_none=False)
        beta = float(f["beta"])
        losses, wholes, lps = [], [], []
        for q in parts:
            lp, u, d, ln = self._walk_terms(f["env"][q], f["first"][q], f["length"][q])
            loss, whole = subtb(lp, u, d, ln, beta, cfg.flow_lambda)
            (loss.sum() / li.size).backward()
            losses.append(loss.detach())
            wholes.append(whole.detach())
            lps.append(lp.detach().sum(1))
        gn = float(torch.nn.utils.clip_grad_norm_(self.policy.parameters(), cfg.max_grad_norm))
        self.optimizer.step()
        self.policy.refresh_shadow()
        drawn = np.concatenate(parts)
        lp_now = torch.cat(lps).double().cpu().numpy()
        return {"subtb": float(torch.cat(losses).mean()),
                "whole_gap": float(torch.cat(wholes).abs().mean()),
                "logp_err": float(np.mean(np.abs(lp_now - f["logp"][drawn]) / f["length"][drawn])),
                "grad_norm": gn,
                "fit_sps": float(f["length"][drawn].sum()) / max(time.perf_counter() - t0, 1e-9)}

    # ------------------------------------------------ the learner's entry
    def train_on_rollout(self, roll, store, walks):
        cfg = self.config
        k = int(roll["_k"])
        self._ingest(k, store, roll)
        self.batch.extend(walks)
        self.stores_in_iter += 1
        if self.stores_in_iter >= cfg.flow_iter_stores:
            self._begin_fit(k)
            self.batch, self.stores_in_iter = [], 0
        out = []
        if self.fit is not None:
            for _ in range(-(-cfg.flow_steps // cfg.flow_iter_stores)):
                if self.fit["step"] >= cfg.flow_steps:
                    break
                r = self._fit_step()
                if r is not None:
                    out.append(r)
        if out:
            self.last = {key: float(np.mean([r[key] for r in out])) for key in out[0]}
        if self.fit is not None:
            self.last.update(beta=self.fit["beta"], fit_walks=self.fit["walks"],
                             dropped=self.fit["dropped"])
        return dict(self.last)


# ==========================================================================
# the run
# ==========================================================================
def train(cfg: DiscoverConfig):
    assert cfg.fixed_seed and cfg.end_on_hit, "discovery needs a fixed seed and lines that end at a hit"
    if cfg.seed:
        np.random.seed(cfg.seed)
        torch.manual_seed(cfg.seed)
    wb = WandB(cfg)
    env = SimPool(base_config(cfg), seed=cfg.seed)
    agent = FlowAgent(cfg)
    agent.reset_hidden(cfg.n_envs)
    T = rollout_length(cfg)
    print(f"discover (sub-trajectory balance) | {','.join(cfg.boss_levels_list)} seed {cfg.fixed_seed} | "
          f"{env.describe()} | flow scale {cfg.flow_scale} lambda {cfg.flow_lambda} | "
          f"{cfg.flow_steps} x {cfg.flow_batch} walks per {cfg.flow_iter_stores} stores, "
          f"ring {cfg.flow_ring_stores} | rollout {T} x {cfg.n_envs}", flush=True)
    boss_state = make_boss_state(cfg)
    os.makedirs(os.path.dirname(cfg.save_path) or ".", exist_ok=True)
    torch.cuda.set_stream(torch.cuda.Stream(priority=-1))
    learner = AsyncLearner(agent)
    env.reset()
    actor = Actor(agent, env, T, n_stores=cfg.queue_stores)
    rq = RolloutQueue(cfg, env, agent, actor, learner, T)
    record = Record(cfg.record_keep)
    walks = Walks(cfg.n_envs)

    env_steps = last_save = epoch = 0
    steps_per_epoch = T * cfg.n_envs
    t_prev = time.perf_counter()
    stop = {"now": False}

    def _sigint(_sig, _frm):
        if stop["now"]:
            raise KeyboardInterrupt
        stop["now"] = True
        print("\n[interrupt] finishing this store, then saving and stopping", flush=True)

    try:
        signal.signal(signal.SIGINT, _sigint)
    except (ValueError, OSError):
        pass

    while not (stop["now"] or (cfg.total_env_steps and env_steps >= cfg.total_env_steps)
               or (cfg.total_epochs and epoch >= cfg.total_epochs)):
        roll, st = rq.next_store()
        t0, t_prev = t_prev, time.perf_counter()
        lines, recs = walks.feed(roll)
        for line in lines:
            record.add(line)
            if line.kind == "kill":
                walks.kills.append(line)
                p = f"{cfg.save_path}_kill{len(walks.kills)}.npy"
                np.save(p, line.actions)
                print(f"  HITLESS KILL: line of {len(line.actions)} steps, R {line.R:.1f} -> {p}", flush=True)
        m, t_train = rq.metrics()
        m = m or {}
        env_steps += steps_per_epoch
        sps = steps_per_epoch / max(t_prev - t0, 1e-9)
        ent = float(-(roll["lp"] - roll["committed"] * roll["lp_a"]).mean())
        best = record.best()
        wl = float(np.mean([r[2] for r in recs])) if recs else 0.0
        wR = float(np.mean([r[3] for r in recs])) if recs else 0.0
        print(f"ep {epoch:5d} | steps {env_steps:>11,} | {sps:6.0f} sps train {t_train:.2f}s | walks "
              f"{len(recs):4d} x {wl:5.0f} steps, R {wR:5.2f} | record {best.R if best else 0:6.2f} over "
              f"{len(best.actions) if best else 0:5d} steps | kills {len(walks.kills)} | H {ent:.2f} | "
              f"beta {m.get('beta', 0):.3f} subtb {m.get('subtb', 0):9.2f} whole {m.get('whole_gap', 0):8.2f} "
              f"logp_err/step {m.get('logp_err', 0):.4f} gn {m.get('grad_norm', 0):.2f} "
              f"fit {m.get('fit_sps', 0):6.0f} sps", flush=True)
        wb.log({"env_steps": env_steps, "perf/steps_per_s": sps, "perf/train_s": t_train,
                "discover/record": best.R if best else 0.0,
                "discover/record_steps": len(best.actions) if best else 0,
                "discover/walks": len(recs), "discover/walk_len": wl, "discover/walk_R": wR,
                "discover/kills": len(walks.kills), "discover/entropy": ent,
                **{f"flow/{key}": v for key, v in m.items()}}, step=env_steps)
        if env_steps - last_save >= cfg.save_every_steps:
            last_save = env_steps
            rq.learner_idle()
            p = f"{cfg.save_path}_{env_steps}.pth"
            if agent.save_checkpoint(p, env.vocab_i2s(), boss_state, env_steps):
                rotate_checkpoints(cfg.save_path, cfg.keep_last_n)
            record.save(f"{cfg.save_path}_lines.npz")
        rq.submit(dict(roll=roll, store=st, walks=recs), roll)
        epoch += 1

    rq.finish()
    learner.close()
    record.save(f"{cfg.save_path}_lines.npz")
    agent.save_checkpoint(f"{cfg.save_path}_final.pth", env.vocab_i2s(), boss_state, env_steps)
    print(f"done: {env_steps:,} env steps, {epoch} stores, record "
          f"{record.best().R if record.best() else 0:.2f}, hitless kills {len(walks.kills)}")
    actor.close()
    env.close()
    wb.finish()


def main(argv=None):
    cfg = DiscoverConfig.from_cli(argv)
    try:
        train(cfg)
    except KeyboardInterrupt:
        return 130
    finally:
        close_all_pools()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
