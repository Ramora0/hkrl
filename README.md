# hkrl

Reinforcement learning on Hollow Knight boss fights. The goal is one policy that beats every Godhome boss.
The repo has three parts:

- **hksim**: a C port of the boss fights, built from the game's decompiled code and runtime dumps. It is
  seeded, deterministic, and fast enough to train in.
- **A PPO trainer** that trains against hksim and evaluates in the real game.
- **HKOracle**: a game mod that records ground truth for the port (traces, RNG draws, lifecycle callbacks,
  scene dumps) and runs the real-game evals.

## Layout

| dir | what |
|---|---|
| `sim/` | the C simulator: `core/` (ABI, frame scheduler, trace writer), `hero/`, `phys/`, `obs/`, and `fsm/`: the PlayMaker runtime and lifecycle, `actions/` (ported actions by category), `components/`, `bosses/`, and generated `tables_*`/`scene_*` |
| `hkpy/` | Python binding and trace tools: `sim_driver`, `sim_config` (the one configuration every caller applies), `provenance` (corpus stamps), `ledger`, `hktrace`, `obs_codec`, `obs_parity`; the engine conformance scenarios and their sim mirror (`conformance_scenarios`, `conformance`); `staterec` (the state recorder's reader) |
| `gate/` | sim-vs-game parity: `ledger` (event ledgers), `control` (game vs game), `boss_gate`, `parity_battery`, `combat_eval`, `build_oracle`, `sweep.sh`, freshness and duplicate checks |
| `tools/` | `check.py`, `fingerprint.py`, `attack_gap` (per-attack sim/game gap), `iframe_window` (where the i-frame window ends, game vs sim), oracle launch/record/dump (`run_oracle`, `record_corpus`, `rerecord`, `dump_all`), `extract_assets` (the game's scenes and prefabs, read from its asset files), `lifecycle_rules`/`lifecycle_compare`, `conformance` (engine probes in the game), `build_ref`/`sim_equal`/`sim_bench` (speed changes: `docs/sim-speed.md`) |
| `tests/` | pytest for the sim; `tests/train/` for the trainer |
| `train/` | the PPO trainer (`train/README.md`) |
| `oracle/` | the game mod (`docs/oracle.md`) |
| `analysis/` | evidence: dumps, FSM dumps, recorded corpora, decompiled source, specs (`analysis/README.md`); only the docs are in git |
| `docs/` | contracts and working notes (below) |

## Quick start

You need Windows 10/11, an NVIDIA GPU from the RTX 30 series or newer, and
[uv](https://docs.astral.sh/uv/getting-started/installation/). No compiler: the sim, the GPU kernels and the
mod come prebuilt with each release.

    git clone <this repo>
    cd hkrl
    powershell -ExecutionPolicy Bypass -File setup.ps1

`setup.ps1` makes the Python environment (`.venv/`), downloads this commit's release files (the sim DLL,
the GPU kernels, the trained checkpoints in `models/`), and ends with `tools/doctor.py --smoke`: one line
per piece and a 30-second training run. Rerun it any time; finished steps are skipped. Then:

    .venv\Scripts\python train\train.py --boss_levels GG_Hornet_1 --save_path runs/hornet/hornet

### The real game

Training runs in the sim; the real game is for evaluation (every eval of a run plays the game too, when the
game install below exists) and for recording ground truth. You need **Hollow Knight 1.5.78** (the current
Steam version) with the [Modding API](https://github.com/hk-modding/api). Either install the API into your
game with [Scarab](https://github.com/fifty-six/Scarab), or download `ModdingApiWin.zip` from the API's
releases and pass it to setup, which puts it in the copy only and leaves your game vanilla:

    powershell -ExecutionPolicy Bypass -File setup.ps1 -Game [-ModdingApi <ModdingApiWin.zip>] [-GamePath <Hollow Knight folder>]

This builds `game/`, a private copy of the game that runs the HKOracle mod (`tools/make_oracle_install.ps1`).
Your own game, saves and settings are never touched:

- the copy keeps its saves, settings and logs in `%USERPROFILE%\AppData\LocalLow\hkrl oracle\` (and the
  registry key `HKCU\Software\hkrl oracle`), not the game's `Team Cherry` folder;
- it has no `steam_api64.dll`, so it never starts Steam, shows you as playing, or syncs Steam Cloud;
- the files the game writes into its own folder are copies, not links into your Steam install;
- setup backs up your saves to `game\save_backup\` before the copy first exists anyway.

Watch a checkpoint fight in a game window:

    .venv\Scripts\python train\game_eval.py --ckpt models\<checkpoint>.pth --levels GG_Grimm_Nightmare --episodes 1 --watch

Without `--watch`, it runs 8 headless instances and prints the win rate and hits taken.

### Building from source

Only to change the sim, the kernels or the mod. Rebuilding the sim needs gcc (mingw64), cmake and ninja;
the kernels, MSVC 2022 and CUDA 12.8 (they build themselves on first use when no prebuilt one matches the
sources); the mod, the .NET SDK and the game install above (`HKRL_GAME` points the build at another one).
`python` below means `.venv\Scripts\python`.

## Commands

    # build the sim
    cmake -S sim -B sim/build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DHKSIM_MODULES=core;hero;phys;fsm;obs"
    cmake --build sim/build

    # check: build, duplicate actions, tests, scene probe, behaviour fingerprint
    python tools/check.py

    # is everything installed? (--smoke: + a 30 s training run)
    python tools/doctor.py

    # build the mod (into oracle/bin/Release), then put it into game/
    dotnet build oracle/HKOracle.csproj -c Release
    powershell -ExecutionPolicy Bypass -File tools/make_oracle_install.ps1

    # a release: build all three, then pack them and write release.json (tools/package_release.py)
    python train/hkkern.py --prebuild
    python tools/package_release.py --tag <tag> --repo <owner>/<repo> [--model <ckpt.pth>]

    # train
    python train/train.py --boss_levels GG_Grimm_Nightmare --save_path runs/nkg/nkg

    # gate one boss against its recorded game fights, or every boss with a corpus
    python gate/boss_gate.py GG_Hornet_1
    bash gate/sweep.sh <label>

    # the event ledgers of recorded game episodes against their sim replays
    python gate/ledger.py analysis/polbat_GG_Grimm_Nightmare

## Docs

- `docs/principles.md`: what counts as correct, and how changes are checked
- `docs/roadmap.md`: the plan and the open problems
- `docs/sim-api.md`: the C ABI and observation layout the trainer uses
- `docs/engine-lifecycle.md`: Unity/PlayMaker execution order, rules R0–R7, the conformance probes and their findings CF-n, and assumptions A-n
- `docs/frame-order.md`: regime R2 and the shape of an agent step
- `docs/float-parity.md`: build flags and C# float semantics
- `docs/trace-format.md`: the `.hktrace` format and corpus files
- `docs/state-record.md`: the `.hkstate` full-state record and what of the sim's state each field covers
- `docs/lockstep.md`: the per-frame lockstep of the sim against a `.hkstate` recording
- `docs/oracle.md`: the mod: build, install, launch modes, environment variables
- `docs/porting.md`: silent failure modes when porting a boss
- `docs/debugging.md`: methods for localising a divergence
- `docs/method-oracle.md`: checking ported actions and component methods call by call against the game
- `docs/sim-speed.md`: making the sim faster without changing what it computes: the equality and speed tools, and how each mechanism stays exact

## Hollow Knight

Hollow Knight is © Team Cherry. This project is not affiliated with or endorsed by Team Cherry. It contains no
game files: the real-game parts need your own copy of the game. The generated tables in `sim/generated/`
(and the sim built from them) hold values read from the game's data.
