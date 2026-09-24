# Working rules for agents

Read `docs/principles.md` first. `README.md` has the layout and the commands.

## Correctness
- Port, don't fit. Every state, action, constant and rule in `sim/` comes from the decompiled code or the
  dumps in `analysis/`, and cites its source (`file:line`, `analysis/specs/...`). Recordings check the port;
  they never define it. Don't write a Hollow Knight constant from memory.
- An unported action or an undecidable case traps with its name (`HKSIM_UNIMPLEMENTED`). Never guess and
  run, and never reset past a trap.
- Match the game's RNG distribution for each decision, not its number or order of `Random` draws. The gate
  replays recorded draws by (site, occurrence).
- Regime R2 only: capture dt 0.02 = fixed dt, no rigidbody interpolation, seed at scene load.
- Boss-specific behaviour goes in `sim/fsm/bosses/`, not in the generic runtime.

## Checking changes
- `python tools/check.py` must pass before every commit.
- A refactor leaves `tests/fingerprint.json` identical. A behaviour change re-records it in the same commit
  (`python tools/fingerprint.py --out tests/fingerprint.json`) and says so in the commit message.
- Read a gate number next to the game-vs-game control. Compare numbers only when they were measured with the
  same `gate/build_oracle.py`.
- Verify a fix by measuring it, and make sure the check could have failed.

## Training
- Never resume a training run. Start fresh runs. A checkpoint is a policy to evaluate.
- Reward or objective changes need the owner's explicit go-ahead.

## Game instances and the mod
- Never build or deploy the mod while game instances are running.
- After killing a run, kill leftover `ev*.exe` game processes (`python tools/run_oracle.py --kill-all` for
  oracle instances).
- `analysis/` is shared data behind a junction. Don't `git add` under it; only `analysis/README.md`,
  `analysis/authorities.md` and `analysis/specs/` are tracked.

## Style
- Scripts print terse output, one line per stage.
- Don't chain `cd x && cmd`. Use path flags (`cmake -S sim -B sim/build`, `git -C <dir>`, absolute paths).
- One path per job. No unused flags, compatibility shims or code kept "for reference": git history is the
  archive.
- Comments state what the code cannot say for itself: citations and non-obvious reasons. No history, dates,
  measurements or stories; those go in commit messages.
- Docs describe the current state. Update the doc in the same commit as the change it describes.
