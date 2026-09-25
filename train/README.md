# train

PPO for Hollow Knight boss fights, trained against the simulator in `sim/`
(through `hkpy/sim_driver.py`) and evaluated against the real game. One
recipe: the defaults in `config.py` are what a run uses.

## Train

Build the sim once, then run from the repo root:

    cmake -S sim -B sim/build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DHKSIM_MODULES=core;hero;phys;fsm;obs"
    cmake --build sim/build
    python train/train.py --boss_levels GG_Grimm_Nightmare --save_path runs/nkg/nkg

`python` is the FullKnight venv's (`FullKnight/.venv/Scripts/python.exe`: torch,
CUDA, wandb, websockets, psutil). Every config field is a `--flag`
(`--no-flag` for booleans). The run needs CUDA; the fused kernels build on
first use into `train/kernels/_build*`. `$HKSIM_DLL` overrides the DLL.

**Never resume a run; always start fresh.** There is no `--resume`: a
checkpoint (`<save_path>_<env_steps>.pth`, the last `keep_last_n` kept, and
`_final.pth`) holds the weights, the normalizers and the vocab id space, for
evaluation only.

A run goes until `--total_env_steps` / `--total_epochs` (0 = until killed).
Ctrl-C finishes the epoch, saves `_final.pth` and reaps the sim workers; a
second Ctrl-C aborts. wandb (project `fullknight`) is on unless `--no-wandb`,
and can never take the run down.

### Hard starts

`--hard_start_p p` (default 0 = off): at an episode end, with probability p
the env restores a saved state from 1-2 x `hard_start_every` steps before a
hit the policy took, instead of starting a fresh fight
(`sim_worker.HardStarts`, on the sim's checkpoint API). The policy replays the
attacks that hit it. Evals never use them, but a run's per-epoch landed/hits
and episode counts include the restarted episodes, so compare runs on the
evals. `python tools/dodge_search.py CKPT` checks, per attack, whether the
hits a checkpoint takes could have been dodged: it random-shoots held-action
sequences from a state before each hit.

`--hard_start_demo_p q` (default 0 = off) adds search demos. Each banked
state keeps the actions the env took from it through the hit; at a restart the
worker replays them with one window swapped for a random held action (plain
CPU search, at most `hard_start_tries` tries) until a variant dodges, and with
probability q replays that variant as the episode's first actions. The
learner trains on those forced steps at ratio 1 with the advantage floored
at 0; outside the window a demo is the policy's own actions, so the push lands
on the deviation that dodged. D is measured on fresh episodes only.

## Evals

Every `eval_every_epochs`, training pauses for a greedy eval (argmax, frozen
normalizers) in the sim and in the real game, same weights, and logs both
plus the game-minus-sim gap (`eval/*`, `game/*`, `transfer/*`). Every eval is
also a line in `<save_path>_evals.jsonl`, with every sim and game episode;
each episode's `hit_by` lists its hits as [step, kind, clip, gap, boss clip,
source], attributed by one definition on both sides
(`tools/attack_gap.py attribute`): the damaging row nearest the knight after
the step (a projectile that vanishes on contact leaves ""; a hit that ends
the episode is attributed from the empty terminal observation). The eval
prints the per-attack game-minus-sim gap in hits per episode;
`python tools/attack_gap.py <evals.jsonl> <evals.jsonl>` prints it for a
saved eval. The configuration both sides play in is `hkpy/sim_config.py`.

The real game is `game_n_envs` headless instances of the HKOracle install at
`--game_path` (default `$HKRL_GAME`, else `<repo>/game`), launched for each eval and killed after.
If the install is missing the run says so and trains without it
(`--no-game_eval` turns it off). A checkpoint on its own:

    python train/game_eval.py --ckpt runs/nkg/nkg_final.pth --levels GG_Grimm_Nightmare
    python train/game_eval.py --ckpt runs/nkg/nkg_final.pth --levels GG_Grimm_Nightmare --episodes 1 --watch   # one window, real speed

Do not run other training on the machine during a game eval: under load the
game's scene transitions hang.

