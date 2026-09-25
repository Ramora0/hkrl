"""Discovery on one fight: find a hitless line through a boss whose RNG is
fixed, onezero-style (C:/Users/Lee/coding/python/AI/onezero): a sampler
trained toward P(line) ~ exp(beta * R), and restarts from the best lines as
the ratchet. No search, no imitation of stored lines.

    python train/train_discover.py --boss_levels GG_Grimm_Nightmare --save_path runs/x

The fight. Every reset uses fixed_seed, so the boss's draws depend only on
what the knight did: a line (the knight's actions from the reset) replays
exactly. A line ends at the first step that costs health (end_on_hit), and its
return R is the % of boss HP landed before it; a line the boss's death ends
is a hitless kill, R ~ 100.

The sampler is the trainer's policy network, playing every frame. Its loss is
trajectory balance with the group mean for log Z (onezero/flow.py), in the
form that the rollout machinery can train: for a walk it played itself, the
balance gradient is the policy gradient of beta * R plus -log pi on every
step (Tiapkin et al., AISTATS 2024), credited from each step on (onezero's
suffix / critic credit, with the value head as the baseline), so

    r'_t = dmg_t  +  (1 / beta) * (-log pi(a_t|s_t)),   gamma 1, lambda 1,

trained by PPO, whose clip is the floor under a line being pushed away (a
signed weight without one collapses: onezero runs/NIGHT2.md section 2).
beta is set by the run as onezero sets it: flow_scale * (entropy of uniform
play over a walk) / (span of returns seen); uniform play over a step is
ln(3*3*8*2), the four heads' joint, gates not counted.

Restarts (onezero --restart-frac 0.5 --restart-source top). The best
store_capacity lines are banked to the workers every bank_every epochs; a
reset restarts with chance restart_frac from a uniform cut of a banked line,
fast-forwarded through its prefix, and the walk goes on from there. Walks from
one cut share a start, which is what the value head baselines. A finished
walk is stitched onto its prefix and the whole line competes for the store.

Everything else -- the rollout queue, the actor, the sims, the learner, the
checkpoints -- is train.py's.
"""
import math
import os
import signal
import sys
import time
from collections import deque
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # hkpy

import numpy as np                                          # noqa: E402
import torch                                                # noqa: E402

from model import ACT_KEYS                                  # noqa: E402
from observation import mirror_stats                        # noqa: E402
from ppo import NO_UPDATE, AsyncLearner                     # noqa: E402
from rollout import Actor, RolloutQueue                     # noqa: E402
from sim_env import SimPool, close_all_pools                # noqa: E402
from sim_worker import WALK_CUT_BITS                        # noqa: E402
from train import WandB, make_boss_state, rollout_length, rotate_checkpoints  # noqa: E402
from train_hitless import HitlessConfig, HitlessPPO, base_config, effective_logp  # noqa: E402

LOG_UNIFORM_STEP = math.log(3 * 3 * 8 * 2)


@dataclass
class DiscoverConfig(HitlessConfig):
    fixed_seed: int = 1
    end_on_hit: bool = True
    restart_frac: float = 0.5
    # One knight: health plays no part before the first hit.
    train_max_health: str = "9,9"
    start_health_low_p: float = 0.0
    # Credit from each step to the end of its walk (the value head baselines).
    gae_lambda: float = 1.0
    # The temperature is beta's, not an entropy target's.
    target_entropy: float = 0.0
    flow_scale: float = 2.0           # onezero's default greed dial
    alpha_max: float = 1.0            # the temperature before the returns have a span
    store_capacity: int = 256
    bank_every: int = 10              # epochs between banks sent to the workers
    # The evals play fresh seeds (and the game its own RNG): off by default,
    # the record is the store's.
    eval_every_epochs: int = 0
    game_eval: bool = False


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
        ids = np.array(self.top, np.int64)
        lens = np.array([len(self.known[i].actions) for i in self.top], np.int64)
        acts = (np.concatenate([self.known[i].actions for i in self.top]) if self.top
                else np.zeros((0, 4), np.int8))
        np.savez_compressed(path, ids=ids, lens=lens, actions=acts,
                            R=np.array([self.known[i].R for i in self.top], np.float32),
                            kill=np.array([self.known[i].kind == "kill" for i in self.top]))


