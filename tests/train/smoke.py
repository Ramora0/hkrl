"""End-to-end smoke test of the training recipe on the real sim (~10 s).
One PASS/FAIL line and a handful of measured numbers.

  1  the sim loads, the pool resets, shapes and row counts as configured, and
     with the game's 9/9 masks every env reaches an episode end and auto-resets
     to the same knight HP
  2  one store through the rollout queue: finite log-probs and values, and no
     sampled action or jump the observation's can_* flags forbid
  3  the real run (train.train on the recipe defaults, small): finite losses,
     updates happen, a greedy eval in the middle, checkpoints written, the
     final checkpoint loads and acts

    python tests/train/smoke.py      (pytest tests/train/smoke.py runs it too)
"""
import os
import sys
import tempfile
import time

import numpy as np
import torch

import _paths  # noqa: F401
import train as train_mod
from config import Config
from model import ACTION_GATES
from observation import GS
from ppo import PPO, AsyncLearner
from rollout import Actor, RolloutQueue
from sim_env import SimPool

FAILS = []


def check(name, ok, detail=""):
    if not ok:
        FAILS.append(f"{name}: {detail}")
    return ok


def smoke_cfg(save_path):
    n = 16
    return Config(n_envs=n, pool_workers=2, total_steps_per_epoch=n * 96, total_epochs=6,
                  eval_every_epochs=3, eval_episodes_per_env=1, eval_max_steps=300,
                  save_every_steps=n * 96 * 3, wandb=False, game_eval=False, seed=1234,
                  save_path=save_path)


def sim_check(cfg):
    env = SimPool(cfg, seed=cfg.seed)
    try:
        obs = env.reset()
        N = cfg.n_envs
        ok = (obs.global_state.shape == (N, cfg.global_state_dim)
              and obs.combat_hb.shape[-1] == cfg.combat_feature_dim
              and obs.terrain_hb.shape[-1] == cfg.terrain_feature_dim
              and env.arrays["n_combat"].max() <= cfg.cap_combat
              and env.arrays["n_terrain"].max() <= cfg.cap_terrain)
        check("shapes", ok, f"gs {obs.global_state.shape} combat {obs.combat_hb.shape}")
        env.set_eval(True)
        env.reset()
        rng = np.random.default_rng(0)
        seen = np.zeros(N, bool)
        hp_after = []
        t = time.perf_counter()
        steps = 0
        while not seen.all() and steps < 20000:
            acts = np.stack([rng.integers(0, n, N) for n in (3, 3, 8, 2)], 1)
            o, _d, _h, _hl, done = env.step(acts)
            steps += 1
            if done.any():
                hp_after.extend(o.global_state[done, GS.HP].tolist())
                seen |= done
        sps = steps * N / (time.perf_counter() - t)
        check("auto_reset", seen.all(), f"{int(seen.sum())}/{N} envs ended in {steps} steps")
        check("reset_hp", bool(hp_after) and min(hp_after) == max(hp_after) > 0,
              f"post-reset knight HP {sorted(set(hp_after))}")
        print(f"1 sim    | {env.describe()} | {int(seen.sum())}/{N} envs ended and "
              f"auto-reset within {steps} steps | post-reset HP {sorted(set(hp_after))} | "
              f"lockstep random play {sps:.0f} steps/s")
    finally:
        env.close()


def rollout_check(cfg):
    env = SimPool(cfg, seed=cfg.seed)
    agent = PPO(cfg)
    agent.reset_hidden(cfg.n_envs)
    learner = AsyncLearner(agent)
    actor = None
    try:
        env.reset()
        T = train_mod.rollout_length(cfg)
        actor = Actor(agent, env, T, n_stores=cfg.queue_stores)
        rq = RolloutQueue(cfg, env, agent, actor, learner, T)
        roll, store = rq.next_store()
        gs = store.buf["global_state"][:T].cpu().numpy()        # the flags are raw
        a, j = roll["actions"]["action"], roll["actions"]["jump"]
        bad_a = sum(int(((a == i) & (gs[..., g] < 0.5)).sum())
                    for i, g in enumerate(ACTION_GATES))
        can_jump = (gs[..., GS.CAN_JUMP] + gs[..., GS.CAN_DOUBLE_JUMP]
                    + gs[..., GS.CAN_WALL_JUMP]) > 0.5
        bad_j = int(((j == 0) & ~can_jump).sum())
        finite = all(np.isfinite(roll[k]).all() for k in ("lp", "lp_a", "v_atk", "v_def"))
        check("rollout_finite", finite, "non-finite log-prob or value")
        check("action_mask", bad_a == 0 and bad_j == 0,
              f"{bad_a} invalid actions, {bad_j} invalid jumps")
        print(f"2 rollout | {T} steps x {cfg.n_envs} envs | invalid action/jump samples "
              f"{bad_a}/{bad_j} | landed {roll['dmg'].sum():.1f} hits {roll['hit'].sum():.1f} "
              f"| committed {100 * roll['committed'].mean():.1f}% | action hist "
              f"{np.bincount(a.ravel(), minlength=8).tolist()}")
        rq.finish()
    finally:
        learner.close()
        if actor is not None:
            actor.close()
        env.close()


def run_check(cfg):
    logs = []
    t = time.perf_counter()
    train_mod.train(cfg, on_epoch=logs.append)
    wall = time.perf_counter() - t
    check("epochs", len(logs) == cfg.total_epochs, f"{len(logs)} epochs")
    check("loss_finite", all(np.isfinite(x["loss/surrogate"]) and np.isfinite(x["loss/value_atk"])
                             for x in logs), "non-finite loss")
    check("updates", logs[-1]["metrics/n_updates"] > 0, "no update landed")
    d = os.path.dirname(cfg.save_path)
    cks = sorted(p for p in os.listdir(d) if p.endswith(".pth"))
    check("checkpoints", f"{os.path.basename(cfg.save_path)}_final.pth" in cks and len(cks) >= 2,
          str(cks))
    agent = PPO(cfg)
    ck = agent.load_checkpoint(cfg.save_path + "_final.pth")
    env = SimPool(cfg, seed=5)
    try:
        agent.reset_hidden(cfg.n_envs)
        acts = agent.act_greedy(env.reset())
        check("ckpt_acts", all(acts[k].shape == (cfg.n_envs,) for k in acts), str(acts))
    finally:
        env.close()
    sps = np.median([x["perf/steps_per_s"] for x in logs[1:]])
    print(f"3 run    | {len(logs)} epochs in {wall:.0f}s, median {sps:.0f} env steps/s at "
          f"n_envs {cfg.n_envs} | surrogate {logs[-1]['loss/surrogate']:+.4f} kl "
          f"{logs[-1]['metrics/kl']:.4f} | checkpoints {cks} | vocab "
          f"{len(ck['kind_vocab_i2s'])}")


def main():
    t = time.perf_counter()
    with tempfile.TemporaryDirectory() as d:
        cfg = smoke_cfg(os.path.join(d, "smoke"))
        sim_check(cfg)
        rollout_check(cfg)
        run_check(cfg)
    print(f"total {time.perf_counter() - t:.0f}s")
    for f in FAILS:
        print(f"  FAILED {f}")
    print("PASS" if not FAILS else "FAIL")
    return 0 if not FAILS else 1


def test_smoke():
    assert main() == 0, FAILS


if __name__ == "__main__":
    if not torch.cuda.is_available():
        print("SKIPPED (no GPU)")
        sys.exit(0)
    sys.exit(main())
