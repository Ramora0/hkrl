"""The hitless trainer: PPO on "boss damage before the next hit", with the
entropy in the reward.

    python train/train_hitless.py --boss_levels GG_Grimm_Nightmare --save_path runs/x

The objective. A segment runs from any step to the next step that costs the
knight health (any damage, a pit included) or to the end of the fight; its
return is the % of boss HP landed in it, undiscounted (gamma 1). The value
head (the attack critic) learns V(s) = the damage still to come before the
next hit. A hit is a terminal for the return, not for the sim: the fight goes
on and the next step starts a new segment. Damage landed on the step of a hit
is a trade and does not count. The cost of a hit is therefore the damage it
forfeits, V(s), and nothing else: no mask price, no D, no heal term, no
defense critic.

Entropy in the reward (maximum-entropy RL; for a problem where every state
has one history this is the GFlowNet objective, Tiapkin et al. AISTATS 2024):

    r'_t = dmg_t * (1 - hit_t)  +  alpha * (-log pi(a_t|s_t) - H_target)

log pi is the joint log-prob of the four heads, less the action head on a
hard-commit step (the agent did not choose it). alpha is the Lagrange
multiplier of the constraint E[H] >= H_target, adapted every epoch toward it
(SAC's temperature update). Subtracting H_target makes the bonus zero-mean at
the target, so staying alive earns no entropy stream a hit could forfeit.
The PPO loss has no separate entropy term: the reward carries it.

Everything else -- the rollout queue, the actor, the sims, the evals, the
checkpoints -- is train.py's.
"""
import os
import signal
import sys
import time
from dataclasses import dataclass, fields

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # hkpy

import numpy as np                                          # noqa: E402
import torch                                                # noqa: E402

from config import Config                                   # noqa: E402
from observation import mirror_stats                        # noqa: E402
from ppo import NO_UPDATE, PPO, AsyncLearner                # noqa: E402
from rollout import Actor, RolloutQueue                     # noqa: E402
from sim_env import SimPool, close_all_pools                # noqa: E402
from train import (KILL_LANDED_PCT, Evaluator, WandB, forced_str, make_boss_state,  # noqa: E402
                   rollout_length, rotate_checkpoints, short, update_D)


@dataclass
class HitlessConfig(Config):
    gamma: float = 1.0
    # The reward carries the entropy (see the module docstring).
    entropy_coeff: float = 0.0
    # No defense critic: a hit is a terminal of the one return.
    def_value_coeff: float = 0.0
    # Joint entropy of the four heads the temperature holds, in nats (the
    # joint maximum is ln(3*3*8*2) = 4.97).
    target_entropy: float = 2.0
    alpha_init: float = 0.01          # % boss HP per nat
    alpha_lr: float = 0.01            # log-alpha step per epoch per nat of error


def base_config(cfg):
    """The plain Config the sim workers get (they are spawned processes and
    unpickle it by its module)."""
    return Config(**{f.name: getattr(cfg, f.name) for f in fields(Config)})


def effective_logp(roll):
    """(T, N) log-prob of the choices the agent made: the action head is out
    on a hard-commit step."""
    return roll["lp"] - roll["committed"] * roll["lp_a"]


