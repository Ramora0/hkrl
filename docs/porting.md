# Porting a boss: silent failure modes

The generators handle most of the mechanics of adding a scene. This list covers the failures they don't
catch, where the build succeeds, the scene survives random play, and the boss is still wrong.

The workflow: dump the arena (`tools/dump_all.py`), regenerate its tables (`sim/fsm/gen/gen_tables.py`,
`sim/core/gen_scene.py`, `sim/hero/gen_fields.py`), then build and run `python tools/check.py`. Record a
policy corpus into `analysis/polbat_<SCENE>` and gate it with `python gate/boss_gate.py <SCENE>`.

`gen_tables.py` stops on any component class or PlayMaker action type in the scene, its DontDestroyOnLoad
objects or the prefabs they can spawn that `sim/fsm/gen/completeness.py` does not decide. Port it, or add it to
one of that file's tables with the evidence that it reaches nothing simulated: an exclusion, an `"inert"`
exclusion that the generator checks against each instance's subtree, an unreachable prefab guarded by
PlayerData, or an action type confined to roots that cannot run.

## 1. A flat `median 0 (min 0 max 0)` is structural

Real divergence is noisy. If every episode is pinned at 0, the first observed step already disagrees and
nothing after it is being measured. `gate/parity_battery.py --corpus-dir <dir>` shows which channel is
pinned:

    clip @0   fsm:<boss>/Control @0     -> the boss FSM is in the wrong STATE at step 0
    n_combat @0   fsm:... @50+          -> the FSMs are fine; the ROW SET is wrong (see 8)

Every FSM the dump shows `started` is restored into its dumped state with its live fields
(`world_restore_scene`). The dumped state's OnEnter re-runs, but every FSM's variables are then put back to
the dump's: its stores already happened, against a world that may have changed since (an object re-parented
after a `GetParent`). Only unstarted FSMs cold-start from `startState`. So a wrong state at step 0 means
one of four things:

- an FSM the dump caught unstarted, which cold start does not bring to the game's state;
- a timer partway through, which the restore must hand over;
- a `SNAPSHOT_RULES` override in `gen_tables.py` forcing the wrong value;
- an initial-pose problem between SceneReady and the first recorded frame.

A ported MonoBehaviour (`sim/fsm/components/scripts.c`) restored as Started reruns neither Start nor OnEnable,
so the private fields they set (a timer, a cached reference, a state enum) come from its dumped fields:
`gen_tables.py` `SCRIPTS` carries them in the component's payload and `scr_restore` puts them back. A new port
with private fields carries them the same way and joins `SCRIPT_STATEFUL`, or it silently starts from the field
initialisers. A dumped absolute time (`Time.time + wait`) is carried relative to the dump's `Time.time`.

Compare the dumped `startState` with `activeStateName` in `analysis/fsm/<SCENE>.json`. Add a
`SNAPSHOT_RULES` entry only to force a value the general rule gets wrong, and cite the measurement. The
generators are shared: regenerate every scene and diff the counts before committing a generator change.

## 2. An attack that spawns nothing

Every FSM on an active object runs, so every FSM spawner runs; a spawner at the scene root, outside the boss
subtree, counts too (GG_Hornet_2's `Barb Region | Spawn Barbs` is her only barb spawner). The generator leaves a
pooled prefab's dumped clones out only when nothing that runs spawns it (`gen_tables.py spawned_families`), and that
analysis sees FSM spawn actions and the hero's own spawns (`HERO_SPAWNS`) only. A prefab that only a MonoBehaviour's
code spawns is missed there: port the script, and make its serialized prefab reference reach the tables the way
`EnemyDreamnailReaction.dreamImpactPrefab` does (`Gen.asset_prefab_ref`).

When an attack spawns nothing, read the generator's `pool: N dumped clones of prefabs nothing spawns left out` line
and the prefab's components in `hierarchy.json.gz`. A prefab with `DamageHero` hurts the knight. One with no
Collider2D, Rigidbody2D, PlayMakerFSM or DamageHero is cosmetic. Never judge by the name.

## 3. What the game Instantiates comes from the prefab

Every copy of a prefab is built from the prefab itself, read from the game's asset files
(`tools/extract_assets.py` into `analysis/assets`, `sim/fsm/gen/prefabs.py`). The tables carry each prefab a
running FSM can Instantiate as a template (`go_def.asset`: never active, found or observed), and each pooled clone
the dump holds as a copy of that template with the clone's own dumped state laid over it, object by object. A
clone's parts are bound inside the clone (its Rigidbody2D, colliders, animator, children, FSM references), and a
dumped reference to a clone goes by instance id: nothing about a pooled object is looked up by path, because
every clone of a prefab has the same path. The same holds outside the pool: every dumped object is created under its dumped parent,
read from `hierarchy.json`'s depth-first walk and instance ids, and a dump row naming a component (a rigidbody, a
HealthManager) binds to that component's object, so root-level clones and duplicated scenery that share a path stay
distinct objects.

