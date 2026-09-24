# Per-frame lockstep

The acceptance test of the sim against the real game, one frame at a time. For every frame f of a full-state
recording (`.hkstate`, `docs/state-record.md`): overwrite the sim's state with the game's frame f (import), step
exactly one frame of the kind the recording shows, with the recorded action, read the sim's state back (export) and
compare it field by field with the game's frame f+1. Floats compare by bit pattern. Every divergence the harness
reports is one frame's, in one field. The next frame starts again from the game's state, so an error does not
spread into later frames.

    python -m hkpy.lockstep <rec.hkstate> [--corpus <corpus.json>] [--frames N] [--out report.json] [--top N]
    python -m hkpy.lockstep --merge <report.json>...      # the ranked table over several recordings

The actions come from `--corpus`, from a `<name>.corpus.json` next to the recording or one directory up
(`corpora_v2/<scene>/state/` keeps a scene's full-state episodes next to its corpus), or from the STEP events of the
recording's `.hktrace`.

## Parts

- `sim/core/lockstep_api.c` (`hkls_*`, internal: `sim/core/lockstep.h`): the sim's state as a table of entries. An
  entry is one field of one object, named the way the state record names it: the owning GameObject, a component
  label and the recorded field name. Values cross as int64: a float as its bits, an int or bool as itself, an FSM
  state or tk2d clip as its index, a string as the world's string id, a GameObject reference as its index. Export
  reads every entry. Import writes through the setters the ported code uses (SetActive, transform and Rigidbody2D
  writes, collider enable), so derived state follows. Fields no setter reaches are written in place. Entries marked
  export-only (world pose `wp lossy euler`, `activeInHierarchy`, `exists`, `hasBody`, `Rigidbody2D.rotation`,
  `tk2d.currentFrame tk2d.playing`, the proxy lists, the contact structures) are compared, never written.
- `hksim_frame(s, kind, action)` (`sim/core/sim.c`) steps one frame of a recorded kind. `hksim_step` is built from
  the same pieces.
- `hkpy/lockstep.py`: pairing, import, stepping, comparison and the report.

## Frame kinds

From the FRAME record (live flag, `Env.step`) and `Time.timeScale` at the frame's end:

| kind | recording | the sim runs |
|---|---|---|
| `step` | frozen, `Env.step` advanced | the step's frozen frame: the action applied, then the frozen LateUpdate |
| `live` | live, timeScale 1 at its end | one live frame |
| `live_last` | live, timeScale 0 at its end | the step's last live frame: reward and observation, then `FsmPauseGate` closes |
| `frozen` | frozen, same step | a frozen frame with no step |

The recording decides where a step's live frames end, so `hksim_frame` never ends a step on the sim's own
episode-end test.

## Identity

GameObjects pair by (path at birth, clone index): the sim's path and its occurrence count among the sim's objects
with that path, against the recorded `path`/`cloneIndex` (`DDOL/` roots accepted). Prefab assets are not in the
record. Components pair inside their GameObject: `Rigidbody2D` by class, colliders by type and their order among
that type (`m_Component` index), FSMs by `fsm.name` and their order among the object's FSMs of that name, states by
index (checked by name), actions by index, `FsmVariables` through `fsm.vars`. Globals pair by class: `Time`, `Env`,
`Random`, `HeroController`, its `cState`, `plain:PlayerData`, `PlayMakerGlobals.vars`. The hero's `NailSlash` state
pairs with the `NailSlash` component of `Knight/Attacks/<slash>`. Collider identities inside
the contact structures are `<sim go>/<Type>#<ord>`, and recorded collider instance ids are mapped to the same form.
Pairing is redone for the objects that a frame's births and deaths touch, and for new sim objects after a step.
An entry with no recorded counterpart is not compared. The report counts the paired entries.

## Import

1. **Re-entry.** For every FSM whose recorded active state differs from the sim's, or that the game re-entered (its
   `loopCount` changed, or `stateTime` went back), the harness leaves the sim's state silently and enters the recorded
   one the way the scene restore does (`snapshot_mode`: no events, RNG restored). `act_live_float` then returns the
   recorded values of the actions' private float fields (a Wait's timer, a drawn WaitRandom time). The harness
   supplies them through `fsm_world.live_field`.