## Tests

    python -m pytest tests/train           # ~20 s: kernels, PPO update, prep, queue, pool, mirror, game client
    python tests/train/smoke.py            # ~10 s: the recipe end to end on the real sim
    python tests/train/test_hitless.py     # the hitless objective's GAE and soft reward

Each test file also runs as a script. They need CUDA and the built sim DLL.

## Layout

| file | what |
|---|---|
| `config.py` | every knob, one dataclass, `--flag` for each; the defaults are the recipe |
| `train.py` | the run: rollout queue -> async learner, the adaptive-difficulty curriculum (D), evals, logging, checkpoints |
| `train_hitless.py` | the same run on the hitless objective (below): its own PPO subclass and loop, everything else shared |
| `train_discover.py`, `flow.py` | discovery on one fixed-seed fight, onezero-style: the sampler fit by trajectory balance on whole walks (no PPO), lines end at the first hit, restarts from the best lines |
| `rollout.py` | the actor (one CUDA graph per batch size: preprocessing kernels over the page-locked sim buffers, the policy, the readback) and the queue that forwards whichever envs are ready and fills per-env store segments |
| `ppo.py` | normalizers, decomposed GAE, the PPO update as one CUDA graph per minibatch, the learner thread, checkpoints |
| `model.py` | the policy network (PyTorch reference) and the switch to its fused kernels |
| `hkkern.py`, `kernels/` | the fused CUDA kernels: the network's forward/backward and the actor's preprocessing |
| `observation.py` | column indices, the `Observation` bundle, the view gate, mirror augmentation |
| `sim_env.py` | the sim pool: worker processes over one shared-memory block; host preprocessing for the evals |
| `sim_worker.py` | the worker process (torch-free), episode starts, the shared vocab id space |
| `game_eval.py`, `game_client.py` | the real-game eval fleet, and the oracle mod's wire protocol and instances |

## The objective

    advantage_t = attack_weight * delta_atk_t / D  +  heal_coef * heal_t  -  delta_def_t

`delta_atk` uses `damage_landed` (% of boss max HP), `delta_def` uses
whether the knight was hit that step: every hit costs 1, however many masks
it takes, so an edge-pit touch (1 mask, then a hazard respawn with i-frames)
is no cheaper than a 2-mask attack. `D` (% of boss HP dealt per hit taken) is
measured from rollouts, per boss, not tuned. Evals and logs still report
masks. No terminal win/loss bonus: under
discounting, idling beat dying. The sim ends an episode where the game does
(knight death, or the boss scene's OnBossesDead).

### The hitless objective (`train_hitless.py`)

    r'_t = dmg_t * (1 - hit_t * done_t)  +  alpha * (-log pi(a_t|s_t) - H_target)

The return is the boss damage landed before the knight dies (or the boss
does), undiscounted (`gamma` 1); damage on the killing step is a trade and
does not count. The knight's masks are the hit budget: training episodes draw
max masks from `train_max_health` (1..9), so a 1-2 mask episode is the
hitless objective itself and a fuller one rewards every dodge that keeps
masks for later; the policy sees its masks, so the return is Markov. One
critic (the attack head); no D (reported, not used), mask price or heal term.
The entropy is in the reward (maximum-entropy RL), with `alpha` the
multiplier of E[H] >= `target_entropy`, stepped every epoch; subtracting the
target keeps staying alive from earning an entropy stream. `log pi` is the
joint log-prob of the heads the agent chose (the action head is out on a
hard-commit step).

## Invariants

* **Sim errors are traps.** A non-zero hksim return means the sim refused to
  invent an answer; the worker raises and the run stops. Never reset past it.
* **One thread per process.** hksim holds process-global scratch, so each
  worker steps its instances on one thread; more cores means more processes.
* **One vocab id space.** hksim assigns kind ids in arrival order, so
  workers would disagree about ids. The trainer owns the canonical list; a
  worker that meets a new string reports it and re-packs with the canonical
  ids before publishing (`sim_worker.py`).
