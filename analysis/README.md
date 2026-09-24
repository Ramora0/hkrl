# analysis/: the evidence tree

Nothing here is authored. It is decompiled, dumped or recorded, and everything in `sim/` cites it.
`authorities.md` says which source settles which kind of question. `analysis/` is a directory junction
that git does not track: never `git add` under it. `gate/inputs_fresh.py`, run on every build, catches a
dump that changed under a committed table.

## Scene data

| path | what | produced by | read by |
|---|---|---|---|
| `dumps/<scene>/` | runtime dumps of each ported scene: `scene.json`, `physics.json`, `hierarchy.json.gz`, `bosses.json`, `hero.json`, `playerdata.json`, `globals.json`, `language.json`, `sprites.json`, `rng_probe.json`, `meta.json` (tier, provenance) | `tools/dump_all.py` (oracle dump mode) | `sim/core/gen_scene.py`, `sim/fsm/gen/gen_tables.py`, `sim/hero/gen_fields.py`, `gate/inputs_fresh.py`, `tests/test_hero.py`, `test_phys.py`, `test_rng.py` |
| `fsm/<scene>.json` | PlayMaker dump of each ported scene: states, transitions, actions, variables; a dump from the current mod also holds the loaded prefab assets' FSMs (`asset`: true, counted apart), which the readers skip | same run (`oracle/Dump/FsmDumper.cs`) | `gen_tables.py`, `gate/build_oracle.py` |
| `dumps_all/<scene>/` | the same dump (with `fsm.json`) for every other arena, at the tier its statue loads first. `_roster/boss_roster.json` lists all 135 (scene, tier) arenas | `tools/dump_all.py` | `sim/hero/gen_fields.py`; porting new bosses |
| `dumps_t1/`, `dumps_t2/` | the statue scenes loaded at BossLevel 1 / 2 (Ascended / Radiant); level keys `<scene>@T1` / `@T2` | `tools/dump_all.py <list> <out> 1\|2` | `gen_scene.py`, `gen_tables.py`, `gen_fields.py` |

Dumps are regime-sensitive. Take them with the oracle's defaults (R2, `docs/frame-order.md`), or boss start
poses shift by whole frames.

## Asset data (`assets/`)

The game's serialized asset files, read by `tools/extract_assets.py` (UnityPy; MonoBehaviour type trees generated
from `oracle_Data/Managed` with the vanilla `Assembly-CSharp.dll.v`; nothing executed). This is what the game
Instantiates: the prefabs and the scene files as authored, before any runtime change. The runtime dumps above are
the state at SceneReady. `sim/fsm/gen/prefabs.py` (gen_tables.py) builds every prefab instance from it. Re-run the
tool after a game update.

| path | what |
|---|---|
| `assets/index.json` | `physics2d`: the project's Physics2DSettings (gravity, iterations, default material, layer matrix); per scene: level file, counts, prefab keys it references, `pools` (every `ObjectPool` / `PersonalObjectPool` startup-pool entry: owner path, prefab ref, size); per prefab: root id, name, counts, `referenced_by` (scenes, `ddol`, other prefabs); `ddol`: the Knight, `_GameManager` and `_GameCameras` prefabs |
| `assets/scenes/<scene>/` | every object of the scene's level file |
| `assets/prefabs/<name>@<file>-<pathID>/` | one prefab: every prefab that the scenes, the DDOL roots or another extracted prefab reference (the closure) |
| `assets/ddol/` | the three DontDestroyOnLoad roots |

Each directory holds three gzipped JSON files:

- `objects.json.gz`: `objects`, one record per GameObject in depth-first sibling order (`Transform.m_Children`):
  `id` (`<file>:<pathID>`), `path` (from its root), `name`, `parent`, `children`, `active` (`m_IsActive`),
  `layer`, `tag`, `localPosition`, `localRotation` (quaternion), `localScale`, and `components` in
  `GameObject.m_Component` order (the order `GetComponents` returns). A component is `id`, `class` (Unity class),
  `script` and `assembly` for a MonoBehaviour, and `data`: every serialized field. Object references are
  `{"$ref": id, "type", "name", "path"}`. A PlayMakerFSM's `data.fsm` points to `fsms.json.gz`.
  `physicsMaterials`: every PhysicsMaterial2D a Collider2D or Rigidbody2D there names, with its friction and
  bounciness.
- `fsms.json.gz`: every PlayMakerFSM decoded from its ActionData into the `fsm/<scene>.json` schema (`path`,
  `fsmName`, `states` with `transitions` and `actions` with typed `fields`, `variables`, `events`,
  `globalTransitions`), plus `component`, `template`, `dataVersion`, `startState`, `restartOnEnable`,
  `handle2d`. A template FSM is the template's body with the component's name and variable overrides
  (`PlayMakerFSM.InitTemplate`). Values are serialized values, not SceneReady values. A field's declared C# type
  comes from the dumps' action field types (`fsm/*.json`, `dumps_all/*/fsm.json`); a field no dump has falls
  back to its ParamDataType.
- `animation.json.gz`: `tk2d_libraries` (every tk2dSpriteAnimator library in the dumps' `library` form: clips, frames
  naming their sprite collection) and `tk2d_collections` (those collections' sprites with their collider fields, the
  `sprites.json` form); `animators` (object, controller, enabled), `controllers` (clip ids, the name table `tos`
  and the compiled state machine `stateMachine` verbatim) and `clips`. A clip has `curves`, each with `path`
  (relative to the Animator), `class`, `attribute` (for example `m_Enabled`, `m_IsActive`, `m_LocalPosition.x`;
  an unknown hash stays a number) and one of `streamed` (keys `{t, coeff}`: the Hermite segment starting at `t`,
  `((a*dt + b)*dt + c)*dt + d`), `dense` (`begin`, `rate`, `values`) or `constant`. Clips also have `events`,
  `start`/`stop`, `sampleRate`, `loopTime`.

