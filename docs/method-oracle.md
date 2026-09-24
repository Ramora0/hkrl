# Method oracle

A whole-fight gate says that the sim and the game diverged, not which ported method caused it. The method oracle
checks every hand-ported PlayMaker action and component method call by call: the mod records single calls in the
real game with the inputs each call read and the outputs it wrote, and the sim replays each call from those inputs
and must write the same outputs, bit for bit.

- Recorder: `oracle/Record/MethodRecorder.cs`, opt-in with `HK_ORACLE_METHODS=1` in record mode.
- Replay entry points: `sim/fsm/runtime/method_oracle.c` (`hkmo_*`, test-only; nothing in `hksim_step` calls them).
- Tool: `tools/method_oracle.py` (`record`, `replay`, `show`, `selfcheck`, `inert`).
- Data: `analysis/method_oracle/<scene>/` (`analysis/README.md`).
- Test: `tests/test_method_oracle.py` (known-answer fixture; the full replay with `HKSIM_METHOD_ORACLE_FULL=1`).

## What is recorded

`<trace base>.methods.jsonl.gz`, one JSON object per line, format version 3 (header `{"methods": 3, ...}`).
Markers: `scene_ready` (level, knight path, the whole PlayerData int/bool/float store `pd` and its types `pdt`),
`episode_end`, `close`.

**Action calls** (`"k":"pm"`) are taken at FsmState's dispatch (`PM/FsmState.cs:302,326,337,351,363,633`): OnEnter,
OnUpdate, OnFixedUpdate, OnLateUpdate, OnExit and Event (`cb` E U F L X V). They are sampled per activation (an
OnEnter through its OnExit), because an action's private state carries from one callback to the next. Fields:

| key | meaning |
|---|---|
| `t`, `o`, `n`, `s`, `i` | action type, owner canonical path, FSM, state, action index: the identity |
| `a`, `j`, `d` | activation id, callback index in it, nesting depth of the call |
| `dt fdt fx tm ts f fc` | Time.deltaTime, fixedDeltaTime, inFixedTimeStep, time, timeScale, frame, fixed count |
| `st`, `fin0`, `sw0`, `as0`, `ps0` | state time, Finished before, pending transition, active and previous state |
| `evd` | Fsm.EventData: sender FSM (path, name), int, float, string |
| `r0` / `r1` | Random.state before / after |
| `vb` / `va` | the FSM's and the globals' (`G` prefix) variables before / after, as deltas: `vb` against the previous record of the activation (all of them in its first), `va` against `vb`. Keys `<bucket>:<name>`; the first variable of a name, as PlayMaker binds it. An Object variable holding an AlertRange is `["AlertRange", path]`, any other its object's name |
| `pb` / `pa` | the action's private fields (non-public, simple types), for diagnosis |
| `gb` / `ga` | the touched GameObjects (owner, knight, every object a field names; for TakeDamage also the target's next two ancestors that hold a HealthManager, which HitTaker.Hit reaches) before / after: active flags, position, local pose and quaternion z/w, parent and the parent chain's local poses (`anc`), Rigidbody2D velocity, tk2dSpriteAnimator and HealthManager fields, AlertRange.IsHeroInRange (`ar`), LimitSendEvents list; an FsmObject field holding a component adds that component's GameObject |
| `hc` / `hca` | CallMethodProper and SendMessage only (their knight receivers are HeroController methods): the HeroController's simple fields `f` and its cState flags `cs`, before / after |
| `vx` / `vxa` | GetFsm* and SetFsm* addressing another FSM than their own: that FSM (`o`, `n`) and its variables `v`, before / after |
| `in` | the knight's HeroActions, bit 0 IsPressed, 1 WasPressed, 2 WasReleased |
| `pd` / `pda` | PlayerData fields that differ from the scene_ready store, before / after |
| `fin`, `sw`, `as`, `ret` | Finished, pending transition and active state after; Event()'s return |
| `evl`, `evt`, `dly` | the events the call sent itself: Fsm.Event(FsmEvent), Event(target, e), DelayedEvent |
| `nd` | action callbacks the call cascaded into |

A call's own events are those sent at its own FsmExecutionStack depth (`PM/FsmExecutionStack.cs:57`); an event sent
from inside the processing of another event belongs to the cascade. The sim logs the stack depth with every
FSM_EVENT (`fsm_rt.c fsm_event`) so the replay applies the same rule.

Format 2 recordings lack `hc`, `vx`, `ar`, AlertRange variables and the HitTaker ancestors; the replay classes a
mismatch of those calls as `input` (below).

**Component calls** (`"k":"cs"`): `HealthManager.Hit`, `Recoil.RecoilByDirection`, `Recoil.FixedUpdate`,
`tk2dSpriteAnimator.Play(clip, t, fps)` and `UpdateAnimation(dt)`, with `args`, the component's simple fields
before / after (`sb` / `sa`), and the same GameObject, RNG, input and event fields. Sampled per (method, owner).

Sampling never draws from `UnityEngine.Random`: per site the first 2 activations, then those whose
FNV hash of (site, count, seed) is 0 mod 8, at most 12 per site, 400 per type, 160 callbacks per activation.

## How a call is replayed

Per episode, the game's PlayerData store at `scene_ready` is written into the reset world before its checkpoint is
taken, so a record's `pd` deltas apply to the game's store. `pd` lists, per scene, the fields where the sim's own scene
load differs from that store: divergences of the scene load, not of a call.

For each activation: restore the reset world's checkpoint, find the GameObject by canonical path (the sim's `$k` /
`#k` pool-copy suffixes dropped), the live FSM, the state and the action, and check the action's type. Then per
callback: set the clocks, the RNG state, the variables (every declaration of a name), PlayerData, HeroActions (with
the direction values and moveVector the held direction keys give, `hero_input_set_direction_values`), the
Fsm.EventData, the parent and local pose of every touched object and its parent chain (root first), velocities,
Rigidbody2D gravity scales,
active flags, animator, HealthManager and LimitSendEvents state, the HeroController's fields and cState (`hc`, through
`hksim_set_value("hero.f.<name>" / "hero.cstate.<name>")`, the names the sim's hero has), the other FSM's variables
(`vx`), put the FSM in the recorded state with its pending
transition and previous state (no other action runs), run the one callback through the action's vtable with the FSM
on the execution stack, and compare. An owner the reset world has inactive is activated through the lifecycle first,
so its components are enabled as in the game. The engine's instance arrays are refilled between callbacks
(`world_reserve`), as between stages.