The pool at scene start is exactly the dumped clones (what the startup pools created). `world_pool_spawn` is
`ObjectPool.Spawn`: the first clone of that prefab under `_GameManager/GlobalPool` goes to the scene root and is
activated; with none left it Instantiates the template (`world_instantiate`). `world_pool_recycle` is
`ObjectPool.Recycle`: a spawned clone goes back to the end of its pool, anything else is destroyed.
`CreateObject` and `SpawnRandomObjects(V2)` Instantiate the template outside the pool (`world_instantiate_at`).
A prefab with no Collider2D, Rigidbody2D, PlayMakerFSM or DamageHero anywhere in it is not copied. A prefab
`sim/fsm/gen/completeness.py` lists as unreachable from the save (`UNREACHABLE_PREFABS`) is a bare stub, and
Instantiating one traps. `hksim_reset` rebuilds the world without the
runtime copies.

## 4. Random play does not reach what a policy reaches

`gate/scenes_probe.py` passing means the scene boots and survives random play. Nail-parry, recoil and
boss-death paths trap only under policy play. Before declaring a boss ported, replay a policy corpus
through the sim (`gate/parity_battery.py`).

When a dispatch traps, check whether the function is already ported in `sim/hero` and only missing its
dispatch entry. When one member of a method family traps, port the whole family in one pass.

## 5. A no-op may be conditioned on the wrong target

Before widening a target-conditioned rule (for example, SpriteFlash methods as no-ops only on the Knight),
grep the decomp for `public void <name>(`. Confirm exactly one declaring class, and read every body.

## 6. Corpus hygiene

- The corpus directory name is load-bearing. `gate/boss_gate.py corpus_dir_for` resolves
  `analysis/polbat_<SCENE>` exactly. Record into the exact directory.
- Validate a recorded trace by parsing it (`hktrace.read_trace(path)`), not by its file size. The frame count
  should track the step count (about 2x at `frames_per_wait` 2).
- Check a corpus's action histogram before trusting it. Policy corpora rarely hold and attack on about 12% of
  steps. Scripted ones do neither.
- `analysis/` is a shared junction. Never `git add` under it.

## 7. The unported-action sweep misses unhandled arguments

Diffing a scene's live action types against the registered vtables finds unported actions. It does not find
an unhandled argument to a ported action. `SendMessage` (actions/hk.c, then `knight_send_message`) and
`CallMethodProper` (the HeroController / GameManager / CameraTarget branches in actions/knight.c) dispatch
on strings. Extract `functionCall.FunctionName` and the `behaviour`/`methodName` pairs from the FSMs that run and
compare them against the `strcmp` names. Boss-death paths are the blind spot. Before porting a traps-on-name
method, read its body: `GameManager.StoryRecord_*` is 24 empty methods, so the port is a cited no-op.

## 8. Row-set defects

If the FSM channel is healthy for tens of steps but `n_combat` is pinned at 0, diff the row kinds directly.
Decode the first OBS record of the game trace with `hkpy/obs_codec.py` and compare its `kinds` with the
sim's. Check both directions across the whole episode: a hitbox only one side emits is as wrong as a
missing one. Both observers classify every live collider by the same predicate (`docs/sim-api.md`), so a
row-set difference is an object one side did not create, activate or place, not a registration gap.

## 9. Ticking does not wake a dormant object

Every FSM and ported component on an active GameObject ticks; one on an inactive GameObject does not. For a
dormant hazard family, confirm that the activator runs and that its `Find*` calls resolve. A `FindChild` returning -1 makes every
downstream `ActivateGameObject` a silent no-op. Then check that the fix did something: count the entity's
rows in the sim observation and in the recorded trace, and require the counts to move. Make sure the query
can fail. Rows are named after the observed piece (`2`, `3`, …), not the collider child (`Box Small`), so
a query for `Box` returns 0 on both sides.