2. **Fields.** Every differing importable entry is written, in four passes: activation, then transforms, then
   bodies, then the rest. This order overwrites what activation callbacks changed. A name or string the sim does not
   have is skipped, except a string, which is interned. The re-entry and write loop runs up to three rounds, until
   nothing importable differs.
3. **Contacts.** The moved shapes are flushed and the broadphase finds new pairs (`hkls_update_pairs`). Each pair
   the game has touching (`PhysicsContacts2D` records) is made to touch in the sim, with the record's state
   (`hkls_touch`), so the next step reports Stay, not Enter. Proxy ids and the contact list's order are not
   imported: they are the broadphase's allocation history. The report counts each outcome under `contact_import`.
   The sim's proxy ids differ from the game's from the first frame on, so the order of new contacts, and with it
   the order of the step's trigger and collision callbacks (`b2World.contactList`, `PhysicsContacts2D.collisions`,
   an FSM that keeps the last collider it was told of), is not one step's own divergence.

After the import, differences can remain: export-only fields, and fields the import could not write. The harness
calls these *carried*. A difference that is still identical after the step (same entry, same sim value, same game
value) is counted under `carried`, not as the step's own. The step's own differences are *fresh*. An importable field
that still differs after the import is counted under `import_lossy`.

## Report

`--out` writes JSON: `frames` (per frame: index, frame count, step, kind, `differing` fresh gameplay fields,
`differing_ui`, `carried`, `first`: the first fresh gameplay differences with sim and game values, `trap`),
`kinds` and `carried` (per field kind: frames, fields, first frame, most frequent objects), `import_lossy`,
`initial` (the first import's differences), `traps` and `contact_import`. A field kind is the component class and
field, with object names stripped (`GameObject.lp.y`, `FsmState.stateTime`, `FsmVariables.float:*`). Objects under
`_GameCameras` (the cameras and the HUD) are outside the gameplay scope. Their kinds carry ` [ui]` and are not
ranked. A trap in a step or in a re-entry is counted under its message. The frame's differences belong to the
trap. The run stops after 200 traps.

The ranked table orders gameplay kinds by the number of frames they diverge in. `Random.state#k` differs when a
frame draws a different number of `Random` values. That count is not a goal (`docs/principles.md`). The import puts
the state back every frame. Within a frame, a value drawn in another order than the game's (a knocked prop's
velocity, a random wait) differs although its distribution is right; read such a row next to the frame's
`Random.state` row.

## What is not covered

Sim state with no entry, so it is neither imported nor compared: hero coroutines, input and the device shim
(`hero.co`, `hero.in`), `hero.box`, `HeroAnimationController`; action private state other than the
re-entry floats; `fsm.delayed`, `fsm.eventTarget`, `lastTransition`; the lifecycle lists, Start queue, coroutines
and delayed destroys (`lc_*`, `BehaviourManager`, `DelayedCall`); iTween, AutoRecycleSelf, ConstrainPosition,
GrimmballControl, flare pillars, object pools, TinkEffect/AlertRange/NonBouncer/LimitSendEvents/TrackTriggerObjects;
`tk2dSprite` sprite ids; Box2D contact internals (manifolds, TOI, island state) and proxy ids (compared, not
imported); the double clocks (`TimeManager`: the sim keeps float `time`/`fixedTime`, so the import writes the float
properties); a nested body's parent-sync anchor (`go_inst fol_*`, `body_follow_parent`), so an import that moves
the parent re-syncs the child's body from its Transform, as a parent move does; the body import then overwrites it;
the stored local quaternion (the record has `leuler.z` only, so the sim rebuilds `q` through `Quaternion.Euler`,
which need not give the game's serialized `q`: native-transform_time.md §10 P-1). Rotated objects' world pose,
`lossy` and `euler.z` can differ in the last bits for that reason.
A divergence in any of these first shows up in a covered field a frame or more later.

## Test

`tests/test_lockstep.py` runs the harness on the first four frames of GG_Hornet_1 `ph_ep00`, cut with
`hkpy.staterec.truncate` into `tests/fixtures/lockstep/`. It checks five things. The SceneReady import leaves no
importable gameplay field differing. The frame kinds are step, live, live_last. The frozen step frame is exact.
Hornet's free fall from rest lands on the game's bits. A one-ulp difference and a signed zero are reported.
