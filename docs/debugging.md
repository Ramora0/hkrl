# Debugging divergence

Methods that found real defects, and the traps that hid them. Read this before debugging a boss.

## Tools

Every gate replays a corpus the same way (`hkpy/sim_driver.py replay_episode`): the recorded seed, clocks
and RNG draws, the corpus's own `frames_per_wait` (fpw 1 policy recordings with hold actions included), and
the one configuration (`hkpy/sim_config.py`, which the trainer and every random-play soak apply too). Every
gate first checks the corpus's provenance (`hkpy/provenance.py`) and refuses a corpus recorded against
another dump or mod, or whose trace header lies outside regime R2. A corpus recorded in anything but the one
configuration (no stamp, a trace header from a mod with the retired opt-in switches, or a stamp of another
configuration) is measured and labelled LEGACY with the reason.

- `python gate/ledger.py <corpus dirs>`: the event ledger, per step: `HERO_DAMAGE` (source, hp after,
  whether it took health), hazard respawns, `ENEMY_DAMAGE`, boss FSM transitions, combat rows appearing and
  disappearing. It finds the sync horizon (the first step whose hero or boss state differs) and lists every
  ledger mismatch up to it (missing, extra, shifted by d steps, same events in another order), ranked by
  source; past it, per-source event rates. A trace's last step ends at its OBS (what follows is the next step's
  first fixed tick), so the frames a game recording runs past the corpus's last step are no step's, the
  last step both sides ran is compared up to its OBS, and a step the sim trapped inside is never compared
  (INCOMPLETE).
  This is the replay-and-diff method that found B2-B7, as a gate.
- `python gate/control.py <recordings A> <recordings B>`: the game against itself, two recordings of one
  corpus (`tools/rerecord.py` twice), with the same observation and ledger instruments. Read every gate
  number next to it.
- `python gate/boss_gate.py <SCENE>`: one verdict line for a boss on its policy corpus (`provenance.play`: the
  stamp's `play`, or for a legacy corpus its recording mode and action head; `--allow-nonpolicy` measures a
  scripted one as a diagnostic, never SHIP). `gate/sweep.sh` runs it for every scene with a corpus.
- `python gate/parity_battery.py --corpus-dir analysis/polbat_<SCENE>`: first divergence per episode on
  three columns: A (hitbox set), B (FSM states and flags) and C (positions), next to the ledger's sync
  horizon (L). It reports the channel and the step where each first differs, and an episode the sim trapped in.
- `python tools/attack_gap.py GAME SIM`: hits per episode per attack for the same checkpoint in the game
  and the sim, sorted by the gap, with one attribution (`attribute`) that both evals call.
- `hkpy/obs_parity.py` compares two traces field by field. `hkpy/hktrace.py` reads either side's trace.
- `python gate/combat_eval.py`: whether the combat loop matches (damage ledger, hits, deaths), regardless of
  bit parity.
- RNG: replay a corpus under the game's recorded draws. `python gate/build_oracle.py` builds the table
  from `.rngdraws.jsonl`; `HKSIM_RNG_ORACLE=<table>` installs it in `sim_driver`. Build the table from the
  corpus you are replaying. A table built from other corpora installs nothing and makes every boss look
  catastrophic.

## Finding where

- **XOR the state words.** Same frame, same position, same clip, but `cstate` differs by one bit
  (`recoilingRight`). One bit localises a defect faster than any amount of reading.
- **Compare damage sources, not row counts.** Some hazards never become combat rows. `n_combat` can match
  exactly while a spike floor deals damage on one side only. If `hp` or `hits_taken` diverges, break
  `HERO_DAMAGE`/`ENEMY_DAMAGE` down by source first.
- **Check that the FSM ticked, not that the object exists.** An object can fail three ways: never created,
  created but never activated, or spawned but inert. Only the first two drop a row. The third shows a
  correct row with no behaviour.
- **Instrument a throwaway build.** Use a separate build directory, take the reading, then revert and
  confirm the tree is clean. A measurement that kills a hypothesis must leave nothing behind that could be
  mistaken for a fix.

## Knowing whether you learned anything

- **Can the check fail?** A query that returns the same answer on both sides whatever the truth, such as
  searching rows for a name the observer never emits, is worse than no check.
- **Which question did the check answer?** "Is this difference mine?" and "What is this difference?" are
  different questions. Don't use a check's result outside its domain.
- **Confirming the arithmetic says nothing about the code.** Proving that `Wait(1.0s)` needs 51 ticks
  doesn't show that the sim computes 51.
- **A check that can't discriminate also suppresses good fixes.** If `HeroController` re-sets the velocity
  every FixedUpdate, `dx = v*dt` whether or not a remainder is integrated. That evidence then fits both
  hypotheses. A wrongly rejected fix fails silently.
- **Predict before you measure.** Write down what the result would look like if you were wrong. A mechanism
  that names a number in advance is stronger than one that explains it afterwards.
- **Draw-derived quantities are episode-specific.** Compare on the same episode. Corpora are named alike,
  and a cross-episode mismatch looks like a finding.
- **Verify the mechanism when the number improves, too.** A variant that scores better while its mechanism
  is wrong in a new way is the hardest regression to catch. Check each variant's mechanism, not the
  aggregate.
- **One knob, opposite signs, worst on the boss that exercises it most, means a wrong model cancelling
  locally.** A change that truncates a boss's best episodes while its median barely moves has not fixed it.
- **A number inside a band is not evidence of correctness.** It only shows the gate will not stop you.

## When everything measures correct and the symptom persists

- **Suspect a cancelling pair.** Two errors of equal size and opposite sign each measure exact in isolation.
  Fixing one alone makes the symptom worse.
- **Suspect the units.** A counter that alternates +1/+2 from opposite starting parity reports 77 vs 75 for
  the same 51 ticks.
- **Distrust an analogy to a fix that worked.** A change that helps your boss and leaves the reference boss
  passing, because two errors now cancel, is the hardest kind of wrong change to catch later.

## Tooling traps

- **`git status` cannot tell content from line endings.** Use `git diff --numstat`: if it is empty, a
  generated file is current.
- **Committed generated files are never regenerated by the build.** CMake runs only `sim/gen_registries.py`.
  `gate/tables_fresh.py` (or `python tools/check.py --full`) and `gate/inputs_fresh.py` catch stale tables.
- **A stale DLL survives a failed link.** Delete `hksim.dll` and relink before trusting a gate number.
- **Read every gate number next to the game-vs-game control** (`gate/control.py`) for the same episodes.
  Some arenas do not reproduce their own recordings (`docs/roadmap.md`).
- **A first-divergence number compares the common prefix only.** An episode the sim trapped in reads as a
  short fight unless the gate says it trapped (`parity_battery.py` and `ledger.py` print the trap).

## Documentation rules

- When you document a limitation, find every place that depends on it. A workaround at one call site means
  the other call sites are broken.
- A general principle stated at one call site is not a general fix. Apply it everywhere it holds, or it
  will be rediscovered.
