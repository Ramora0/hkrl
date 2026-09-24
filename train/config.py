"""Every knob the trainer reads, one dataclass, a --flag for each. The
defaults are the training recipe."""
import argparse
import os
from dataclasses import dataclass, fields


@dataclass
class Config:
    # ---------------------------------------------------------------- sim
    boss_levels: str = "GG_Hornet_1"   # comma-separated; env i plays levels[i % k]
    # 128 x 64 steps is the 8192-step epoch the D curriculum was tuned at.
    n_envs: int = 128
    # Sim worker processes (sim_env.py). Past ~12 the workers contend with the
    # trainer's own cores; fewer leave cores free on a shared machine.
    pool_workers: int = 12
    # Sim frames per agent step: 1 decides every frame (50 Hz). Everything
    # counted in steps scales with it: gamma, gae_lambda, seq_len, the eval
    # step budgets.
    frames_per_wait: int = 1
    # Row capacities of the shared buffers and the actor's fixed widths. The
    # sim writing more rows than cap_combat / cap_terrain, or more terrain in
    # view than cap_terrain_view, is an error, never a silent truncation.
    cap_combat: int = 64
    cap_terrain: int = 128
    cap_terrain_view: int = 64

    # ------------------------------------------------------- observation
    # Combat: [rel_x, rel_y, w, h, vel_x, vel_y, is_trigger, gives_damage,
    #          takes_damage, is_target, is_invincible, hp_raw, hp_max_raw,
    #          anim_phase]  (obs-wire.md S3.3)
    combat_feature_dim: int = 14
    combat_normalized_dims: int = 6   # first N cols z-scored; flags raw; hp log1p; anim_phase raw
    terrain_feature_dim: int = 8      # [mx, my, hdx, hdy, npx, npy, dist, is_trigger]
    terrain_normalized_dims: int = 7  # is_trigger raw
    global_state_dim: int = 33
    n_binary_flags: int = 27          # 7 ability + 9 validity + 11 commit, all raw

    # ------------------------------------------------------------- model
    # The fused kernels (hkkern.py) are compiled for exactly these shapes.
    model_d: int = 128
    model_n_heads: int = 4
    model_ffn_expansion: int = 3
    n_combat_queries: int = 8
    n_terrain_queries: int = 2
    trunk_n_layers: int = 3
    gru_dim: int = 256
    kind_vocab_size: int = 4096
    kind_embed_dim: int = 32
    movement_n: int = 3
    direction_n: int = 3
    action_n: int = 8
    jump_n: int = 2

    # ---------------------------------------------------------- episodes
    # The knight's max masks per training episode, drawn uniformly from
    # "lo,hi" (evals always play the game's 9/9), and the chance an episode
    # starts below max, at a uniform 1..max. More masks let episodes reach
    # late phases a 9-mask policy rarely sees.
    train_max_health: str = "5,15"
    start_health_low_p: float = 0.25
    # Hard starts (sim_worker.HardStarts): the chance a training episode starts
    # from a saved state 1-2 x hard_start_every steps before a hit the policy
    # took, instead of a fresh fight. Each env keeps its last hard_start_pool
    # such states, each used hard_start_uses times (an NKG checkpoint is 5-9 MB,
    # and each env holds 2 + hard_start_pool of them). 0 = off.
    hard_start_p: float = 0.0
    hard_start_every: int = 25
    hard_start_pool: int = 2
    hard_start_uses: int = 4
    # Search demos at hard starts (sim_worker.HardStarts): the chance a restart
    # searches for a dodge (random held-action shooting, at most
    # hard_start_tries sequences, each action held 2..hard_start_hold_max
    # steps, hitless for the state's age + hard_start_after steps) and, if it
    # finds one, replays it as forced steps the learner imitates. 0 = off.
    hard_start_demo_p: float = 0.0
    hard_start_tries: int = 24
    hard_start_after: int = 25
    hard_start_hold_max: int = 12

    # ------------------------------------------------- reward / curriculum
    # advantage = attack_weight * d_atk / D  +  heal_coef * heal  -  d_def.
    # D (% of boss HP dealt per hit taken; every hit costs 1, whatever its
    # masks) is measured from rollouts, not tuned.
    # attack_weight 1 prices a mask at D, the damage a mask buys on average;
    # 2 priced it at half that. On NKG (2026-09-22, same sim and recipe) 2.0
    # stalled at 39% landed / 0% kills by 44M steps; 1.0 reached 63% / 5% by
    # 44M and 72% / 23% by 103M (D 14.9). Fixed at 1: one less knob.
    attack_weight: float = 1.0
    heal_coef: float = 1.0
    D_min: float = 0.01
    D_initial: float = 2.0
    D_max_delta: float = 0.40     # max relative change per 8192-step epoch
    D_event_window: int = 32      # size the lookback by hits, not epochs
    D_event_max_epochs: int = 400  # the lookback's cap
    D_event_min_landed: float = 100.0
    D_event_min_epochs: int = 60

    # --------------------------------------------------------------- PPO
    lr: float = 1.5e-4
    # A ~4 s horizon (1 / (1 - gamma) steps of 0.02 s): an NKG attack cycle is
    # ~5 s, and the gap between chances to land damage has a 4 s p90.
    gamma: float = 0.995
    gae_lambda: float = 0.975
    clip_eps: float = 0.15
    value_coeff: float = 0.5
    def_value_coeff: float = 0.15
    entropy_coeff: float = 0.002
    max_grad_norm: float = 0.5
    # Truncated BPTT chunk, ~1.9 s: a spike is telegraphed only by how long
    # the boss has been gone, which a short window cannot learn.
    seq_len: int = 96
    # The update is launch-bound: up to 32 chunks per minibatch cost the same
    # per optimizer step, so 32 is the knee.
    chunks_per_batch: int = 32
    train_iters: int = 2
    # Env steps per epoch, rounded up to whole seq_len chunks per env. The D
    # knobs are rescaled from 8192 to what an epoch actually holds.
    total_steps_per_epoch: int = 8192
    value_var_ema: float = 0.9    # per-boss return-variance EMA (value-loss scale)

    # ------------------------------------------------------ rollout queue
    # Run a GPU step once this many envs are ready (0 = all of them)...
    queue_batch: int = 0
    # ...or once the oldest ready env has waited this long.
    queue_timeout_us: int = 1000
    # GPU segment stores. An env may run ahead of the slowest env until it
    # would enter a store the learner still holds; more stores is more
    # policy lag, not much more throughput.
    queue_stores: int = 2
    # Parts a worker's envs are split into per dispatch, each published on its
    # own, so the worker steps its next part while the server turns one around.
    queue_split: int = 2

    # ---------------------------------------------------------- training
    total_env_steps: int = 0      # 0 = run until killed
    total_epochs: int = 0         # 0 = bounded by total_env_steps only
    save_every_steps: int = 1_024_000
    keep_last_n: int = 2
    save_path: str = "runs/hkrl"
    # wandb never takes the run down: init failure degrades to disabled and
    # every log() is wrapped.
    wandb: bool = True
    wandb_project: str = "fullknight"
    wandb_name: str = ""
    seed: int = 0                 # 0 = clock-seeded

    # -------------------------------------------------------------- eval
    # Every N epochs: the sim greedy eval (argmax, frozen normalizers,
    # eval_episodes_per_env x n_envs episodes within eval_max_steps steps) and
    # the real-game eval, ~15% of the run's wall time. Rule: an eval every
    # ~39M simulated frames (eval_every_epochs x env steps per epoch x
    # frames_per_wait: 3200 x 12288 x 1, sized as 2400 x 8192 x 2 at fpw 2),
    # and eval_max_steps x frames_per_wait = 8000 frames.
    eval_every_epochs: int = 3200
    eval_episodes_per_env: int = 3
    eval_max_steps: int = 8000

    # --------------------------------------------------- real-game eval
    # Greedy episodes against the real game (the HKOracle install) at every
    # eval point, next to the sim eval of the same weights (game_eval.py).
    # The fleet exists only while an eval runs. If it cannot be built the run
    # says so and trains without it.
    game_eval: bool = True
    game_eval_episodes: int = 16      # started (and finished) per eval
    game_eval_levels: str = ""        # round-robin per episode; "" = boss_levels
    game_eval_max_s: float = 900.0    # cap on the episode phase; cut episodes are dropped
    game_n_envs: int = 8
    game_path: str = os.environ.get("HKRL_GAME") or os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "game")   # hkpy/paths.py GAME

    @property
    def boss_levels_list(self) -> list:
        return [s.strip() for s in self.boss_levels.split(",") if s.strip()]

    @classmethod
    def from_cli(cls, argv=None) -> "Config":
        p = argparse.ArgumentParser()
        for f in fields(cls):
            if f.type is bool or f.type == "bool":
                p.add_argument(f"--{f.name}", action="store_true", default=None)
                p.add_argument(f"--no-{f.name}", dest=f.name, action="store_false")
            else:
                p.add_argument(f"--{f.name}", type=eval(f.type) if isinstance(f.type, str) else f.type,
                               default=None)
        args = p.parse_args(argv)
        return cls(**{k: v for k, v in vars(args).items() if v is not None})
