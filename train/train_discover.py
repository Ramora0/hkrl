"""Discovery on one fight: find a hitless line through a boss whose RNG is
fixed, the onezero way (C:/Users/Lee/coding/python/AI/onezero): a sampler fit
by trajectory balance, P(line) ~ exp(beta * R) (flow.py), and restarts from
the best lines as the ratchet. No search, no imitation of stored lines, no
policy-gradient loss.

    python train/train_discover.py --boss_levels GG_Grimm_Nightmare --save_path runs/x

The fight. Every reset uses fixed_seed, so the boss's draws depend only on
what the knight did and a line (the knight's actions from the reset) replays
exactly. A line ends at the first step that costs health (end_on_hit); its
return R is the % of boss HP landed before it. A line the boss's death ends
is a hitless kill.

The sampler is the trainer's policy network, playing every frame. A walk is
what it played from a reset or a restart: its log P is the sum of its steps'
log-probs as the actor sampled them (the action head left out on a
hard-commit step, where the agent did not choose it), with the GRU starting
from zero at the walk's first step, as the actor's did.

The fit, as onezero's train_flow. An iteration is flow_iter_stores stores of
walks. At its end: beta = flow_scale * (entropy of uniform play over the root
walks) / (span of returns seen), uniform play over a step being
ln(3*3*8*2) (the four heads' joint, gates not counted); every walk's gap
delta = log P - beta * R - c against its restart group (flow.gaps); and
flow_steps * flow_batch draws in proportion to |delta|, weighted by
1 / max(|delta|, 1e-3 sigma). Each step recomputes the drawn walks' log P
under the network being trained -- the walk's own steps, gathered from a ring
of stored observations into one sequence per walk -- and descends
weight * (log P - beta * R - c)^2 / 2: first every walk's log P without
gradients, then the walks' steps carrying their walk's coefficient (the same
gradient, a device's worth of steps at a time). An iteration's steps run
spread over the next iteration's stores, beside the rollouts.

Restarts (onezero --restart-frac 0.5 --restart-source top): the best
store_capacity lines are banked to the workers every bank_every stores; a
reset restarts with chance restart_frac from a uniform cut of a banked line,
fast-forwarded through its prefix. Walks from one cut are one group. A
finished walk is stitched onto its prefix and the whole line competes for
the store.
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
from sim_worker import WALK_CUT_BITS                        # noqa: E402
from train import WandB, make_boss_state, rollout_length, rotate_checkpoints  # noqa: E402

LOG_UNIFORM_STEP = math.log(3 * 3 * 8 * 2)


@dataclass
class DiscoverConfig(Config):
    fixed_seed: int = 1
    end_on_hit: bool = True
    restart_frac: float = 0.5
    # One knight: health plays no part before the first hit.
    train_max_health: str = "9,9"
    start_health_low_p: float = 0.0
    # The evals play fresh seeds (and the game its own RNG); the record is the store's.
    eval_every_epochs: int = 0
    game_eval: bool = False
    store_capacity: int = 256
    bank_every: int = 10              # stores between banks sent to the workers
    # The fit (onezero's --flow-* defaults where they carry over).
    flow_scale: float = 2.0           # beta's greed dial
    flow_steps: int = 12              # optimizer steps per iteration: the fit keeps pace with the rollouts
    flow_batch: int = 256             # whole walks per step
    flow_clip: float = 0.0            # Huber bound in gap sd; 0 = quadratic
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
    __slots__ = ("actions", "cum", "kind")

    def __init__(self, actions, cum, kind):
        self.actions = actions            # (L, 4) int8
        self.cum = cum                    # (L + 1,) float32: damage landed before ply i
        self.kind = kind                  # "hit" or "kill"

    @property
    def R(self):
        return float(self.cum[-1])


class LineStore:
    """The best lines by R. A line that ever entered stays resolvable by id
    (walks restarted from it may still be running when it is evicted)."""

    def __init__(self, capacity):
        self.capacity = capacity
        self.known = {}
        self.top = []                     # ids, best first
        self.next_id = 0

    def add(self, line):
        if len(self.top) >= self.capacity and line.R <= self.known[self.top[-1]].R:
            return None
        lid = self.next_id
        self.next_id += 1
        self.known[lid] = line
        self.top.append(lid)
        self.top.sort(key=lambda i: -self.known[i].R)
        del self.top[self.capacity:]
        return lid

    def best(self):
        return self.known[self.top[0]] if self.top else None

    def bank(self):
        return [(i, self.known[i].actions) for i in self.top]

    def save(self, path):
        acts = (np.concatenate([self.known[i].actions for i in self.top]) if self.top
                else np.zeros((0, 4), np.int8))
        np.savez_compressed(path, ids=np.array(self.top, np.int64),
                            lens=np.array([len(self.known[i].actions) for i in self.top], np.int64),
                            actions=acts,
                            R=np.array([self.known[i].R for i in self.top], np.float32),
                            kill=np.array([self.known[i].kind == "kill" for i in self.top]))


class Walks:
    """Each env's steps into walks, each finished walk stitched onto the
    prefix it restarted from. A walk's record for the fit: (env, first ply of
    the env's timeline, length, group tag, R of the whole line, log P as
    sampled)."""

    def __init__(self, n_envs, store):
        self.store = store
        self.acts = [[] for _ in range(n_envs)]
        self.dmg = [[] for _ in range(n_envs)]
        self.logp = np.zeros(n_envs)
        self.first = np.zeros(n_envs, np.int64)
        self.tag = [None] * n_envs
        self.kills = []
        self.tag_mismatch = 0

    def feed(self, roll):
        """One store. -> (lines, records) of the walks it finished."""
        T, N = roll["dmg"].shape
        base = int(roll["_k"]) * T
        act = np.stack([roll["actions"][k] for k in ACT_KEYS], -1).astype(np.int8)    # (T, N, 4)
        lp = roll["lp"] - roll["committed"] * roll["lp_a"]
        lines, recs = [], []
        for e in range(N):
            for t in range(T):
                tag = int(roll["walk"][t, e])
                if self.tag[e] is None:
                    self.tag[e] = tag
                    self.first[e] = base + t
                    self.logp[e] = 0.0
                elif tag != self.tag[e]:
                    self.tag_mismatch += 1
                self.acts[e].append(act[t, e])
                self.logp[e] += float(lp[t, e])
                hit = roll["hit"][t, e] > 0
                done = bool(roll["done"][t, e])
                self.dmg[e].append(0.0 if (hit and done) else float(roll["dmg"][t, e]))
                if done:
                    line = self._finish(e, "hit" if hit else "kill")
                    lines.append(line)
                    recs.append((e, int(self.first[e]), len(self.dmg[e]), self.tag[e], line.R,
                                 float(self.logp[e])))
                    self.acts[e], self.dmg[e], self.tag[e] = [], [], None
        return lines, recs

    def _finish(self, e, kind):
        acts = np.array(self.acts[e], np.int8)
        dmg = np.array(self.dmg[e], np.float32)
        tag = self.tag[e]
        if tag >= 0:
            prefix = self.store.known[tag >> WALK_CUT_BITS]
            cut = tag & ((1 << WALK_CUT_BITS) - 1)
            acts = np.concatenate([prefix.actions[:cut], acts])
            cum = np.concatenate([prefix.cum[:cut + 1], prefix.cum[cut] + np.cumsum(dmg)])
        else:
            cum = np.concatenate([[0.0], np.cumsum(dmg)])
        return Line(acts, cum.astype(np.float32), kind)


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
    WANT = {"combat_hb": torch.float32, "combat_mask": torch.float32,
            "combat_kind_ids": torch.int64, "combat_parent_ids": torch.int64,
            "terrain_hb": torch.float32, "terrain_mask": torch.float32,
            "global_state": torch.float32}

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
        slot = k % self.config.flow_ring_stores
        torch.cuda.current_stream().wait_event(store.ready)
        for key, dt in self.RING.items():
            self.ring[key][slot].copy_(store.buf[key][:T].to(dt))
        act = np.stack([roll["actions"][a] for a in ACT_KEYS], -1).astype(np.int8)
        self.ring["actions"][slot].copy_(torch.from_numpy(act))
        self.ring["committed"][slot].copy_(torch.from_numpy(roll["committed"].astype(bool)))

    def _line_logp(self, env, first, length):
        """log P of whole walks under the current network, (B,) with autograd:
        each walk's steps gathered into one sequence, GRU from zero."""
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
        committed = take("committed", torch.float32)
        hx = torch.zeros((B, self.config.gru_dim), device=dev)
        lp, _ent, _va, _vd, _info, lp_a, _ea = self.policy.forward_sequence(obs, hx, actions)
        live = (steps[None, :] < ln[:, None]).float()
        return ((lp - committed * lp_a) * live).sum(1)

    # ------------------------------------------------------------ the fit
    def _begin_fit(self, k_now):
        """The iteration's walks -> beta, gaps, draws (onezero trainer._flow_rows)."""
        cfg = self.config
        if not self.batch:
            return
        env, first, length, tag, value, logp = (np.array(c) for c in zip(*self.batch))
        # every walk has to stay in the ring until the fit of this iteration ends
        oldest_needed = k_now + cfg.flow_iter_stores - cfg.flow_ring_stores + 1
        keep = (first // self.T) >= oldest_needed
        dropped = int((~keep).sum())
        env, first, length, tag, value, logp = (a[keep] for a in (env, first, length, tag, value, logp))
        if env.size == 0:
            return
        root = tag < 0
        _, group = np.unique(np.where(root, -1, tag), return_inverse=True)
        group = np.where(root, -1, group - (1 if root.any() else 0))
        self.seen_lo = min(self.seen_lo, float(value.min()))
        self.seen_hi = max(self.seen_hi, float(value.max()))
        logu = -length * LOG_UNIFORM_STEP
        beta = flow.inverse_temperature(logu[root] if root.any() else logu,
                                        self.seen_lo, self.seen_hi, cfg.flow_scale)
        delta = flow.gaps(value, logp, group, beta)
        sigma = float(delta.std())
        bound = cfg.flow_clip * sigma if cfg.flow_clip > 0 else float("inf")
        pull = flow.reaches(delta, bound)
        self.fit = dict(env=env, first=first, length=length, logp=logp, base=logp - delta,
                        weight=1.0 / np.maximum(np.abs(pull), 1e-3 * max(sigma, 1e-6)),
                        draws=flow.draw_lines(pull, cfg.flow_steps * cfg.flow_batch, self.rng),
                        bound=bound, step=0, beta=beta, sigma=sigma, walks=int(env.size),
                        dropped=dropped)

    def _fit_step(self):
        cfg, f = self.config, self.fit
        j = f["step"]
        f["step"] += 1
        li = f["draws"][j * cfg.flow_batch:(j + 1) * cfg.flow_batch]
        t0 = time.perf_counter()
        if li.size == 0:
            return None
        order = li[np.argsort(f["length"][li])]
        # micro-batches of walks by length, a device's worth of steps each
        parts, start = [], 0
        while start < order.size:
            n = 1
            while start + n < order.size and (n + 1) * int(f["length"][order[start + n]]) <= cfg.flow_micro:
                n += 1
            parts.append(order[start:start + n])
            start += n
        # pass 1: every drawn walk's log P now
        with torch.no_grad():
            now = torch.cat([self._line_logp(f["env"][q], f["first"][q], f["length"][q]) for q in parts])
        drawn = np.concatenate(parts)
        gap = now.double().cpu().numpy() - f["base"][drawn]
        coef = f["weight"][drawn] * np.clip(gap, -f["bound"], f["bound"]) / drawn.size
        # pass 2: the steps carry their walk's coefficient
        self.optimizer.zero_grad(set_to_none=False)
        at = 0
        for q in parts:
            c = torch.as_tensor(coef[at:at + q.size], dtype=torch.float32, device=self.device)
            (c * self._line_logp(f["env"][q], f["first"][q], f["length"][q])).sum().backward()
            at += q.size
        gn = float(torch.nn.utils.clip_grad_norm_(self.policy.parameters(), cfg.max_grad_norm))
        self.optimizer.step()
        self.policy.refresh_shadow()
        w = f["weight"][drawn]
        return {"imbalance": float(np.sum(w * np.abs(gap)) / np.sum(w)),
                "fit_sps": float(f["length"][drawn].sum()) / max(time.perf_counter() - t0, 1e-9),
                "logp_err": float(np.mean(np.abs(now.double().cpu().numpy() - f["logp"][drawn])
                                          / f["length"][drawn])),
                "grad_norm": gn, "steps": int(f["length"][drawn].sum())}

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
            per_store = -(-cfg.flow_steps // cfg.flow_iter_stores)
            for _ in range(per_store):
                if self.fit["step"] >= cfg.flow_steps:
                    break
                r = self._fit_step()
                if r is not None:
                    out.append(r)
        if out:
            self.last = {key: float(np.mean([r[key] for r in out])) for key in out[0]}
            self.last["fit_steps"] = len(out)
        if self.fit is not None:
            self.last.update(beta=self.fit["beta"], sigma=self.fit["sigma"],
                             fit_walks=self.fit["walks"], dropped=self.fit["dropped"])
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
    print(f"discover (flow) | {','.join(cfg.boss_levels_list)} seed {cfg.fixed_seed} | {env.describe()} | "
          f"restart {cfg.restart_frac} store {cfg.store_capacity} | flow scale {cfg.flow_scale} "
          f"{cfg.flow_steps} x {cfg.flow_batch} walks per {cfg.flow_iter_stores} stores, ring "
          f"{cfg.flow_ring_stores} | rollout {T} x {cfg.n_envs}", flush=True)
    boss_state = make_boss_state(cfg)
    os.makedirs(os.path.dirname(cfg.save_path) or ".", exist_ok=True)
    torch.cuda.set_stream(torch.cuda.Stream(priority=-1))
    learner = AsyncLearner(agent)
    env.reset()
    actor = Actor(agent, env, T, n_stores=cfg.queue_stores)
    rq = RolloutQueue(cfg, env, agent, actor, learner, T)
    store = LineStore(cfg.store_capacity)
    walks = Walks(cfg.n_envs, store)

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
        n_restart = sum(r[3] >= 0 for r in recs)
        for line in lines:
            store.add(line)
            if line.kind == "kill":
                walks.kills.append(line)
                p = f"{cfg.save_path}_kill{len(walks.kills)}.npy"
                np.save(p, line.actions)
                print(f"  HITLESS KILL: line of {len(line.actions)} steps, R {line.R:.1f} -> {p}", flush=True)
        if epoch % cfg.bank_every == 0 and store.top:
            env.send_bank(store.bank())
        m, t_train = rq.metrics()
        m = m or {}
        env_steps += steps_per_epoch
        sps = steps_per_epoch / max(t_prev - t0, 1e-9)
        ent = float(-(roll["lp"] - roll["committed"] * roll["lp_a"]).mean())
        best = store.best()
        top10 = float(np.mean([store.known[i].R for i in store.top[:10]])) if store.top else 0.0
        wl = float(np.mean([r[2] for r in recs])) if recs else 0.0
        print(f"ep {epoch:5d} | steps {env_steps:>11,} | {sps:6.0f} sps train {t_train:.2f}s | walks "
              f"{len(recs):4d} x {wl:5.0f}, {n_restart:3d} restarted | record {best.R if best else 0:6.2f} "
              f"top10 {top10:6.2f} | kills {len(walks.kills)} | H {ent:.2f} | beta {m.get('beta', 0):.4f} "
              f"imbalance {m.get('imbalance', 0):7.2f} logp_err/step {m.get('logp_err', 0):.4f} "
              f"gn {m.get('grad_norm', 0):.2f} fit {m.get('fit_sps', 0):6.0f} sps", flush=True)
        wb.log({"env_steps": env_steps, "perf/steps_per_s": sps, "perf/train_s": t_train,
                "discover/record": best.R if best else 0.0, "discover/top10": top10,
                "discover/walks": len(recs), "discover/walk_len": wl, "discover/restarted": n_restart,
                "discover/kills": len(walks.kills), "discover/entropy": ent,
                "discover/tag_mismatch": walks.tag_mismatch,
                **{f"flow/{key}": v for key, v in m.items()}}, step=env_steps)
        if env_steps - last_save >= cfg.save_every_steps:
            last_save = env_steps
            rq.learner_idle()
            p = f"{cfg.save_path}_{env_steps}.pth"
            if agent.save_checkpoint(p, env.vocab_i2s(), boss_state, env_steps):
                rotate_checkpoints(cfg.save_path, cfg.keep_last_n)
            store.save(f"{cfg.save_path}_lines.npz")
        rq.submit(dict(roll=roll, store=st, walks=recs), roll)
        epoch += 1

    rq.finish()
    learner.close()
    store.save(f"{cfg.save_path}_lines.npz")
    agent.save_checkpoint(f"{cfg.save_path}_final.pth", env.vocab_i2s(), boss_state, env_steps)
    print(f"done: {env_steps:,} env steps, {epoch} stores, record "
          f"{store.best().R if store.top else 0:.2f}, hitless kills {len(walks.kills)}")
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
