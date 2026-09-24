"""The training run: the rollout queue feeding an asynchronous PPO learner,
the adaptive-difficulty (D) curriculum, evals, logging, checkpoints.

    python train/train.py --boss_levels GG_Grimm_Nightmare --save_path runs/x

A run never resumes: it starts fresh and runs until total_env_steps /
total_epochs, or until Ctrl-C (which finishes the epoch, saves _final.pth and
reaps the workers; a second Ctrl-C aborts).
"""
import os
import signal
import sys
import time
import traceback
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))  # hkpy

import numpy as np                                          # noqa: E402
import torch                                                # noqa: E402

from config import Config                                   # noqa: E402
from model import ACT_KEYS                                  # noqa: E402
from observation import mirror_stats                        # noqa: E402
from ppo import NO_UPDATE, PPO, AsyncLearner                # noqa: E402
from rollout import Actor, RolloutQueue                     # noqa: E402
from sim_env import SimPool, close_all_pools                # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import attack_gap                                           # noqa: E402

# An episode that ends with at least this much of the boss's HP landed counts
# as a kill in the metrics. (The sim ends the episode itself, at the game's
# OnBossesDead, which is also what ends special-death bosses like NKG.)
KILL_LANDED_PCT = 99.95


class WandB:
    """wandb that cannot take the run down: init failure degrades to
    mode="disabled" and every log() is wrapped. The stdout epoch line carries
    everything needed to diagnose a run without it."""

    def __init__(self, cfg):
        self.on = False
        self.fails = 0
        self._w = None
        if not cfg.wandb:
            print("wandb off (--no-wandb)")
            return
        try:
            import wandb as _w
        except Exception as exc:                       # noqa: BLE001
            print(f"wandb off (import failed: {exc!r})")
            return
        self._w = _w
        try:
            _w.init(project=cfg.wandb_project, name=(cfg.wandb_name or None),
                    config=vars(cfg))
        except Exception as exc:                       # noqa: BLE001
            print(f"WARNING: wandb.init failed ({exc!r}); continuing with wandb disabled")
            try:
                _w.init(project=cfg.wandb_project, config=vars(cfg), mode="disabled")
            except Exception:                          # noqa: BLE001
                return
        self.on = True
        run = getattr(_w, "run", None)
        print(f"wandb {getattr(run, 'id', '?')} -> {getattr(run, 'url', None) or 'local'}")

    def log(self, data, step):
        if not self.on:
            return
        try:
            self._w.log(data, step=int(step))
        except Exception as exc:                       # noqa: BLE001
            self.fails += 1
            if self.fails in (1, 10, 100) or self.fails % 1000 == 0:
                print(f"  [wandb] log failed x{self.fails} ({exc!r}); training continues",
                      flush=True)

    def finish(self):
        if self.on:
            try:
                self._w.finish()
            except Exception:                          # noqa: BLE001
                pass