An action object keeps its private fields from one activation to the next (IdleBuzzV3's `accelX` / `waitTime`
persist, its OnEnter does not reset them), so the replay carries the sim's private state (`act_inst.st`) from one
replayed activation of a site to the next, in the game's order. When the game's private fields at an OnEnter differ
from those the previous recorded activation ended with, an activation nobody recorded ran in between: the carried
state is unknown and a mismatch in that activation is an `input` gap.

Outputs compared: Finished, Event()'s return, pending transition, own events, delayed events, every variable, the
touched objects' position, local position, local scale, local euler z, velocity, gravity scale, activeSelf, animator and
HealthManager, PlayerData, the HeroController's fields and cState (`hca`), the other FSM's variables (`vxa`), and the
RNG state. Floats compare bit-exact, except that +0 equals -0 (no HK path can
tell them apart, and Unity canonicalises some signed zeros natively). A call that cascaded (`nd` > 0) is compared on
its own FSM's outputs only (its own variables, not the globals the cascade's FSMs wrote).

Unity stores a rotation as a quaternion and reads `localEulerAngles` back from it; the sim stores the angle
(`analysis/native_specs/native-transform_time.md` T0). A local euler z equals the game's when it does mod 360, or when
Unity's round trip of the sim's angle (EulerToQuaternion, NormalizeSafe, QuaternionToEuler, MakePositive: §2.5, §3.5;
`unity_euler_z`, with libm stand-ins for the engine's sinf / cosf / atan2f) gives the game's bits. The table counts
the second kind per type (`readback`): the action computed the game's angle and only the storage differs. An action
that composes rotations in quaternion space (`Transform.Rotate`: Rotate, RotateTo) still mismatches; that is the T0
class, not an action port error.

Verdicts: `exact`, `mismatch`, `rng-only` (only the RNG state after differs: the draw count, not a defect by
itself), `input` (the recorded inputs could not be reproduced: an object the call reads is missing in the sim, a
position reads back far off, the carried private state is unknown, or a format 2 record lacks the HeroController,
other-FSM, AlertRange or HitTaker-ancestor state the call reads), `trap`, `unported`, `identity` (the sim has no
such object / FSM / state / action, or another type at that index).
A touched object whose world (or local) position reads back off by rounding after its local chain is set is not
compared on that position: the sim composes transforms (and a body's local position from its world one) in other
float steps than Unity.

## Limits

Inputs the record does not carry are the reset world's: the hero's collision state and the HeroController outside
CallMethodProper / SendMessage records, other FSMs' states (and their variables outside GetFsm* / SetFsm* records),
the physics world beyond the touched objects (a raycast can hit an object the record does not pose), a Recoil's sweep
direction, AlertRange latches in format 2 records, the BossSceneController's HasTransitionedIn, iTween components, particle systems and
audio (an AudioPlay finishes when its clip ends, which the sim does not model). Pool clones share one canonical path; the replay maps them
all to the first sim clone, a GameObject variable holding a clone of the game's path matches any sim clone of that
path, and a LimitSendEvents list that holds two clones of one path makes the call an `input` gap, as does a pool
spawn (SpawnObjectFromGlobalPool, Fling*, AudioPlayerOneShot, ...) whose only differences are on clones: the clone the
game took and the clones the record names share one path, so the spawn's placement is not checked. A call that reads
them can mismatch without a
port error (`show` lists every differing field; read the action's source before calling it a bug). The Euler angle
the sim stores is not the quaternion Unity stores, so a rotation can read back a ULP apart.

## Procedure

    set HKRL_GAME=<your isolated install>
    python tools/method_oracle.py record --policy 4 --perturbed 2 --jobs 4
    python tools/method_oracle.py replay analysis/method_oracle --jobs 6 --out report.json --calls calls.tsv
    python tools/method_oracle.py pd analysis/method_oracle
    python tools/method_oracle.py coverage report.json
    python tools/method_oracle.py record --scenes GG_Gruz_Mother_V --tier 1    # -> analysis/method_oracle/GG_Gruz_Mother_V@T1

`coverage` lists, for every registered action type no recording compared, the states of the recorded scenes' FSMs
that use it and whether the sim runs that FSM: a recording that reaches such a state covers the type. A recording
under `<scene>@T<k>/` replays on the sim's level key `<scene>@T<k>` (the BossLevel k dumps).
    python tools/method_oracle.py show analysis/method_oracle/GG_Hornet_1 --type ScaleTo --n 3
    python tools/method_oracle.py selfcheck analysis/method_oracle
    python tools/method_oracle.py inert <on.hktrace> <off.hktrace>