class HitlessPPO(PPO):
    def __init__(self, config):
        super().__init__(config)
        self.log_alpha = float(np.log(config.alpha_init))

    @property
    def alpha(self):
        return float(np.exp(self.log_alpha))

    def adapt_alpha(self, entropy):
        """One temperature step toward the target, from a rollout's mean
        -log pi of the choices made."""
        cfg = self.config
        self.log_alpha += cfg.alpha_lr * (cfg.target_entropy - entropy)

    def soft_reward(self, roll, alpha):
        hit = roll["hit"] > 0
        bonus = alpha * (-effective_logp(roll) - self.config.target_entropy)
        return (np.where(hit, 0.0, roll["dmg"]) + bonus).astype(np.float32)

    def _gae_all(self, reward, hits_taken, hp_healed, values, values_def, D_per_env, dones):
        """GAE with a terminal at every hit and every episode end. Same
        (adv, adv_atk, adv_def, atk_ret, def_ret) layout as PPO's; the defense
        parts are zeros (no defense critic). hp_healed and D are unused."""
        cfg = self.config
        T, N = reward.shape
        gamma, gl = np.float32(cfg.gamma), np.float32(cfg.gamma * cfg.gae_lambda)
        z = np.float32(0.0)
        adv, ret = np.empty((T, N), np.float32), np.empty((T, N), np.float32)
        g = np.zeros(N, np.float32)
        for t in reversed(range(T)):
            term = np.asarray(dones[t], bool) | (hits_taken[t] > 0)
            next_v = np.where(term, z, values[t + 1])
            g = np.where(term, z, g)
            delta = reward[t] + gamma * next_v - values[t]
            g = delta + gl * g
            adv[t] = g
            ret[t] = g + values[t]
        zeros = np.zeros((T, N), np.float32)
        return adv, adv, zeros, ret, zeros

    def rollout_src(self, roll, store, D_per_env, boss_per_env, value_var_state):
        alpha = self.alpha
        src, stats = super().rollout_src(dict(roll, dmg=self.soft_reward(roll, alpha)), store,
                                         D_per_env, boss_per_env, value_var_state)
        stats["alpha"] = alpha
        return src, stats


def segment_stats(roll, seg_landed):
    """Damage per completed segment (landed between hits), carried across
    epochs in seg_landed (N,). Returns the list of completed segments' damage."""
    done_segs = []
    for t in range(roll["dmg"].shape[0]):
        hit = roll["hit"][t] > 0
        seg_landed += np.where(hit, 0.0, roll["dmg"][t])
        end = hit | roll["done"][t]
        done_segs.extend(seg_landed[end].tolist())
        seg_landed[end] = 0.0
    return done_segs


def epoch_log(cfg, agent, epoch, env_steps, t_roll, m, t_train, roll, n_kills, segs, qs, env,
              boss_state):
    """One epoch's stdout line and wandb dict. D is train.py's measure (% of
    boss HP landed per hit taken), reported for comparison, not trained on."""
    sps = roll["dmg"].size / max(t_roll, 1e-9)
    ent = float((-effective_logp(roll)).mean())
    n_done = int(roll["done"].sum())
    seg_mean = float(np.mean(segs)) if segs else 0.0
    D_str = " ".join(f"{short(b)}:{boss_state[b]['D']:.2f}" for b in cfg.boss_levels_list)
    print(f"ep {epoch:5d} | steps {env_steps:>9,} | {sps:6.0f} sps (roll {t_roll:.1f}s "
          f"train {t_train:.1f}s) | D {D_str} | eps {n_done:3d} kills {n_kills:2d} | landed "
          f"{roll['dmg'].sum():8.1f} hits {roll['hit'].sum():6.1f}{forced_str(roll)} | seg "
          f"{len(segs):4d} x {seg_mean:5.2f}% | H {ent:.2f} alpha {agent.alpha:.4f} | surr "
          f"{m['surrogate']:+.4f} kl {m['kl']:.4f} ev {m['ev_atk']:+.2f} | H m/d/a/j "
          f"{m['hent_move']:.2f}/{m['hent_dir']:.2f}/{m['hent_act']:.2f}/{m['hent_jump']:.2f} "
          f"| q batch {qs['batch_mean']:.0f} x{qs['batches']} lag {qs['lag_mean']:.2f}", flush=True)
    log = {
        "env_steps": env_steps, "epoch": epoch,
        "perf/steps_per_s": sps, "perf/rollout_s": t_roll, "perf/train_s": t_train,
        "loss/surrogate": m["surrogate"], "loss/value": m["value_atk"],
        "metrics/ev": m["ev_atk"], "metrics/kl": m["kl"], "metrics/lr": cfg.lr,
        "metrics/return_var": m["atk_return_var"], "metrics/n_updates": m["n_updates"],
        "hitless/alpha": agent.alpha, "hitless/entropy": ent,
        "hitless/target_entropy": cfg.target_entropy,
        "hitless/segments": len(segs), "hitless/segment_landed_mean": seg_mean,
        "rollout/damage_landed": float(roll["dmg"].sum()),
        "rollout/hits_taken": float(roll["hit"].sum()),
        "rollout/hp_healed": float(roll["heal"].sum()),
        "episodes/done": n_done, "episodes/kills": n_kills,
        "episodes/win_rate": n_kills / max(n_done, 1),
        "diag/committed_frac": float(roll["committed"].mean()),
        "diag/gru_norm": m["gru_norm"],
        "diag/hent_move": m["hent_move"], "diag/hent_dir": m["hent_dir"],
        "diag/hent_act": m["hent_act"], "diag/hent_jump": m["hent_jump"],
        "vocab/size": env.vocab_size(), "vocab/unknown_id_rows": int(env.unknown_id_rows()),
        "queue/batches": qs["batches"], "queue/batch_mean": qs["batch_mean"],
        "queue/env_waits": qs["env_waits"], "queue/policy_lag_mean": qs["lag_mean"],
        "queue/server_s": qs["server_s"],
    }
    Ds = []
    for b in cfg.boss_levels_list:
        log[f"curriculum/D/{b}"] = boss_state[b]["D"]
        Ds.append(max(boss_state[b]["D"], 1e-6))
    log["curriculum/D_geomean"] = float(np.exp(np.log(Ds).mean()))
    return log