class Walks:
    """Stitches each env's steps into walks, and each finished walk onto the
    prefix it restarted from."""

    def __init__(self, n_envs, store):
        self.store = store
        self.acts = [[] for _ in range(n_envs)]
        self.dmg = [[] for _ in range(n_envs)]
        self.tag = [None] * n_envs
        self.lens = deque(maxlen=4096)
        self.lo, self.hi = math.inf, -math.inf
        self.kills = []
        self.tag_mismatch = 0

    def feed(self, roll):
        """One store: (T, N) steps. Returns the walks it finished:
        [(line, restarted, walk length)]."""
        T, N = roll["dmg"].shape
        act = np.stack([roll["actions"][k] for k in ACT_KEYS], -1).astype(np.int8)   # (T, N, 4)
        out = []
        for e in range(N):
            for t in range(T):
                tag = int(roll["walk"][t, e])
                if self.tag[e] is None:
                    self.tag[e] = tag
                elif tag != self.tag[e]:
                    self.tag_mismatch += 1
                self.acts[e].append(act[t, e])
                hit = roll["hit"][t, e] > 0
                done = bool(roll["done"][t, e])
                self.dmg[e].append(0.0 if (hit and done) else float(roll["dmg"][t, e]))
                if done:
                    out.append(self._finish(e, "hit" if hit else "kill"))
        return out

    def _finish(self, e, kind):
        acts = np.array(self.acts[e], np.int8)
        dmg = np.array(self.dmg[e], np.float32)
        tag = self.tag[e]
        if tag is not None and tag >= 0:
            prefix = self.store.known[tag >> WALK_CUT_BITS]
            cut = tag & ((1 << WALK_CUT_BITS) - 1)
            acts = np.concatenate([prefix.actions[:cut], acts])
            cum = np.concatenate([prefix.cum[:cut + 1], prefix.cum[cut] + np.cumsum(dmg)])
        else:
            cum = np.concatenate([[0.0], np.cumsum(dmg)]).astype(np.float32)
        line = Line(acts, cum.astype(np.float32), kind)
        n = len(self.dmg[e])
        self.lens.append(n)
        self.lo, self.hi = min(self.lo, line.R), max(self.hi, line.R)
        self.acts[e], self.dmg[e], self.tag[e] = [], [], None
        return line, tag is not None and tag >= 0, n

    def alpha(self, cfg):
        """1 / beta, beta = flow_scale * (uniform play's entropy over a walk)
        / (span of returns seen); the whole range (alpha_max) before any span."""
        span = self.hi - self.lo
        if not self.lens or not np.isfinite(span) or span <= 0:
            return cfg.alpha_max
        beta = cfg.flow_scale * float(np.mean(self.lens)) * LOG_UNIFORM_STEP / span
        return min(cfg.alpha_max, 1.0 / beta)