def rollout_length(cfg):
    """Steps per env per epoch, rounded up to a whole number of BPTT chunks."""
    per_env = max(1, cfg.total_steps_per_epoch // cfg.n_envs)
    return max(cfg.seq_len, int(np.ceil(per_env / cfg.seq_len)) * cfg.seq_len)


def make_boss_state(cfg):
    n = cfg.D_event_max_epochs
    return {b: {"D": cfg.D_initial,
                "landed_window": deque(maxlen=n),
                "taken_window": deque(maxlen=n),
                "atk_var_ema": None,
                "def_var_ema": None,
                "rail_window": deque(maxlen=200)}
            for b in cfg.boss_levels_list}


def forced_str(roll):
    """" forced 1.2%" when some steps replayed search demos (HardStarts), else ""."""
    f = float(roll["forced"].mean())
    return f" forced {100 * f:.1f}%" if f > 0 else ""


def update_D(cfg, boss_state, boss, landed, taken, D_max_delta_eff):
    """Adaptive reward scale: D = % of boss HP dealt per hit taken,
    measured from rollouts. The lookback is sized by events (hits), not
    epochs, so the ratio's noise stays ~1/sqrt(k) at every skill level, with a
    damage floor and an epoch floor because damage and hits come in episode
    bursts. A slew clamp bounds per-epoch movement; `rail_window` records how
    often it saturates (over ~5% means D has stopped tracking and is ramping
    open-loop)."""
    bs = boss_state[boss]
    bs["landed_window"].append(landed)
    bs["taken_window"].append(taken)

    w_landed = w_taken = 0.0
    n_back = 0
    for le, te in zip(reversed(bs["landed_window"]), reversed(bs["taken_window"])):
        w_landed += le
        w_taken += te
        n_back += 1
        if (w_taken >= cfg.D_event_window and w_landed >= cfg.D_event_min_landed
                and n_back >= cfg.D_event_min_epochs):
            break

    D_before = bs["D"]
    if w_landed > 0:
        # +0.5 pseudo-count: a hitless window is evidence the true ratio is at
        # least ~2x the window's damage, not that it is infinite.
        D_raw = max(w_landed / (w_taken + 0.5), cfg.D_min)
        if len(bs["landed_window"]) == 1:
            bs["D"] = D_raw
        else:
            bs["D"] = float(np.clip(D_raw, D_before * (1 - D_max_delta_eff),
                                    D_before * (1 + D_max_delta_eff)))
    elif w_taken > 0:
        # taking hits, landing nothing: too hard, drop D
        bs["D"] = float(max(D_before * (1 - D_max_delta_eff), cfg.D_min))

    if D_before > 0:
        r = bs["D"] / D_before
        bs["rail_window"].append(
            1.0 if (r > 1 + 0.99 * D_max_delta_eff or r < 1 - 0.99 * D_max_delta_eff)
            else 0.0)
    return bs["D"]


@torch.no_grad()
def greedy_eval(cfg, env, agent):
    """Argmax play with frozen normalizers on fresh episodes, eval_max_steps
    lockstep steps at most: the deployed policy, not the exploring one. The
    knight plays the game's 9/9 masks. Every step that costs masks is
    attributed as the game eval attributes it (tools/attack_gap.py), per
    episode. Destroys the live episodes; the caller resets afterwards."""
    env.set_eval(True)
    try:
        N = cfg.n_envs
        obs = env.reset()
        agent.reset_hidden(N)
        i2s = env.vocab_i2s()
        eps_done = np.zeros(N, int)
        dmg_ep = np.zeros(N, np.float64)
        hit_ep = np.zeros(N, np.float64)
        steps_ep = np.zeros(N, int)
        hit_by = [[] for _ in range(N)]
        per_boss = {b: {"dmg": 0.0, "hits": 0.0, "eps": 0, "kills": 0, "unfinished": 0, "episodes": []}
                    for b in cfg.boss_levels_list}
        for _ in range(cfg.eval_max_steps):
            if (eps_done >= cfg.eval_episodes_per_env).all():
                break
            acts = agent.act_greedy(obs)
            obs, dmg, hit, _heal, done = env.step(np.stack([acts[k] for k in ACT_KEYS], 1))
            live = eps_done < cfg.eval_episodes_per_env
            dmg_ep += dmg * live
            hit_ep += hit * live
            steps_ep += live
            for i in np.nonzero((hit > 0) & live)[0]:
                if len(i2s) < env.vocab_size():
                    i2s = env.vocab_i2s()
                hit_by[i].append([int(steps_ep[i])] + attack_gap.attribute_rows(
                    obs.combat_hb[i], obs.combat_mask[i], obs.combat_kind_ids[i], obs.combat_parent_ids[i],
                    i2s, obs.global_state[i], bool(done[i])))
            for i in np.nonzero(done & live)[0]:
                b = env.env_boss[i]
                per_boss[b]["dmg"] += dmg_ep[i]
                per_boss[b]["hits"] += hit_ep[i]
                per_boss[b]["eps"] += 1
                per_boss[b]["kills"] += int(dmg_ep[i] >= KILL_LANDED_PCT)
                per_boss[b]["episodes"].append({"landed": float(dmg_ep[i]), "hits": float(hit_ep[i]),
                                                "steps": int(steps_ep[i]), "hit_by": hit_by[i]})
                dmg_ep[i] = hit_ep[i] = 0.0
                steps_ep[i] = 0
                hit_by[i] = []
                eps_done[i] += 1
            if done.any():
                agent.reset_hidden_for(done)
        for i in np.nonzero(eps_done < cfg.eval_episodes_per_env)[0]:
            per_boss[env.env_boss[i]]["unfinished"] += 1
        return per_boss
    finally:
        env.set_eval(False)


def short(b):
    return b[3:] if b.startswith("GG_") else b


class Evaluator:
    """An eval point: the sim greedy eval, the real-game eval (game_eval.py)
    with the game-minus-sim gaps, and one JSON line per eval appended to
    <save_path>_evals.jsonl (every game episode, readable without wandb).
    A failing game eval never takes the run down: it is reported, training
    continues, and GAME_FAILS_TO_DISABLE in a row switch it off."""

    GAME_FAILS_TO_DISABLE = 3

    def __init__(self, cfg):
        self.cfg = cfg
        self.spent = 0.0          # eval seconds so far
        self.n = 0
        self.fleet = None
        self.game_fails = 0
        self.levels = [s.strip() for s in (cfg.game_eval_levels or cfg.boss_levels).split(",")
                       if s.strip()]
        self.path = f"{cfg.save_path}_evals.jsonl"
        if cfg.game_eval:
            import game_eval
            self._ge = game_eval
            try:
                self.fleet = game_eval.GameFleet(cfg)
                print(f"game eval: {cfg.game_eval_episodes} episodes of "
                      f"{','.join(self.levels)} per eval on {cfg.game_n_envs} instances "
                      f"of {cfg.game_path}")
            except Exception:                                   # noqa: BLE001
                print(f"  [game] WARNING: game eval OFF for this run, the fleet could "
                      f"not be set up:\n{traceback.format_exc()}", flush=True)
        if cfg.eval_every_epochs:
            print(f"eval every {cfg.eval_every_epochs} epochs "
                  f"({cfg.eval_every_epochs * cfg.n_envs * rollout_length(cfg):,} env steps)")

    def due(self, epoch):
        return bool(epoch) and bool(self.cfg.eval_every_epochs) and \
            epoch % self.cfg.eval_every_epochs == 0

    def run(self, env, agent, env_steps, epoch, final=False):
        """The evals. Returns (wandb log, jsonl record); note() finishes both."""
        cfg = self.cfg
        log = {}
        rec = {"env_steps": env_steps, "epoch": epoch, "final": final}
        sim = {}
        t = time.perf_counter()
        for b, s in greedy_eval(cfg, env, agent).items():
            if not s["eps"]:
                continue
            sim[b] = {"eps": s["eps"], "landed_per_ep": s["dmg"] / s["eps"],
                      "hits_per_ep": s["hits"] / s["eps"], "kills": s["kills"],
                      "win_rate": s["kills"] / s["eps"], "unfinished": s["unfinished"],
                      "episodes": s["episodes"]}
            print(f"  eval {b}: {s['eps']} eps  landed/ep {s['dmg'] / s['eps']:.1f}  "
                  f"hits/ep {s['hits'] / s['eps']:.2f}  kills {s['kills']} "
                  f"({100 * s['kills'] / s['eps']:.0f}%)"
                  + (f"  unfinished {s['unfinished']}" if s["unfinished"] else ""), flush=True)
            for k in ("landed_per_ep", "hits_per_ep", "win_rate", "unfinished"):
                log[f"eval/{short(b)}/{k}"] = sim[b][k]
            log[f"eval/{short(b)}/episodes"] = s["eps"]
        log["eval/sim_s"] = time.perf_counter() - t
        rec["sim"] = sim
        if self.fleet is not None:
            t = time.perf_counter()
            g = None
            try:
                g = self._ge.run_game_eval(self.fleet, agent, env.vocab_i2s(), self.levels,
                                           cfg.game_eval_episodes, cfg.game_eval_max_s)
                self.game_fails = 0
            except Exception:                                   # noqa: BLE001
                self.game_fails += 1
                print(f"  [game] eval FAILED ({self.game_fails}/{self.GAME_FAILS_TO_DISABLE} "
                      f"in a row); training continues\n{traceback.format_exc()}", flush=True)
                log["game/failures"] = self.game_fails
                rec["game_error"] = traceback.format_exc()
                if self.game_fails >= self.GAME_FAILS_TO_DISABLE:
                    print("  [game] disabling the game eval for the rest of the run", flush=True)
                    self.close()
                    self.fleet = None
            log["eval/game_s"] = time.perf_counter() - t
            if g is not None:
                rec["game"] = g
                self._log_game(g, sim, log)
        return log, rec

    @staticmethod
    def _log_game(g, sim, log):
        log.update({"game/boot_s": g["boot_s"], "game/play_s": g["wall_s"],
                    "game/steps_per_s": g["steps_per_s"], "game/instances": g["instances"],
                    "game/unknown_rows": g["unknown_rows"],
                    "game/cut_episodes": g["cut_episodes"],
                    "game/failed_envs": len(g["failed_envs"])})
        for b, s in g["per_level"].items():
            if not s["eps"]:
                print(f"  game {b}: no completed episodes", flush=True)
                continue
            for k in ("landed_per_ep", "hits_per_ep", "win_rate", "steps_per_ep"):
                log[f"game/{short(b)}/{k}"] = s[k]
            log[f"game/{short(b)}/episodes"] = s["eps"]
            gap = ""
            if b in sim:
                # game minus sim, same weights: the transfer gap itself
                dl = s["landed_per_ep"] - sim[b]["landed_per_ep"]
                dh = s["hits_per_ep"] - sim[b]["hits_per_ep"]
                log[f"transfer/{short(b)}/landed_gap"] = dl
                log[f"transfer/{short(b)}/hits_gap"] = dh
                gap = f"  | game-sim landed {dl:+.1f} hits {dh:+.2f}"
            print(f"  game {b}: {s['eps']} eps  landed/ep {s['landed_per_ep']:.1f}  "
                  f"hits/ep {s['hits_per_ep']:.2f}  win {100 * s['win_rate']:.0f}%  "
                  f"steps/ep {s['steps_per_ep']:.0f}{gap}", flush=True)
        for b in g["per_level"]:
            if b in sim:
                ge = [e for e in g.get("episodes", ()) if e.get("level") == b]
                attack_gap.print_table(ge, sim[b]["episodes"], top=6, out=sys.stdout)
        print(f"  game fleet: {g['instances']} instances, boot {g['boot_s']:.0f}s, "
              f"play {g['wall_s']:.0f}s, {g['steps_per_s']:.0f} steps/s, "
              f"fwd {g['fwd_ms']:.1f} ms x {g['batch_mean']:.1f} envs, "
              f"reset {g['reset_s_mean']:.1f}s"
              + (f", {g['cut_episodes']} cut" if g["cut_episodes"] else "")
              + (f", FAILED envs {g['failed_envs']}" if g["failed_envs"] else "")
              + (f", unknown strings {g['unknown_strings']}" if g["unknown_strings"] else ""),
              flush=True)

    def note(self, seconds, elapsed, log, rec):
        """Book one eval's wall time and write its records."""
        import json
        self.spent += seconds
        self.n += 1
        frac = self.spent / max(elapsed, 1e-9)
        log.update({"eval/wall_s": seconds, "eval/total_s": self.spent,
                    "eval/time_frac": frac})
        print(f"  eval #{self.n}: {seconds:.0f}s, {self.spent:.0f}s of {elapsed:.0f}s so far "
              f"({100 * frac:.1f}%)", flush=True)
        rec.update(eval_s=seconds, run_s=elapsed, time_frac=frac)
        try:
            with open(self.path, "a") as f:
                f.write(json.dumps(rec, default=str) + "\n")
        except OSError as exc:
            print(f"  [eval] could not append {self.path}: {exc!r}", flush=True)

    def close(self):
        if self.fleet is not None:
            try:
                self.fleet.close()
            except Exception:                                   # noqa: BLE001
                pass


def rotate_checkpoints(save_path, keep_last_n):
    """Keep the keep_last_n most recent step-suffixed checkpoints (named ones
    like _final.pth are never touched)."""
    import glob
    import re
    d = os.path.dirname(save_path) or "."
    stem = os.path.basename(save_path)
    pat = re.compile(re.escape(stem) + r"_(\d+)\.pth$")
    found = sorted(((int(m.group(1)), p) for p in glob.glob(os.path.join(d, stem + "_*.pth"))
                    if (m := pat.match(os.path.basename(p)))), reverse=True)
    for _, p in found[keep_last_n:]:
        try:
            os.remove(p)
        except OSError:
            pass


def epoch_log(cfg, epoch, env_steps, t_roll, m, t_train, roll, D_per_env, boss_state,
              boss_per_env, n_kills, qs, env):
    """One epoch's stdout line and wandb dict."""
    sps = roll["dmg"].size / max(t_roll, 1e-9)
    t_wait = qs["learner_wait_s"]
    reward = float((cfg.attack_weight * roll["dmg"] / D_per_env[None, :]
                    - roll["hit"] + cfg.heal_coef * roll["heal"]).mean())
    n_done = int(roll["done"].sum())
    rail = max((float(np.mean(boss_state[b]["rail_window"]))
                for b in cfg.boss_levels_list if boss_state[b]["rail_window"]), default=0.0)
    D_str = " ".join(f"{short(b)}:{boss_state[b]['D']:.2f}" for b in cfg.boss_levels_list)
    print(f"ep {epoch:5d} | steps {env_steps:>9,} | {sps:6.0f} sps (roll {t_roll:.1f}s "
          f"train {t_train:.1f}s wait {t_wait:.1f}s) | rew {reward:+.4f} | D {D_str} "
          f"rail {100 * rail:3.0f}% | eps {n_done:3d} kills {n_kills:2d} | landed "
          f"{roll['dmg'].sum():8.1f} hits {roll['hit'].sum():6.1f} heal "
          f"{roll['heal'].sum():3.0f}{forced_str(roll)} | surr {m['surrogate']:+.4f} kl {m['kl']:.4f} "
          f"ev_a {m['ev_atk']:+.2f} ev_d {m['ev_def']:+.2f} | H m/d/a/j "
          f"{m['hent_move']:.2f}/{m['hent_dir']:.2f}/{m['hent_act']:.2f}/{m['hent_jump']:.2f} "
          f"| q batch {qs['batch_mean']:.0f} x{qs['batches']} lag {qs['lag_mean']:.2f} "
          f"waits {qs['env_waits']}", flush=True)
    log = {
        "env_steps": env_steps, "epoch": epoch,
        "perf/steps_per_s": sps, "perf/rollout_s": t_roll, "perf/train_s": t_train,
        "perf/learner_wait_s": t_wait,
        "loss/surrogate": m["surrogate"], "loss/value_atk": m["value_atk"],
        "loss/value_def": m["value_def"], "loss/entropy": m["entropy"],
        "metrics/ev_atk": m["ev_atk"], "metrics/ev_def": m["ev_def"], "metrics/kl": m["kl"],
        "metrics/lr": cfg.lr, "metrics/atk_return_var": m["atk_return_var"],
        "metrics/def_return_var": m["def_return_var"], "metrics/n_updates": m["n_updates"],
        "rollout/curriculum_reward": reward,
        "rollout/damage_landed": float(roll["dmg"].sum()),
        "rollout/hits_taken": float(roll["hit"].sum()),
        "rollout/hp_healed": float(roll["heal"].sum()),
        "rollout/deaths": n_done,
        "episodes/done": n_done, "episodes/kills": n_kills,
        "episodes/win_rate": n_kills / max(n_done, 1),
        "diag/committed_frac": float(roll["committed"].mean()),
        "diag/forced_frac": float(roll["forced"].mean()),
        "diag/gru_norm": m["gru_norm"],
        "diag/hent_move": m["hent_move"], "diag/hent_dir": m["hent_dir"],
        "diag/hent_act": m["hent_act"], "diag/hent_jump": m["hent_jump"],
        # The shared id space's size, the ids assigned after startup, and live
        # rows that resolved to id 0 (never legitimate: a non-zero value means
        # the id space is full and strings are being dropped).
        "vocab/size": env.vocab_size(),
        "vocab/late_strings": env.late_string_count(),
        "vocab/unknown_id_rows": int(env.unknown_id_rows()),
        "curriculum/rail_frac_max": rail,
        # Since the previous store: GPU batch sizes, env waits at the store
        # bound, and the policy lag of the steps whose update started.
        "queue/batches": qs["batches"], "queue/batch_mean": qs["batch_mean"],
        "queue/batch_p10": qs["batch_p10"], "queue/batch_p90": qs["batch_p90"],
        "queue/env_waits": qs["env_waits"], "queue/env_wait_s": qs["env_wait_s"],
        "queue/policy_lag_mean": qs["lag_mean"], "queue/server_s": qs["server_s"],
    }
    Ds = []
    for b in cfg.boss_levels_list:
        log[f"curriculum/D/{b}"] = boss_state[b]["D"]
        Ds.append(max(boss_state[b]["D"], 1e-6))
        if boss_state[b]["rail_window"]:
            log[f"curriculum/rail_frac/{b}"] = float(np.mean(boss_state[b]["rail_window"]))
        bm = np.array([x == b for x in boss_per_env])
        log[f"rollout/landed/{b}"] = float(roll["dmg"][:, bm].sum())
        log[f"rollout/hits/{b}"] = float(roll["hit"][:, bm].sum())
        log[f"rollout/active_envs/{b}"] = int(bm.sum())
        if b in m["adv_std_by_boss"]:
            log[f"metrics/adv_std/{b}"] = m["adv_std_by_boss"][b]
    log["curriculum/D_geomean"] = float(np.exp(np.log(Ds).mean()))
    return log


def train(cfg: Config, on_epoch=None):
    """The run. `on_epoch(log)`, if given, receives each epoch's wandb dict."""
    if cfg.seed:
        np.random.seed(cfg.seed)
        torch.manual_seed(cfg.seed)
    wb = WandB(cfg)
    env = SimPool(cfg, seed=cfg.seed)
    agent = PPO(cfg)
    agent.reset_hidden(cfg.n_envs)
    T = rollout_length(cfg)
    counts = {b: env.env_boss.count(b) for b in cfg.boss_levels_list}
    print(f"envs {cfg.n_envs} " + " ".join(f"{b}:{n}" for b, n in counts.items())
          + f" | {env.describe()} | params "
          f"{sum(p.numel() for p in agent.policy.parameters()):,} | fpw {cfg.frames_per_wait} | "
          f"lr {cfg.lr:g} | rollout {T} x {cfg.n_envs} = {T * cfg.n_envs} steps/epoch | "
          f"seq_len {cfg.seq_len} | horizon "
          + (f"{cfg.total_env_steps:,} env steps" if cfg.total_env_steps else "until killed"))

    # The D slew limit was tuned at 8192 steps per epoch; rescale it to what
    # an epoch actually holds.
    steps_per_epoch = T * cfg.n_envs
    D_max_delta_eff = cfg.D_max_delta * steps_per_epoch / 8192
    boss_state = make_boss_state(cfg)

    os.makedirs(os.path.dirname(cfg.save_path) or ".", exist_ok=True)
    # High priority: the actor's small forwards should not queue behind the
    # learner's long update.
    torch.cuda.set_stream(torch.cuda.Stream(priority=-1))
    learner = AsyncLearner(agent)
    env.reset()
    actor = Actor(agent, env, T, n_stores=cfg.queue_stores)
    rq = RolloutQueue(cfg, env, agent, actor, learner, T)
    print(f"actor: {actor.describe()} | batch >= {rq.qb} or {cfg.queue_timeout_us} us | "
          f"{cfg.queue_stores} stores | {cfg.queue_split} parts per worker", flush=True)
    evaluator = Evaluator(cfg)

    env_steps = last_save = epoch = 0
    ep_landed = np.zeros(cfg.n_envs, np.float64)   # this episode's landed, per env
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
        pass                                  # not on the main thread

    def running():
        return not (stop["now"]
                    or (cfg.total_env_steps and env_steps >= cfg.total_env_steps)
                    or (cfg.total_epochs and epoch >= cfg.total_epochs))

    while running():
        # An epoch is one completed store (every env's next T steps); the
        # workers never stop, so its time is the time since the previous one.
        roll, store = rq.next_store()
        t0, t_prev = t_prev, time.perf_counter()

        boss_per_env = list(env.env_boss)
        # D is the exchange rate of fresh fights: hard-start episodes (HardStarts)
        # begin at the attacks that hit, and would drag it down.
        fresh = ~roll["hard"]
        for b in set(boss_per_env):
            bm = np.array([x == b for x in boss_per_env])[None, :] & fresh
            update_D(cfg, boss_state, b, float(roll["dmg"][bm].sum()),
                     float(roll["hit"][bm].sum()), D_max_delta_eff)
        D_per_env = np.array([boss_state[b]["D"] for b in boss_per_env], np.float32)
        job = dict(roll=roll, store=store, D_per_env=D_per_env, boss_per_env=boss_per_env,
                   value_var_state=boss_state,
                   # the statistics this rollout ended on, for the learner's mirror
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
        log = epoch_log(cfg, epoch, env_steps, t_prev - t0, m or NO_UPDATE, t_train, roll,
                        D_per_env, boss_state, boss_per_env, n_kills, rq.snapshot(), env)
        wb.log(log, step=env_steps)
        if on_epoch is not None:
            on_epoch(log)

        if evaluator.due(epoch):
            t_ev = time.perf_counter()
            rq.pause()                # workers and learner idle, episodes cut here
            eval_log, rec = evaluator.run(env, agent, env_steps, epoch)
            env.reset()
            agent.reset_hidden(cfg.n_envs)
            rq.resume()
            evaluator.note(time.perf_counter() - t_ev, time.perf_counter() - t_run,
                           eval_log, rec)
            wb.log(eval_log, step=env_steps)
            t_prev = time.perf_counter()   # the next epoch does not carry the eval

        if env_steps - last_save >= cfg.save_every_steps:
            last_save = env_steps
            rq.learner_idle()
            p = f"{cfg.save_path}_{env_steps}.pth"
            if agent.save_checkpoint(p, env.vocab_i2s(), boss_state, env_steps):
                rotate_checkpoints(cfg.save_path, cfg.keep_last_n)
                print(f"  saved {p}", flush=True)
        rq.submit(job, roll)          # starts once the learner is idle
        epoch += 1

    rq.finish()                       # workers idle; every completed store trained
    learner.close()
    # A run that reached its horizon ends on the weights it saves, so those
    # get the last point of the curve. Not after a Ctrl-C.
    if evaluator.fleet is not None and epoch and not stop["now"]:
        t_ev = time.perf_counter()
        eval_log, rec = evaluator.run(env, agent, env_steps, epoch, final=True)
        evaluator.note(time.perf_counter() - t_ev, time.perf_counter() - t_run, eval_log, rec)
        wb.log(eval_log, step=env_steps)
    evaluator.close()
    agent.save_checkpoint(f"{cfg.save_path}_final.pth", env.vocab_i2s(), boss_state,
                          env_steps)
    print(f"done: {env_steps:,} env steps, {epoch} epochs, "
          f"{time.perf_counter() - t_run:.0f}s")
    actor.close()                     # unpin the raw block before the pool frees it
    env.close()
    wb.finish()
    return env_steps, epoch


def main(argv=None):
    cfg = Config.from_cli(argv)
    # Teardown here too, so a crash during setup, a worker death or an
    # unhandled Ctrl-C still reaps the workers and unlinks the shared memory.
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