def hitless_wins(rec, log):
    """Kills with no hit taken, in the sim eval and the game eval of one
    eval record."""
    for b, s in rec.get("sim", {}).items():
        n = sum(1 for e in s["episodes"] if e["landed"] >= KILL_LANDED_PCT and e["hits"] == 0)
        log[f"eval/{short(b)}/hitless_wins"] = n
        print(f"  eval {b}: hitless wins {n}/{s['eps']}", flush=True)
    game = rec.get("game")
    if game:
        eps = game.get("episodes", ())
        n = sum(1 for e in eps if e.get("info") == "win" and e.get("hits", 1) == 0)
        log["game/hitless_wins"] = n
        print(f"  game: hitless wins {n}/{len(eps)}", flush=True)


def train(cfg: HitlessConfig):
    if cfg.seed:
        np.random.seed(cfg.seed)
        torch.manual_seed(cfg.seed)
    wb = WandB(cfg)
    env = SimPool(base_config(cfg), seed=cfg.seed)
    agent = HitlessPPO(cfg)
    agent.reset_hidden(cfg.n_envs)
    T = rollout_length(cfg)
    steps_per_epoch = T * cfg.n_envs
    print(f"hitless | envs {cfg.n_envs} {','.join(cfg.boss_levels_list)} | {env.describe()} | "
          f"params {sum(p.numel() for p in agent.policy.parameters()):,} | gamma {cfg.gamma} "
          f"lambda {cfg.gae_lambda} | H target {cfg.target_entropy} alpha0 {cfg.alpha_init} | "
          f"rollout {T} x {cfg.n_envs} = {steps_per_epoch} steps/epoch", flush=True)
    # The learner's per-boss return-variance state (PPO.rollout_src), and D,
    # measured as train.py does (same lookback and slew limit) for comparison.
    boss_state = make_boss_state(cfg)
    D_max_delta_eff = cfg.D_max_delta * steps_per_epoch / 8192

    os.makedirs(os.path.dirname(cfg.save_path) or ".", exist_ok=True)
    torch.cuda.set_stream(torch.cuda.Stream(priority=-1))
    learner = AsyncLearner(agent)
    env.reset()
    actor = Actor(agent, env, T, n_stores=cfg.queue_stores)
    rq = RolloutQueue(cfg, env, agent, actor, learner, T)
    print(f"actor: {actor.describe()}", flush=True)
    evaluator = Evaluator(cfg)

    env_steps = last_save = epoch = 0
    ep_landed = np.zeros(cfg.n_envs, np.float64)
    seg_landed = np.zeros(cfg.n_envs, np.float64)
    ones = np.ones(cfg.n_envs, np.float32)
    t_run = t_prev = time.perf_counter()
    stop = {"now": False}

    def _sigint(_sig, _frm):
        if stop["now"]:
            raise KeyboardInterrupt
        stop["now"] = True
        print("\n[interrupt] finishing this epoch, then saving and stopping "
              "(Ctrl-C again to abort now)", flush=True)

    try:
        signal.signal(signal.SIGINT, _sigint)
    except (ValueError, OSError):
        pass

    def running():
        return not (stop["now"]
                    or (cfg.total_env_steps and env_steps >= cfg.total_env_steps)
                    or (cfg.total_epochs and epoch >= cfg.total_epochs))

    while running():
        roll, store = rq.next_store()
        t0, t_prev = t_prev, time.perf_counter()
        boss_per_env = list(env.env_boss)
        fresh = ~roll["hard"]
        for b in set(boss_per_env):
            bm = np.array([x == b for x in boss_per_env])[None, :] & fresh
            update_D(cfg, boss_state, b, float(roll["dmg"][bm].sum()),
                     float(roll["hit"][bm].sum()), D_max_delta_eff)
        agent.adapt_alpha(float((-effective_logp(roll)).mean()))
        job = dict(roll=roll, store=store, D_per_env=ones, boss_per_env=boss_per_env,
                   value_var_state=boss_state,
                   mstats=mirror_stats(agent.obs_normalizer, agent.combat_normalizer,
                                       agent.terrain_normalizer))
        m, t_train = rq.metrics()

        env_steps += steps_per_epoch
        n_kills = 0
        for t in range(roll["dmg"].shape[0]):
            ep_landed += roll["dmg"][t]
            d = roll["done"][t]
            n_kills += int((d & (ep_landed >= KILL_LANDED_PCT)).sum())
            ep_landed[d] = 0.0
        segs = segment_stats(roll, seg_landed)
        log = epoch_log(cfg, agent, epoch, env_steps, t_prev - t0, m or NO_UPDATE, t_train,
                        roll, n_kills, segs, rq.snapshot(), env, boss_state)
        wb.log(log, step=env_steps)

        if evaluator.due(epoch):
            t_ev = time.perf_counter()
            rq.pause()
            eval_log, rec = evaluator.run(env, agent, env_steps, epoch)
            hitless_wins(rec, eval_log)
            env.reset()
            agent.reset_hidden(cfg.n_envs)
            rq.resume()
            evaluator.note(time.perf_counter() - t_ev, time.perf_counter() - t_run,
                           eval_log, rec)
            wb.log(eval_log, step=env_steps)
            t_prev = time.perf_counter()

        if env_steps - last_save >= cfg.save_every_steps:
            last_save = env_steps
            rq.learner_idle()
            p = f"{cfg.save_path}_{env_steps}.pth"
            if agent.save_checkpoint(p, env.vocab_i2s(), boss_state, env_steps):
                rotate_checkpoints(cfg.save_path, cfg.keep_last_n)
                print(f"  saved {p}", flush=True)
        rq.submit(job, roll)
        epoch += 1

    rq.finish()
    learner.close()
    if evaluator.fleet is not None and epoch and not stop["now"]:
        t_ev = time.perf_counter()
        eval_log, rec = evaluator.run(env, agent, env_steps, epoch, final=True)
        hitless_wins(rec, eval_log)
        evaluator.note(time.perf_counter() - t_ev, time.perf_counter() - t_run, eval_log, rec)
        wb.log(eval_log, step=env_steps)
    evaluator.close()
    agent.save_checkpoint(f"{cfg.save_path}_final.pth", env.vocab_i2s(), boss_state, env_steps)
    print(f"done: {env_steps:,} env steps, {epoch} epochs, {time.perf_counter() - t_run:.0f}s")
    actor.close()
    env.close()
    wb.finish()
    return env_steps, epoch


def main(argv=None):
    cfg = HitlessConfig.from_cli(argv)
    try:
        train(cfg)
    except KeyboardInterrupt:
        print("\ninterrupted", flush=True)
        return 130
    finally:
        close_all_pools()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