def train(cfg: DiscoverConfig):
    assert cfg.fixed_seed and cfg.end_on_hit, "discovery needs a fixed seed and lines that end at a hit"
    if cfg.seed:
        np.random.seed(cfg.seed)
        torch.manual_seed(cfg.seed)
    wb = WandB(cfg)
    env = SimPool(base_config(cfg), seed=cfg.seed)
    agent = HitlessPPO(cfg)
    agent.reset_hidden(cfg.n_envs)
    T = rollout_length(cfg)
    steps_per_epoch = T * cfg.n_envs
    print(f"discover | {','.join(cfg.boss_levels_list)} seed {cfg.fixed_seed} | {env.describe()} | "
          f"params {sum(p.numel() for p in agent.policy.parameters()):,} | restart {cfg.restart_frac} "
          f"store {cfg.store_capacity} bank every {cfg.bank_every} | flow scale {cfg.flow_scale} | "
          f"rollout {T} x {cfg.n_envs}", flush=True)
    boss_state = make_boss_state(cfg)
    os.makedirs(os.path.dirname(cfg.save_path) or ".", exist_ok=True)
    torch.cuda.set_stream(torch.cuda.Stream(priority=-1))
    learner = AsyncLearner(agent)
    env.reset()
    actor = Actor(agent, env, T, n_stores=cfg.queue_stores)
    rq = RolloutQueue(cfg, env, agent, actor, learner, T)
    store = LineStore(cfg.store_capacity)
    walks = Walks(cfg.n_envs, store)
    ones = np.ones(cfg.n_envs, np.float32)

    env_steps = last_save = epoch = 0
    t_prev = time.perf_counter()
    stop = {"now": False}

    def _sigint(_sig, _frm):
        if stop["now"]:
            raise KeyboardInterrupt
        stop["now"] = True
        print("\n[interrupt] finishing this epoch, then saving and stopping", flush=True)

    try:
        signal.signal(signal.SIGINT, _sigint)
    except (ValueError, OSError):
        pass

    while not (stop["now"] or (cfg.total_env_steps and env_steps >= cfg.total_env_steps)
               or (cfg.total_epochs and epoch >= cfg.total_epochs)):
        roll, st = rq.next_store()
        t0, t_prev = t_prev, time.perf_counter()
        finished = walks.feed(roll)
        n_restart = 0
        for line, restarted, _n in finished:
            n_restart += restarted
            store.add(line)
            if line.kind == "kill":
                walks.kills.append(line)
                p = f"{cfg.save_path}_kill{len(walks.kills)}.npy"
                np.save(p, line.actions)
                print(f"  HITLESS KILL: line of {len(line.actions)} steps, R {line.R:.1f} -> {p}", flush=True)
        agent.log_alpha = math.log(walks.alpha(cfg))
        if epoch % cfg.bank_every == 0 and store.top:
            env.send_bank(store.bank())
        job = dict(roll=roll, store=st, D_per_env=ones, boss_per_env=list(env.env_boss),
                   value_var_state=boss_state,
                   mstats=mirror_stats(agent.obs_normalizer, agent.combat_normalizer,
                                       agent.terrain_normalizer))
        m, t_train = rq.metrics()
        m = m or NO_UPDATE
        env_steps += steps_per_epoch
        sps = steps_per_epoch / max(t_prev - t0, 1e-9)
        ent = float((-effective_logp(roll)).mean())
        best = store.best()
        top10 = np.mean([store.known[i].R for i in store.top[:10]]) if store.top else 0.0
        wl = np.mean([n for _, _, n in finished]) if finished else 0.0
        print(f"ep {epoch:5d} | steps {env_steps:>11,} | {sps:6.0f} sps | walks {len(finished):4d} "
              f"x {wl:6.0f} steps, {n_restart:4d} restarted | record {best.R if best else 0:6.2f} "
              f"top10 {top10:6.2f} | kills {len(walks.kills)} | alpha {agent.alpha:.4f} H {ent:.2f} | "
              f"kl {m['kl']:.4f} ev {m['ev_atk']:+.2f}", flush=True)
        wb.log({"env_steps": env_steps, "perf/steps_per_s": sps, "perf/train_s": t_train,
                "discover/record": best.R if best else 0.0, "discover/top10": top10,
                "discover/walks": len(finished), "discover/walk_len": wl,
                "discover/restarted": n_restart, "discover/kills": len(walks.kills),
                "discover/alpha": agent.alpha, "discover/entropy": ent,
                "discover/tag_mismatch": walks.tag_mismatch,
                "loss/surrogate": m["surrogate"], "metrics/kl": m["kl"], "metrics/ev": m["ev_atk"],
                "diag/hent_move": m["hent_move"], "diag/hent_dir": m["hent_dir"],
                "diag/hent_act": m["hent_act"], "diag/hent_jump": m["hent_jump"]}, step=env_steps)
        if env_steps - last_save >= cfg.save_every_steps:
            last_save = env_steps
            rq.learner_idle()
            p = f"{cfg.save_path}_{env_steps}.pth"
            if agent.save_checkpoint(p, env.vocab_i2s(), boss_state, env_steps):
                rotate_checkpoints(cfg.save_path, cfg.keep_last_n)
            store.save(f"{cfg.save_path}_lines.npz")
        rq.submit(job, roll)
        epoch += 1

    rq.finish()
    learner.close()
    store.save(f"{cfg.save_path}_lines.npz")
    agent.save_checkpoint(f"{cfg.save_path}_final.pth", env.vocab_i2s(), boss_state, env_steps)
    print(f"done: {env_steps:,} env steps, {epoch} epochs, record "
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
