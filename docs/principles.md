# Principles

hksim is a port of the parts of Hollow Knight a policy can observe, built so that a policy trained in it
transfers to the real game. It is not a model fitted to recordings.

## What counts as correct
- **Port, don't fit.** Every FSM state, transition, action, and hero/health rule is a port of the dumped
  FSM data (`analysis/fsm`), the scene dumps (`analysis/dumps`) and the decompiled game code
  (`analysis/decomp`). Recordings validate the port; they never define it.
- **Cite evidence.** Every constant and non-obvious rule in `sim/` cites its source
  (`ACT/SetVelocity2d.cs:41`, `HC:1841`, `analysis/specs/...`). A plausible wrong constant is worse than a
  missing one.
- **Unknown traps.** An unported action or an undecidable case traps with its name (`HKSIM_UNIMPLEMENTED`).
  Don't guess and run.
- **A class matters only if it changes gameplay state or the observation.** Matching the game's count or
  order of `Random` draws is not a goal. Each decision must draw from the right distribution; the gate
  replays recorded draws by (site, occurrence).
- **Regime R2** is the target: captureDeltaTime 0.02 = fixedDeltaTime, no rigidbody interpolation,
  `Random.InitState(seed)` at scene load. The oracle mod runs the real game in the same regime.

## How changes are checked
- `python tools/check.py` builds, runs the dup-action check and the tests, and compares
  `tests/fingerprint.json`. A refactor must leave the fingerprint identical. A deliberate behaviour change
  re-records it in the same commit and says so in the commit message.
- `gate/ledger.py`, `gate/boss_gate.py <scene>` / `gate/sweep.sh` measure sim-vs-game parity on recorded
  corpora, replayed in the configuration the game ran in. Always read a gate number next to the
  game-vs-game control (`gate/control.py`), and only compare numbers measured with the same `build_oracle`.

## How the code is kept
- One path per job. No flags nobody turns on, no compatibility shims, no code kept "for reference": git
  history is the archive.
- Comments state what the code cannot: citations and non-obvious reasons. No history, dates, measurements
  or stories. Those belong in commit messages.
- Boss-specific behaviour lives in `sim/fsm/bosses/`, not in generic runtime files.