`python tools/extract_assets.py --check` compares the extraction with the dumps, one line per check: scene
FSMs against `fsm/<scene>.json`, pooled clones' FSMs and subtrees against their prefab, scene objects against
`hierarchy.json.gz`, collider geometry against `scene.json`, and startup-pool sizes against dumped clone
counts. A difference is either a runtime write the dump saw (a variable's SceneReady value, a moved object, a
component added at runtime such as `PlayMakerUnity2DProxy`) or a finding.

## Recordings

Each corpus is `<name>.corpus.json` (the action script, `docs/trace-format.md`) plus the game's recording,
`<name>.a.hktrace` and `<name>.a.rngdraws.jsonl`. Re-record with `tools/rerecord.py`.

| path | what | read by |
|---|---|---|
| `polbat_<SCENE>/` | policy-driven fights per boss, 8–12 episodes, fpw 2: the gate's corpora. `polbat_hornet` and `polbat_gruz` are GG_Hornet_1's and GG_Gruz_Mother's | `gate/boss_gate.py`, `gate/sweep.sh`, `gate/parity_battery.py`, `tools/lifecycle_compare.py` |
| `traces/p0/` | scripted reference recordings (`r2_*`, `canon_*`, `base_seed`), with `.b` game-vs-game repeats. Protected: never overwrite | `tests/test_fsm.py`, `tests/test_hero.py`, `gate/combat_eval.py` |
| `traces/logs/` | mod and player logs of oracle runs | written by `tools/run_oracle.py` |
| `rngdraws/` | re-recorded draw logs for the `traces/p0` `r2_*` corpora | `gate/build_oracle.py` (default `--draws`) |
| `control/<build>/<corpus>/{a,b}/` | the game-vs-game control: the first 4 episodes of every `polbat_<corpus>`, re-recorded twice in script mode with `HK_ORACLE_LIFECYCLE=1` (`.hktrace`, `.rngdraws.jsonl`, `.fsmticks.jsonl`, `.lifecycle.gz`). `<build>` names the mod: `main` (7ca54fb), `fix1` (+ interpolation held off), `fix2` (+ `RegimeClock`), `fix3` (ab62cc6, + wall-clock load timeouts), `fix4` (+ root/env's step and IL pins, `isPlaying` at every call site); `_aborted_*` are interrupted runs. `ab.json` / `ao.json`: `tools/control_diff.py` a~b and a~original; `logs/<name>.mod.log` the mod log (from fix4). `CONTROL.md`: the table and its causes | read every gate number next to it |
| `method_oracle/<scene>/` | the method oracle's recordings (`HK_ORACLE_METHODS=1`): per scene 4 policy corpora from `polbat_*` and 2 perturbed ones (`pert<k>_*.corpus.json`, 25% of steps random), each with `.a.hktrace`, `.a.methods.jsonl.gz`, `.a.rngdraws.jsonl`, `.a.fsmticks.jsonl`; `_inertness/` holds the same corpora recorded without the recorder. A junction to the shared store like the others | `tools/method_oracle.py`, `tests/test_method_oracle.py`; spec in `docs/method-oracle.md` |
| `lifecycle/<scene>/` | `.lifecycle.gz` recordings (with their `.hktrace`) for five scenes; `rules.json`, `inertness*.json`, `_inertness/` | `tools/lifecycle_rules.py`, `tools/lifecycle_compare.py`; spec in `docs/engine-lifecycle.md` |
| `conformance/<run>/` | engine conformance probes run in the game (oracle probe mode, GG_Hornet_1): `spec.json` (the scenarios as run), `rep<i>.json` (one game process each: per scenario its spec, instance ids and the callback log `[frame, stage, label, callback, other, x]`, plus `obs` samples), `logs/`. `2026-09-23-drain` is the run the tests check (101 scenarios, the physics ones with a broadphase drain); `2026-09-23` is the 93-scenario run before drains; `run1`–`run3` are earlier runs of fewer scenarios (`run1` with the boss active) that document the non-reproducible cases (`run2`: `f_two_receivers` flips in one process); `drain_contrast` runs the same-step pair and receiver-order cases with and without a drain (2 processes, `make_spec.py` wrote its spec); `smoke1` is a five-scenario smoke run | `tools/conformance.py`, `tests/test_conformance.py`; spec in `docs/engine-lifecycle.md` §3 |

## Sources and specs

| path | what |
|---|---|
| `decomp/` | ILSpy output of Assembly-CSharp, Assembly-CSharp-firstpass and PlayMaker. Cited by `file:line` throughout `sim/` and `oracle/` |
| `upstream/` | UnityCsReference at tag `2020.2.2f1`, and Box2D (`box2d-v2.3.1`, `box2d-v2.4.1` worktrees). Re-clone per `authorities.md` |
| `specs/*.md` | one cited index per subsystem (hero motion, physics, FSM runtime, RNG, tk2d, damage path). `obs-wire.md` is the observation contract, and `sim/obs` and `train/observation.py` follow it |
| `open-questions.md` | questions not yet settled by evidence, cited by id (`Q-…`) from code |
| `authorities.md` | which source settles which kind of question |
