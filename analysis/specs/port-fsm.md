# port-fsm — PlayMaker runtime, action library, tk2d animator, HK gameplay components (C11)

Owner: port worker F. Status 2026-08-31: landed in `sim/fsm/` (built with `-DHKSIM_MODULES="core;fsm"`), open-loop
replay of Hornet is full-length on all four R2 corpora with zero FSM-side divergences (§5). This document is the
port's design record; the decomp (`analysis/decomp/`) and the dumps (`analysis/dumps/GG_Hornet_1/`,
`analysis/fsm/GG_Hornet_1.json`) are the truth it was written from; the specs (`fsm-runtime.md`, `fsm-actions.md`,
`boss-hornet.md`, `tk2d-animator.md`, `frame-order.md`, `damage-path.md`) were used as a cited index and are
corrected in §3 where the source disagrees with them.

## 0. Deliverables

| piece | files | notes |
|---|---|---|
| generator | `sim/fsm/gen/gen_tables.py`, `sim/fsm/gen/synth_scene.py` | `python gen_tables.py GG_Hornet_1` compiles `analysis/fsm/<scene>.json` + dumps into `sim/fsm/tables_<scene>.{c,h}` (checked in); `--json <path>` for synthetic scenes; prints `round-trip: OK` after re-reading its own output |
| tables | `sim/fsm/fsm_tables.h`, `tables_GG_Hornet_1.{c,h}`, `tables_SYNTH_fsm.{c,h}` | 962 FSMs / 23 958 actions / 312 action types / 9 170 interned strings; 31 FSMs flagged live |
| runtime | `fsm.h`, `fsm_rt.c` (Fsm/FsmState/FsmStateAction/DelayedEvent), `fsm_world.c` (objects, transforms, SetActive, event fan-out, physics bridge), `act_core.c` / `act_transform.c` / `act_hk.c` (111 action vtables), `tk2d.c`, `hk_comp.c` (HealthManager, DamageHero, Recoil, ConstrainPosition, HitTaker, EnemyHitEffectsUninfected, FlingUtils, AudioEvent), `itween.c` | ≈4.8 k lines of hand-written C plus the generated tables |
| entry points | `fsm_api.c` (`hkfsm_*` ctypes API, trap-armed), `fsm_iface.c` (`hksim_fsm_iface_get()` for `sim/core/sim.c`) | |
| tests | `harness/tests/test_fsm.py` | (a) 25 synthetic runtime checks on `SYNTH_fsm`, (b) open-loop replay `--replay r2_idle.a,r2_move.a,r2_rand1.a,r2_rand2.a`, (c) generator round trip, `--cold-start <corpus>` census |

Build: `cmake -S sim -B sim/build-f -G Ninja -DHKSIM_MODULES="core;fsm"`, `cmake --build sim/build-f`;
run the tests with `HKSIM_DLL=sim/build-f/hksim.dll` (the harness defaults to that path).

## 1. Design

### 1.1 Tables and generator

* Every FSM, state, transition, action and field of the dump is compiled to static `const` tables
  (`fsm_tables.h`): `fsm_def` → `fsm_state_def` → `fsm_action_def` → `fsm_field` → `fsm_pv` (a tagged value:
  literal / local variable / global variable / None / dangling, kinds `PV_BOOL…PV_FARRAY`, `PV_FVAR`, `PV_FEVENT`,
  `PV_OWNERDEF`, `PV_EVTARGET`, `PV_FUNCCALL`, `PV_ARRAY`, `PV_OBJREF`, `PV_UNSUPPORTED`). Variables live in 15
  typed buckets per FSM (`VB_*`, mirroring `FsmVariables`), globals from `globals.json` (31 variables, 51 events).
* Variable references resolve local → global → dangling (a `useVariable:true` field naming a variable that exists
  in neither store gets a fresh instance, `FsmVariables.cs:1195` read-through semantics).
* Game objects: `bosses.json` hierarchy first, then every `scene.json` collider row, then FSM owners; per-object
  FSM order = the `PlayMakerFSM` component order of `scene.json`. Transforms come from the collider rows; a
  parent without a row is inferred from a child's world/local pair (`infer_parent_transforms`); `_GameCameras` /
  `CameraParent` get the origin pose; `Boss Holder/Godseeker Crowd` is derived from its own FSM variables
  (`DERIVED_TRANSFORMS`, cited in the generator). Objects without any pose trap on first transform read.
* Live set (`LIVE_RULES`): the Hornet hierarchy, `Needle/Control`, `Needle Tink/Setup and Follow`, `Boss
  Holder/FSM`, `Godseeker Crowd/{Control,Set Target}`, `CameraParent/CameraShake`, the 11 root `Knight` FSMs and
  `Knight/Effects/Damage Effect/Knight Damage` (31 FSMs). Everything else is compiled but inert (never started).
* Strings are interned once, emitted last (the string table is only known after every section has interned).

### 1.2 Runtime core (`fsm_rt.c`, line-cited to `analysis/decomp/PlayMaker/HutongGames.PlayMaker/*.cs`)

* `Fsm.Start` = `SwitchState(startState)` → logged as a `S -> S` transition (the recorder confirms: `Needle
  Init -> Init`). `Update` = Continue / UpdateDelayedEvents / UpdateState / UpdateStateChanges; FixedUpdate and
  LateUpdate go through the proxy behaviours (only FSMs with `handleFixedUpdate` / `handleLateUpdate`).
* `ProcessEvent` order: active-state `OnEvent` (the **last** active action's answer is the one that survives —
  `flag = action.Event(e)` is an assignment), then global transitions, then state transitions.
* `DoTransition` commits immediately when `EventData.SentByFsm != this` (an external `Fsm.Event` switches state
  inside the call); a self-sent event is committed by `UpdateStateChanges` at the end of the tick.
* `ActivateActions` aborts when a transition happened mid-`OnEnter` (`IsSwitchingState`) or when the state is a
  sequence; `CheckAllActionsFinished` raises `FINISHED` once (`FsmState.finished`), also from `OnEnter`.
* `Entered` is sticky for `OnExit`; the loop guard (`MaxLoopCount`, 1000 on all 962 FSMs) disables the component
  and runs `OnDisable`; `restartOnEnable` re-runs `Start` from `OnEnable`; `OnDisable` sends `DISABLE` first, then
  removes the FSM from `FsmList`, then `Stop`s it.
* `DelayedEvent`: `timer -= dt`, fires when `< 0`; cleared on state exit unless `keepDelayedEventsOnStateExit`.
* Fan-out: `BroadcastEvent` iterates a snapshot of `FsmList`; `BroadcastEventToGameObject` = FsmList order then
  children; `SendEventToFsmOnGameObject`; `FSMUtility.SendEventToGameObject` looks the event up with
  `FsmEvent.FindEvent` (null → nothing) and calls `Fsm.Event` per component.
* Logging mirrors `oracle/Oracle/TraceRecorder.cs`: `FSM_EVENT` is the pre-hook on `Fsm.Event(FsmEvent)` only —
  `SendEventByName` / `SendEvent` go through `Event(target, ev)` and are **not** recorded; `FSM_TRANSITION` is
  post-`SwitchState` (post-order, so an `OnEnter` cascade logs inner transitions first).

### 1.3 World (`fsm_world.c`)

* Objects carry parent / first-child / next-sibling links, a 2-D TRS (position, z-rotation, scale — Unity
  `eulerAngles` reported in [0, 360)), `activeSelf`, colliders, an optional rigidbody mirror, and the component
  instances (animator, HealthManager, DamageHero, Recoil, ConstrainPosition).
* `SetActive` (`go_set_active`): activation sets the flag then runs `OnEnable` parent-first (Recoil cancel,
  body simulated, FSM `OnEnable`, children); deactivation runs `OnDisable` **before** the flag flips (an earlier
  version flipped first and therefore never disabled the object's own FSMs — found by the Sphere Ball DISABLE in
  `r2_move.a` f25517, fixed).
* `Transform.parent` assignment is only implemented for `Needle` / `Needle Tink` (`SetParent` in
  `Setup and Follow/Deparent`, world-pose preserving as Unity's `SetParent(parent, true)`); any other target traps
  (Q-pfsm-10).
* Physics bridge: `sim/phys` symbols are resolved at run time with `GetModuleHandleExA` / `GetProcAddress`
  (`PH` table) because `#pragma weak` does not survive `__declspec(dllexport)` prototypes on mingw. Without
  `sim/phys` the bodies are mirrored locally (velocity / gravity scale / kinematic flags) and `Physics2D.Raycast`
  traps.
* Event helpers: `world_broadcast_event`, `world_broadcast_to_go`, `world_send_to_fsm_on_go`,
  `world_send_event_to_go`; `fsm_event_to` implements the `FsmEventTarget` kinds Self / GameObject /
  GameObjectFSM / BroadcastAll (FSMComponent / HostFSM / SubFSMs never occur in the dumps and trap).

### 1.4 Actions

* Each action is an `act_vtable {type_short, state_size, bind, on_enter, on_update, on_fixed_update,
  on_late_update, on_exit, on_event, proxy_cb}`; `bind` resolves the dump fields by name once into a per-instance
  struct (`a_field_req` traps on a name that is not in the dump — every bind was checked against the JSON; the only
  mismatch found was `FindGameObject.store`, not `storeResult`). Field reads go through `pf/pi/pb/ps/pv3/pgo`
  which honour `useVariable`, None, globals and owner-default targets (`Fsm.GetOwnerDefaultTarget`).
* Unported types get a vtable-less entry: `HKSIM_UNIMPLEMENTED FsmStateAction <type> in <owner>/<fsm> state '<s>'
  action <i>` at `OnEnter` (`fsm_rt.c:67`). Typed stubs that are reachable in the live set trap with their own
  message (§6).
* Registration order for the RNG-visible actions follows the decomp exactly: `SendRandomEvent/V2/V3`
  (`GetRandomWeightedIndex`, float32 weight sum, one draw per iteration; V2 spins forever on an all-excluded set →
  trap), `WaitRandom` (one draw), `RandomFloat`, `AudioPlayerOneShot` (2 draws: clip index, pitch),
  `AudioPlayerOneShotSingle` (1), `AudioPlayRandom` (2), `ShakePositionV2` (3 per update, `Random.Range(-1, 1)`
  ×3), `FlingObjectsFromGlobalPool` (1 int + per clone 2 origin + speed + angle; clones are phantom — draws only).

### 1.5 tk2d (`tk2d.c`, `tk2dSpriteAnimator.cs`)

`Play` (no restart when the same clip is playing), `PlayFromFrame` (`((float)frame + 0.001f) / fps`), wrap modes
Loop / Once / LoopSection / PingPong / RandomFrame / Single, completion and animation-event delegates, `clipTime`
advanced in **LateUpdate** by `clipTime += deltaTime * fps`. Both compound statements are evaluated as double and
rounded once (§1.8); the replays compare clip, frame and `clipTime` on every FRAME record (1 800+ records across
the four corpora) with no difference.

### 1.6 HK components (`hk_comp.c`)

`HealthManager.Hit/TakeDamage/Invincible` (`HealthManager.cs:349-517`) including `IsBlockingByDirection`, the
`DirectionUtils.GetCardinalDirection` banker's rounding, the evasion timer, `HitInstance.GetActualDirection`
(`atan2` of the source → target vector for circle-direction hits), the events `HIT / HIT LANDED / TOOK DAMAGE /
DEALT DAMAGE / BLOCKED HIT`, `sendHitTo`, recoil by direction, the **one** `Random.Range` int draw of the nail
branch (impact rotation), `Mathf.Max(hp - damage, -50)`; `Recoil` (`Recoil.cs`) with the sweep needing a raycast
distance (§7); `ConstrainPosition.Update`; `HitTaker`; `EnemyHitEffectsUninfected.RecieveHitEffect` →
`AudioEvent.SpawnAndPlayOneShot` (a pitch draw only when `!Mathf.Approximately(PitchMin, PitchMax)`) →
`FlingUtils.SpawnAndFling` (int draw, then per object 2 origin draws, speed and angle draws only when the prefab
has a Rigidbody2D). The pitch range and prefab flags are not compiled from `scene.json` yet, so the hit-effect
entry traps (Q-pfsm-6); the replays exercise HP, `TOOK DAMAGE` and the Control reactions through the harness's
`hkfsm_hm_set` / event path instead.

### 1.7 iTween (`itween.c`, `iTween.cs`, `iTweenFsmAction.cs`, `iTweenFSMEvents.cs`)

Only the `ScaleTo` launcher exists in the live set (`Sphere Ball/Grow`: `(1.5, 1.5, 1)` over 0.25 s, easeOutSine,
`LoopType.none`, `stopOnExit`). Ported: `Launch → Awake/RetrieveArgs`, `Start → TweenStart` (onstart callback,
`ConflictCheck`, `GenerateScaleToTargets`), `Update` (`percentage < 1 ? TweenUpdate : TweenComplete`),
`ApplyScaleToTargets`, `UpdatePercentage`, `Dispose`, `Stop(go, type)`, the `iTweenFSMEvents.iTweenOnStart /
iTweenOnComplete` callbacks (`Fsm.Event(finishEvent)` — null for Grow — then `action.Finish()`), easing
easeOutQuad / easeInSine / easeOutSine / linear (other ease types, delay > 0, loop types and `ignoretimescale`
trap). The timing model was fitted on all 14 `Grow -> Grow … FINISHED` intervals of the corpus set (§3, finding 7).

### 1.8 Float parity

Per `docs/float-parity.md` (amendment 2026-08-31): every compound C# float statement is evaluated in double and
rounded once at the float store / float call argument. Sites converted: `FaceAngle` / `FaceAngleV2`
(`atan2 * k + offset`), `GetAngleToTarget2D` (`target.y + offsetY - self.y`, `atan2 * k`), `GetSpeed2d`
(`Vector2.magnitude`), `FloatAdd` / `FloatSubtract` per-second, `ShakePositionV2` (`1 - timer / Duration`),
`tk2dSpriteAnimator` (`clipTime += dt * fps`, `PlayFromFrame`), iTween easing bodies and `UpdatePercentage`.
`Mathf.Atan2/Cos/Sin/Sqrt` are `(float)Math.X((double)f)` (`m_atan2`, `m_cos`, `m_sin`, `m_sqrt` in `fsm.h`).
Single-operation statements (`timer += dt`, `x *= decel`, `Vector3` operators — per-component single ops in
UnityEngine's C#) are unchanged. `Vector3AddXYZ` per-second is `Vector3 * float` then `+`, i.e. two single ops.
The decisive measurement: `r2_rand1.a` f25188 `Control.$Angle` = 22.948442459106445 in the trace; per-op float32
gives 22.948440551757812, a float32 `Quaternion.Euler → eulerAngles` round trip gives 22.948438644, the double-stack
evaluation of `FaceAngle.cs:59` gives the trace bit exactly.

### 1.9 RNG

All draws go through `sim/core/rng.h` (`hk_rng_range_f / range_i / value`) on the stream the core hands in
(`w->rng`; standalone worlds own one). Draw sources in the FSM module: §1.4 list, `HealthManager.TakeDamage`
(1 int), `EnemyHitEffectsUninfected` chain (§1.6), `AudioEvent.SelectPitch`. No other code path touches the RNG
(`grep hk_rng_ sim/fsm` is the census).

### 1.10 Frame slots (`fsm_iface.c`)

* `create`: `world_create`, binds the core RNG and the physics world, and the hero hooks from `sim/hero/hero.h`
  (`fsm_send_event` / `fsm_set_bool` onto `Knight/ProxyFSM`, `Superdash`, `Spell Control`,
  `CameraParent/CameraShake`, `Damage Effect/Knight Damage`; the other hooks are no-ops until the Knight FSMs are
  driven, Q-pfsm-14).
* `scene_start`: cold start (§4) looped until Hornet's body collider enables.
* `fixed_update`: `PlayMakerFixedUpdate` for the FSMs that handle it, then `HealthManager` / `Recoil` fixed parts.
* `update_before_hero`: **all** live FSMs. Census basis: in every corpus the boss-side `FSM_EVENT` /
  `FSM_TRANSITION` records of a frame sit between `FIXED` and `HC_UPDATE_PRE` (e.g. `r2_move.a` f25517: Control
  `FINISHED`, Sphere Ball `DISABLE`, `Sphere A -> Sphere Recover A`, then `HC_UPDATE_PRE`), i.e. `PlayMakerFSM.Update`
  precedes `HeroController.Update`. `update_after_hero` is empty. Worker H's measurement that an enemy-hit
  `RecoilRight` lands on the hero after `HeroController.Update` (`r2_attack.a` f24650, Q-phero-6) concerns the
  `HealthManager.Hit` path, which is physics-callback driven and not yet routed (Q-pfsm-11).
* `frozen_update` (R2 frozen frame, `frame-order.md §3.2`): `Update` still runs with `deltaTime = 0` on every
  MonoBehaviour; the oracle's `FsmPauseGate` gates only `PlayMakerFSM.Update` / `PlayMakerLateUpdate` /
  `PlayMakerFixedUpdate`. The module therefore ticks `HealthManager.Update`, `ConstrainPosition.Update` and iTween
  with `dt = 0` in frozen frames and no PlayMaker FSM.
* `late_update`: FSMs with `handleLateUpdate` (exactly one in the scene, `Orbit Shield(Clone)/Control`, not live),
  then `tk2dSpriteAnimator.LateUpdate` for every animator.
* `emit_entities` / `emit_events`: ENTITY block through `tracew`, `boss_dead` / `boss_hp` from Hornet's
  HealthManager. `on_phys_event` traps until the body/shape user-handle map lands with `sim/phys` (§7).

### 1.11 Snapshot init vs cold start

* Snapshot (`hkfsm_snapshot_init`, used by the replay): every live FSM is silently re-entered in the state the
  first FRAME reports (`state_silent_enter/exit`, RNG saved and restored around it), variables / HP / animator /
  poses are forced from the trace, then frame 0's own LateUpdate runs (FRAME is captured before LateUpdate).
  Timers of `Wait` / `WaitRandom` actions that were already running at SceneReady are unknown (Q-pfsm-1); the
  harness takes their first expiry from the trace and counts it as an initial-condition injection (2 per corpus:
  `Evade Range/Fluctuate FINISHED`, `Godseeker Crowd/Set Target FINISHED`).
* Cold start (`hkfsm_cold_start`, `scene_start`): `OnEnable` in dump order (DontDestroyOnLoad objects first, then
  the scene), animator `Start`, then `Fsm.Start` for the whole `FsmList` in order (§4).

## 2. Action-type coverage

Counts from `hkfsm_action_types` and the dump (`harness/tests/test_fsm.py` prints the same numbers):

| set | distinct types | action instances | ported | stubs (typed trap) | unported |
|---|---|---|---|---|---|
| whole scene (962 FSMs) | 312 | 23 958 | 175 registered vtables (sections 9-10) | | 137 (never started) |
| Hornet-reachable live set (20 FSMs: Hornet hierarchy, Needle, Needle Tink, Boss Holder, Godseeker Crowd, CameraShake) | 102 | 612 | 99 | `SpawnObjectFromGlobalPool` (Control/Stun Start), `iTweenMoveTo` + `DestroySelf` (Needle/Control Return, Destroy), `DestroyObject` (Corpse Hornet/Blow) | `FireAtTarget` (Control/Fire, **disabled** in the dump), `CreateObject`, `SpawnRandomObjectsV2` (Corpse Hornet/Blow, death only) |
| Knight live set (11 root FSMs + Knight Damage) | 130 | 1 758 | 77 | `GetLanguageString`, `SpawnBlood`, `iTweenMoveBy`, `SpawnObjectFromGlobalPool`, `DestroyObject` | 53 (`ListenFor*` ×12, `CallMethodProper`, `AudioPlay`, `RayCast2d/V2`, `SetParticleEmissionRate`, …) |

Ported types by file (Hornet-set usage count in parentheses):

* `act_core.c` (37): AddEventRegister(1) BoolTest(25) BoolTestMulti(3) FindChild(4) FindGameObject(2) FloatAdd(2)
  FloatClamp(2) FloatCompare(19) FloatInRange(6) FloatMultiply(3) FloatOperator(5) FloatSubtract(2)
  FloatTestToBool(4) GGCheckIfBossScene(1) GameObjectIsNull(1) GetHero(1) GetOwner(12) GetParent(10) GetTag(1)
  GotoPreviousState(5) IntAdd(4) IntAddV2(1) IntCompare(7) IntOperator(2) NextFrameEvent(10) RandomFloat(4)
  SendEvent(2) SendEventByName(17) SendEventToGameObjectOptimized SendEventToRegister SendRandomEvent(4)
  SendRandomEventV2(2) SendRandomEventV3(2) SetBoolValue(12) SetFloatValue(30) SetGameObject(3) SetIntValue(4)
  SetStringValue Vector3AddXYZ(1) Wait(12) WaitRandom(9) (+ the test-only `HKSimTestConsumeEvent`).
* `act_transform.c` (33): ActivateAllChildren(1) ActivateGameObject(25) CheckCollisionSide(7)
  CheckCollisionSideEnter(6) CheckTargetDirection(1) DecelerateV2(5) DecelerateXY(3) DestroyObject* DestroySelf*
  FaceAngle(2) FaceAngleV2(1) FaceObject(11) FlipScale(2) GetAngleToTarget2D(2) GetPosition(16) GetRotation(2)
  GetScale(5) GetSpeed2d(1) GetVelocity2d(1) SetBoxCollider2DSizeVector(19) SetBoxColliderTrigger(7) SetCollider(8)
  SetGravity2dScale(11) SetIsKinematic2d(4) SetMeshRenderer(4) SetParent(2) SetPosition(14) SetRotation(14)
  SetScale(15) SetVelocity2d(23) SetVelocityAsAngle(2) Translate(1) Trigger2dEvent(14).
* `act_hk.c` (41): ApplyMusicCue(2) AudioPlayInState(1) AudioPlayRandom(1) AudioPlaySimple(16) AudioPlayerOneShot(7)
  AudioPlayerOneShotSingle(3) AudioStop(3) FlingObjectsFromGlobalPool GetFsmGameObject(1) GetFsmInt(1)
  GetLanguageString* GetPlayerDataInt(1) IncrementPlayerDataInt PlayParticleEmitter(1) PlayVibration(12)
  PlayerDataBoolTest(1) PlayerDataBoolTrueAndFalse SendMessage(2, `FreezeMoment` only) SetDamageHeroAmount(2)
  SetFsmBool(15) SetFsmFloat(1) SetFsmGameObject(2) SetFsmString(1) SetInvincible(2) SetPlayerDataBool
  SetRecoilSpeed(2) SetSpriteRendererSprite SetTextMeshProText ShakePositionV2(15) SpawnBlood*
  SpawnObjectFromGlobalPool* StopParticleEmitter Tk2dPlayAnimation(22) Tk2dPlayAnimationWithEvents(21)
  Tk2dPlayFrame Tk2dWatchAnimationEvents TransitionToAudioSnapshot(4) VibrationPlayerStop iTweenMoveBy*
  iTweenMoveTo* iTweenScaleTo(1). (`*` = typed trap stub.)

Audio, particle, vibration and snapshot actions are ported as "finish + draws only" (their RNG consumption is the
only state they leave behind).

## 3. Findings — where the source or the traces disagree with the specs

1. **`FsmState.OnEvent` keeps only the last action's answer** (`FsmState.cs:319-329`, assignment not `|=`). An
   event consumed by an earlier action but passed by a later one still transitions. Verified by synthetic test
   `onevent: [consume, pass] -> last wins`.
2. **`Fsm.Start` logs `S -> S`** and the recorder shows it (`Needle Init -> Init`, `Sphere Ball Grow -> Grow` on
   every re-activation); `fsm-runtime.md` describes Start as silent.
3. **`Wait.finishEvent` preempts a sequence** — a `Wait` with a finish event raises it from `OnUpdate` through
   `Fsm.Event`, which commits the transition before later sequence steps run (synthetic `Seq` uses `ev=None`).
4. **`SendEventByName` / `SendEvent` are invisible to the recorder** (they use `Fsm.Event(FsmEventTarget,
   FsmEvent)`), so every CameraShake rumble / shake started by the non-live Knight FSMs shows up in the traces as a
   bare transition. The harness recovers them (transition → event name, plus the `$Rumbling*` gate bools that
   `Spell Control` sets with `SetFsmBool`).
5. **Velocity- and position-derived variables read before a same-tick write** (`GetVelocity2d` /
   `GetPosition` followed by `SetVelocity2d` / `SetPosition` in the same state) cannot be verified open-loop: FRAME
   is post-Update, so the injected pose is the post-write one. The harness marks them soft (sticky until the trace
   rewrites them); every such note in §5 is of this kind.
6. **Sphere Ball DISABLE order**: `ActivateGameObject(false)` in `Sphere Recover A`'s `OnEnter` disables the ball's
   FSM before the `Sphere A -> Sphere Recover A` record is written (post-order logging) — consistent with §1.2, and
   what exposed the `go_set_active` bug (§1.3).
7. **iTween timing** (14 `Grow` launches, `r2_walk/move/dash/rand2`, `noint*`, `dt02_noseed`, `move_seed.b` at
   real-time dt): the recorded `FINISHED` lands 21 frames after a launch on the first live frame of a step and 23
   after a launch on the second — no per-frame tick model fits both unless (a) iTween ticks in **every** frame
   including frozen ones (dt = 0), first tick the frame after `AddComponent`, after the launching FSM's own
   Update, and (b) the FSM only sees completion through `action.Finish()` → `CheckAllActionsFinished` at its next
   ungated Update. With (a)+(b) all 14 intervals are reproduced (`itween.c` header). `boss-hornet.md`'s
   "FINISHED 0.25 s after Grow" is therefore 0.25 s + up to one frozen frame + one live frame.
8. **`Transform.eulerAngles` is not the culprit for the `Angle` bit** — the double-stack rule is (§1.8).
9. **Only `PlayMakerFSM.Update` is paused in frozen frames** (`oracle/Game/FsmPauseGate.cs:23-61`); `frame-order.md
   §3.2` lists `HeroController.Update` but not the other MonoBehaviours; iTween / HealthManager /
   ConstrainPosition keep ticking with `dt = 0`.
10. **`HealthManager.TakeDamage` draws exactly one int** for the nail branch (impact rotation), not the "2-3 draws"
    `damage-path.md §2.6` estimated; the remaining draws of a knight hit are on the hero side (Slash effects,
    `Knight Damage`, `EnemyHitEffectsUninfected` chain).

## 4. Pre-SceneReady window (cold start census, seed 12345, `r2_move.a`: RNG_SEED f24661 → SCENE_READY f24762)

Sim (`--cold-start r2_move.a`): 102 frames (the trace's Δ`fixed_count` 912 → 1014 is also 102; SceneReady is
inside the 102nd frame counted from the seed frame), 3 draws, `Control = GG Fall`, every Hornet-hierarchy state
equal to the first FRAME's. What runs before SceneReady, in `FsmList` order:

* frame 0 (all `Fsm.Start`s): `CameraShake Init -> Normal`; `Knight/ProxyFSM Init`; `Knight Damage Init -> Idle`;
  `Godseeker Crowd/Set Target State 1`; `Godseeker Crowd/Control Init Target -> Init`; `Boss Holder/FSM Wait`;
  `Evade Range/FSM`, `Evade Check/FSM`, `A Sphere Range/FSM`, `Refight Range/FSM`, `Sphere Range/FSM`,
  `A Dash Range/FSM`, `Run Away Check/FSM` all `Init -> Detect`; `Evade Range/Fluctuate Off`; `Stun Control Init ->
  Heavy Blow -> Idle`; `Control Pause`; `Needle Tink/Setup and Follow Pause`; then in the same frame's Update:
  `Boss Holder/FSM Wait -> Wait 2`, `Control Pause -> Init -> (GG BOSS) Inert -> GG Intro 1`, `Needle Tink Pause ->
  Setup -> Deparent -> Follow`.
* frame 1: `Boss Holder/FSM Wait 2 -> State 1`.
* every frame: `Evade Check/FSM Detect -> Exit -> Detect` (`EXIT` from the trigger proxy — the trace shows the same
  pair on every live frame).
* frame 24: `ProxyFSM Init -> Idle`; `Godseeker Crowd/Control Init -> Front -> Delay 1` — **draw 1**
  (`WaitRandom`).
* frame 27: `Godseeker Crowd/Control Delay 1 -> LookR` — **draw 2** (`SendRandomEvent`).
* frame 101: `Control GG Intro 1 -> GG Fall` (`Wait 2.0`) — **draw 3** (`GG Fall` `AudioPlayerOneShotSingle`);
  `GG Fall` enables the body collider, which is what `TrainingEnv.Reset` waits for.

The other 52 of the trace's 55 pre-SceneReady draws are not FSM-side: the live set has no other draw source in
this window, so they belong to the hero (`HeroController` / Knight FSM audio), the HUD/UI FSMs, or scene-load code
(Q-pfsm-15). The core loader can drive the window with `scene_start` (or `hkfsm_cold_start` + per-frame
`fixed/update/late` until `hkfsm_go_collider` reports Hornet's body collider enabled); it must disable that
collider first because the dump serialises it enabled (Q-pfsm-7).

## 5. Open-loop replay (`test_fsm.py --replay`)

Injected from the trace each frame: hero and Hornet poses (FRAME), HeroController → `ProxyFSM` events, range
trigger ENTER/EXIT, `Control` LAND/collision-side events (raycasts need `sim/phys`), `Knight Damage`
DAMAGE/HAZARD DAMAGE, CameraShake shakes started by non-live senders, HP after knight hits. Compared on every FRAME:
every live FSM's state and variables, Hornet's animator clip / frame / `clipTime`, HealthManager HP / invincible,
Hornet's `scale_x` facing, RNG state (a mismatch is attributed by the frame's record owners and the stream is
resynced), and the full per-FSM event / transition log in slot order.

| corpus | live frames | zero-issue frames | divergences | FSM-side divergences | first divergence |
|---|---|---|---|---|---|
| r2_idle.a | 244 | 239 | 0 | 0 | none |
| r2_move.a | 605 | 586 | 13 (all RNG resyncs) | 0 | f25124, 1 draw, hero-side (nail swing) |
| r2_rand1.a | 485 | 455 | 24 (all RNG resyncs) | 0 | f24711, 1 draw, hero-side (`Slash/damages_enemy`) |
| r2_rand2.a | 485 | 458 | 21 (all RNG resyncs) | 0 | f24806, 1 draw, hero-side |

External injections: 259 / 865 / 647 / 580; initial-condition injections 2 each (§1.11); precision resyncs 0;
soft velocity/position notes 60 / 93 / 223 / 93 (§3 finding 5). "Zero-issue frames" excludes every frame that
carried a resync note, so the gap to the live-frame count is the resync frames plus the trailing FIXED-only frames
without a FRAME record. The RNG resyncs are all hero-owned draws that the FSM module does not simulate: 1 draw per
nail swing (`Slash` / `Hit L` / `Hit R` `damages_enemy` frames), 15–29 draws per knight-damage frame (`Knight
Damage` + `Damage Effect` chain), 15 per `HeroCtrl-LeftGround` / dash-burst frame, and the Superdash crystal
spawner (`SD Crystal Gen`, 2 + 8 draws per frame while active). Worker H's hero port reproduces those streams
bit-exactly, so a combined run should need no resync.

## 6. Trap list (every `HKSIM_UNIMPLEMENTED` / `HKSIM_UNKNOWN` in `sim/fsm/`)

| site | condition | reachable from the live set? |
|---|---|---|
| `fsm_rt.c:67` | action type without a vtable | Knight FSMs only (53 types) / Corpse Hornet death (`CreateObject`, `SpawnRandomObjectsV2`) |
| `fsm_rt.c:501` | `FsmEventTarget` FSMComponent / HostFSM / SubFSMs | no (absent from every dump) |
| `act_core.c:46,69` | `Wait` / `WaitRandom` `realTime` (wall clock) | no |
| `act_core.c:242,268,266` | unknown `FloatOperator` / `IntOperator` op, int divide by zero | no |
| `act_core.c:339,354` | `FindGameObject` by tag; `GetTag` on an object whose tag is not dumped | no (by-name only in the live set) |
| `act_core.c:450` | `SendRandomEventV2` all weights excluded (C# spins) | no |
| `act_hk.c:184` | `ShakePositionV2.FpsLimit > 0` (`Time.unscaledTime`) | no (0 on all 15 instances) |
| `act_hk.c:305,306` | PlayerData field missing / wrong kind | no |
| `act_hk.c:419` | `SendMessage` other than `FreezeMoment` | no |
| `act_hk.c:428` | `GetLanguageString` (Language sheets not dumped) | Knight only |
| `act_hk.c:439` | `SpawnObjectFromGlobalPool` | **yes: `Control/Stun Start`** (stun effect prefab; cosmetic pooled object) |
| `act_hk.c:461` | `FlingObjectsFromGlobalPool` with an FSM event on the clone | Knight only |
| `act_hk.c:475` | `SpawnBlood` | Knight only |
| `act_hk.c:479` | `iTweenMoveTo` / `iTweenMoveBy` | `Needle/Control Return` (the needle's flight back to Hornet) |
| `act_hk.c:509` | `iTweenScaleTo` speed mode | no |
| `act_transform.c:114,120` | `SetRotation` from a quaternion / non-z euler | no |
| `act_transform.c:195,196` | `FaceObject.resetFrame/playNewAnimation` without an animator | no |
| `act_transform.c:291` | `GetVelocity2d` in `Space.Self` | no |
| `act_transform.c:475` | `Trigger2dEvent.collideTag` on an object without a dumped tag | no (`HeroBox`/`Knight` tags are dumped) |
| `act_transform.c:620,629` | `DestroySelf` / `DestroyObject` | `Needle/Control Destroy`, Corpse Hornet |
| `fsm_iface.c:183` | `on_phys_event` routing | until `sim/phys` user handles land |
| `fsm_world.c:233` | transform of an object without a dump pose | no (camera chain / holders covered) |
| `fsm_world.c:355` | `Transform.parent` on anything but Needle / Needle Tink | no |
| `fsm_world.c:386` | rigidbody use on an object without a dumped Rigidbody2D | no |
| `fsm_world.c:464` | `Collider2D.bounds` for non-box colliders | not from FSM actions |
| `fsm_world.c:497` | `Physics2D.Raycast` without `sim/phys` | **yes** (`CheckCollisionSide`, Recoil sweep) — the replay injects the outcomes |
| `fsm_world.c:709,799,807` | variable bucket mismatch, unknown field name, array access on a non-array | no |
| `hk_comp.c:62` | `EnemyHitEffectsUninfected` pitch range not compiled | **yes, on any knight hit** once the hit path is driven (Q-pfsm-6) |
| `hk_comp.c:113,142,153,158` | `HealthManager` invincible-effect branch, other attack types, NonFatalHit alt animation, `Die` | Die = episode end |
| `hk_comp.c:208,210,227` | Recoil sweep needs raycast + hit distance; `freezeInPlace` | **yes** when Hornet is hit while recoiling (§7) |
| `itween.c:57,158,167,203` | other ease types, loop types, delay > 0, ignoretimescale | no |
| `tk2d.c:20` | animator whose clip library is not dumped | no for Hornet; `Godseeker Crowd` sprites have none (Q-pfsm-5) |

## 7. Interface change requests (orchestrator-owned files)

1. `sim/core/phys.h`: `phys_raycast` should return the hit distance / point and the hit collider's `isTrigger`
   and layer (Recoil sweep `Recoil.cs`, `CheckCollisionSide` uses `1 << layer` masks); a `phys_bounds` for circle
   colliders.
2. `sim/core/sim_modules.h`: `on_phys_event` needs the body/shape → `HKSIM_USER_FSM_BASE` handle map from `sim/phys`
   so `world_dispatch_trigger/collision` can run `HealthManager.Hit`, `DamageHero`, `Trigger2dEvent`,
   `CheckCollisionSideEnter`.
3. `sim/hero/hero.h`: a `soul_gain` hook (`HealthManager.TakeDamage` nail branch, currently
   `hero_damage_pending |= 2`), and a shared PlayerData store (the FSM module reads `playerdata.json` at create;
   `IncrementPlayerDataInt` / `SetPlayerDataBool` write a private copy).
4. SceneDumper (`oracle/`): dump every Transform (not only collider owners), every `tk2dSpriteAnimator` library,
   `EnemyHitEffectsUninfected.enemyDamage.{PitchMin,PitchMax}` + prefab Rigidbody2D flags, object tags,
   EventRegister registrations, and the serialized (pre-`GG Fall`) `enabled` of Hornet's body collider.
5. `sim/CMakeLists.txt`: nothing needed — `file(GLOB … fsm/*.c)` picked up `itween.c`.

## 8. Open questions

### Q-pfsm-1 — in-flight timers at SceneReady
`Evade Range/Fluctuate` (`WaitRandom`) and `Godseeker Crowd/Set Target` (`Wait`) are mid-timer when the first FRAME
is captured; their remaining time is in no record. The snapshot init cannot reproduce their first expiry (the
harness injects it from the trace). A cold start does not have the problem; the core loader should prefer
`scene_start` over a snapshot for episode resets.

### Q-pfsm-2 — FsmList / Start order
The port starts DontDestroyOnLoad objects first (`_GameCameras`, `Knight`) then scene objects in dump order, and
orders FSMs on one object by `scene.json` component order. Nothing in the four corpora contradicts it (all f0
records match), but the census is one scene; `GG_False_Knight` etc. should be checked when their dumps exist.

### Q-pfsm-3 — `GameObject.Find` order
`FindGameObject` by name returns the first registry match in dump order; Unity's order for duplicate names is
undefined. The live set only looks up unique names.

### Q-pfsm-4 — EventRegister components are not dumped
`SendEventToRegister` / `AddEventRegister` use a process-wide registry (`EventRegister.cs`); the module rebuilds it
from the `AddEventRegister` actions it executes, so registrations made by non-live or non-FSM code are missing.

### Q-pfsm-5 — Godseeker Crowd animators
`Boss Holder/Godseeker Crowd/*` sprites play clips (`Tk2dPlayAnimation`) whose libraries are not in `bosses.json`;
the animator is created without a library and traps on first `Play`. Not reached in the corpora (the crowd's
Control only faces / delays there).

### Q-pfsm-6 — EnemyHitEffectsUninfected data
`AudioEvent.SelectPitch` draws only when `PitchMin != PitchMax`; the range is a component field in `scene.json`
that the generator does not compile yet, and the prefabs' Rigidbody2D presence decides the `SpawnAndFling` draw
count. Until compiled, `hm_hit` traps at the effect; the replay validates the hit path by injecting HP.

### Q-pfsm-7 — Hornet body collider serialized state
`scene.json` records the body collider `enabled: true` (dumped at SceneReady); the cold start needs it disabled
until `GG Fall` (`SetCollider`), otherwise `TrainingEnv.Reset`'s wait ends at frame 0. `scene_start` disables it
explicitly; the dumper should record the pre-Awake value.

### Q-pfsm-8 — Unity-native float rounding (resolved for the observed case)
The one bit-level mismatch seen (`Angle` after `FaceAngle`) is fully explained by the double evaluation stack. No
native round trip (`Quaternion.Euler`) had to be modelled; if a later corpus shows a residual, the harness reports it
as a `precision:` note with a resync rather than a hard stop.

### Q-pfsm-9 — the 1-draw frames
Every unattributed single draw co-occurs with a nail swing (`Slash` / `Hit L` / `Hit R` `damages_enemy` records) or
a button press (`Text/Dialogue Page Control Idle -> Text Speedup`, i.e. an input frame); `DialogueBox` has no
`Random` use, so the draw is `HeroController`-side (attack audio). Worker H's bit-exact hero replay confirms it is
not FSM-owned.

### Q-pfsm-10 — `SetParent` world-pose preservation
Implemented as `Transform.SetParent(parent, worldPositionStays = true)` for Needle / Needle Tink; the Needle Tink's
pose is not an ENTITY in the traces, so the equivalence is unverified.

### Q-pfsm-11 — hit-path slot
`HealthManager.Hit` is entered from physics callbacks (`DamageEnemies` / `damages_enemy` FSMs on the slash) and
worker H measured its hero-side recoil after `HeroController.Update`; the module exposes `hkfsm_hm_hit` and
`on_phys_event` but the slot in which the core delivers the event decides the frame parity.

### Q-pfsm-12 — iTween edge cases
`ConflictCheck` disposes the **new** tween when an identical one is running (`iTween.cs:4145`); with `stopOnExit`
the live launcher never hits it. Delay, loops, other ease types and `ignoretimescale` trap.

### Q-pfsm-13 — Needle flight
`Needle/Control` is ported but disabled in the replay: its `Return` state uses `iTweenMoveTo` (trap) and its
LAND/collision events need `sim/phys`. The Hornet side (`Throw`, `NEEDLE RETURN` broadcast) is covered by the
harness broadcasting `NEEDLE RETURN` when the trace's needle finishes returning.

### Q-pfsm-14 — Knight FSMs
The 11 root Knight FSMs and `Knight Damage` are compiled and live but disabled in the replay: 53 of their action
types are input listeners / UI / hero-controller calls (`ListenFor*`, `CallMethodProper`, …) that belong with the
hero port or the core's input layer. Their observable effects on the boss side (CameraShake gate bools, `HeroCtrl-*`
events into `ProxyFSM`) are what the harness injects.

### Q-pfsm-15 — the 52 non-FSM pre-SceneReady draws
The cold start accounts for 3 of 55 draws in the load window; the rest must be attributed by the hero / core ports
(hero audio, HUD, `GameManager` scene-load code) before a seeded cold start can be bit-exact from the seed frame.

## 9. Full-stack integration (2026-08-31, build `core;hero;phys;fsm;obs`)

### 9.1 What landed

* **Knight input listeners** (`act_knight.c`): `ListenForAttack/Jump/Dash/Cast/Up/Down/Left/Right/QuickMap/DreamNail/Superdash/QuickCast`
  read `InputHandler.inputActions.<action>.{WasPressed,WasReleased,IsPressed}` through sim/hero (`hero_pa_was_pressed` /
  `hero_pa_was_released` / `hero_pa_is_pressed`, `PA_*` order = `HeroActions` creation order) and raise the four events in the
  decomp's fixed order; the Cast/DreamNail `activeBool` gate, the Up/Down `isPressedBool` output and `stateEntryOnly` are
  per-variant. `GetButtonDown` (Spell Control/Fireball Antic, `Input.GetButtonDown("Cast")`) is always false: the oracle's
  InputDeviceShim drives InControl, never Unity's Input Manager buttons (Q-pfsm-18).
* **`CallMethodProper`**: dispatch table of every (behaviour, method) pair the live Knight FSMs use: `HeroController.{CanCast,
  CanFocus, CanNailArt, CanQuickMap, CanSuperDash, CanDreamNail, GetState, SetCState, AffectedByGravity, FlipSprite,
  EnterWithoutInput, SetDamageModeFSM, ResetQuakeDamage, StartMPDrain, StopMPDrain, AddHealth, MaxHealth, SetHazardRespawn}`,
  `GameManager.{GetSceneNameString, GetCurrentMapZone}` (data: `GG_Hornet_1`, `GODS_GLORY`), `CameraTarget.SetSuperDash`
  (cosmetic). Results are written through the `FsmVar` pool (`variableName` + `VariableType` -> the named FSM variable).
* **`SendMessage`** to the Knight now resolves to hero pokes (`RelinquishControl[NotVelocity]`, `RegainControl`, `FaceLeft/Right`,
  `FlipSprite`, `Start/EndCyclone`, `ResetHardLandingTimer`, `CancelParryInvuln`, `QuakeInvuln`, `AffectedByGravity(bool)`,
  `SetMPCharge(int)`, `TakeMP(int)`, `ResetAirMoves`, `Is/NotSwimming`, `SetStartWith*`); SpriteFlash / renderer /
  `Start|StopAnimationControl` (HeroAnimationController) are accepted no-ops; `_GameManager.{SaveGame, TimePasses,
  ResetSemiPersistentItems}` and `Camera Target.{SetQuake, SetSuperDash}` likewise.
* **PlayerData** goes through the hero's store when a hero is bound (`hero->pd` via `hero_pd_field_table` + `hero_offsetof("pd")`,
  codes i/b/f), else the dump copy; new actions `GetPlayerDataBool/Float/String`, `SetPlayerDataFloat/String`,
  `PlayerDataIntAdd`, `PlayerDataBoolAllTrue`; `HealthManager.TakeDamage`'s nail branch calls `hero_soul_gain`.
* **Raycasts** `RayCast2d` / `RayCast2dV2` through `phys_raycast` (+ `phys_shape_user` / `phys_shape_is_trigger`): origin =
  fromPosition + transform, `Vector2.normalized` (double-stack magnitude), `Space.Self` rotation, `ActionHelpers.LayerArrayToLayerMask`
  (`-5` default), `distance = |point - origin|`, V2's `storeHitDistance <- fraction` quirk kept; infinite length is clamped to 1e4.
  Hit objects resolve through the owner handles (`HKSIM_USER_HERO` -> Knight, `>= HKSIM_USER_FSM_BASE` -> FSM object; static
  colliders and the slash shapes -> null).
* Also ported: `SetBoxCollider2DSize`, `Tk2dStopAnimation`, `BoolFlip/AllTrue/NoneTrue`, `FloatAddV2`, `SetVector3XYZ/Value`,
  `SendEventByNameV2`, `SendEventByScale`, `StringCompare`, `BuildString`, `CheckSceneName`, `CheckCurrentMapZone`,
  `GGCheckIsBossRushMode`, `GGSetCanTransition`, `CheckCanDreamWarpInScene`, `Get/SetStaticVariable`, `CheckStaticBool`
  (falseEvent unconditional, as in the decomp), `FadeAudio` (volume math decides the Finish frame), `AudioPlay` (never finishes
  while the clip plays; a bound `finishedEvent` traps — clip lengths are not dumped, Q-pfsm-16), `SetParticleEmissionRate`,
  `VibrationPlayerPlay`, `SetInterpolate/Extrapolate`; typed traps `CreateObject` (spell prefabs), `GetConstantsValue`,
  `GetTagCount`, `BeginSceneTransition`. Registry: **167 of 312** scene types; the live set's only unregistered types are
  `FireAtTarget` (disabled), `SpawnRandomObjectsV2` (death).
* **`FindGameObject` by tag**: registry order (Q-fsmact-1), name-equals-tag fallback for the untagged DontDestroyOnLoad objects
  (`CameraTarget`), else null + a NOTE log record.
* **sim/phys binding** (`world_phys_bind`, called from `create`): a body for every scene Rigidbody2D the module owns (`Boss
  Holder/Hornet Boss 1` dynamic g 1.5, the corpse (inactive, unsimulated), `Needle` dynamic g 0; the Knight, pools and HUD are
  excluded), users `HKSIM_USER_FSM_BASE + go`; every collider whose `rigidbody` is that body becomes a shape on it with the
  offset/scale composed from the child chain (`chain_to_body`, rotation-free), its own object's layer (13 Hero Detector,
  14 Terrain Detector, 22 Enemy Attack) and `user = FSM_BASE + collider GO`, enabled iff `enabled && activeInHierarchy`
  (SetActive toggles the shapes, `SetCollider` / `SetBoxCollider*` already went through `PH`). Circle radii and box sizes follow
  `go_set_local_scale` (Sphere Ball tween -> `phys_shape_set_radius`). `bind_hero_body` binds the core's hero body to the Knight
  GO so `GetPosition($Hero Obj)` / `FaceObject` / `GetAngleToTarget2D` read the live pose (facing from `phys_body_scale_x`).
  `on_phys_event` resolves both shape users and dispatches `Trigger2dEvent` / `Fsm.OnTrigger*2D` / `OnCollision*2D` on the
  shape's own object. `slash_set_enabled` keeps the state; no slash shape exists yet (Q-pfsm-17).
* The frozen frame ticks iTween / HealthManager / ConstrainPosition; the cold-start log is dropped at SceneReady (the oracle
  records no FSM events before `SCENE_READY`).

### 9.2 Driver status (`harness/sim_driver.py --only r2_ --tolerances tolerances_p3.json`)

| corpus | steps before trap | trap | first divergence vs the real trace |
|---|---|---|---|
| all nine r2 corpora | 10 | `phys_collide.c:196: solid contact manifold for shape kinds 2 vs 0 not pinned (Q-pphys-4)` — Hornet's box body (layer 11) landing on a polygon terrain collider (11 x 8 collides) | record 11: the sim writes the FSM records after `HC_UPDATE_POST`, the real trace has them between `HC_FIXED_POST` and `HC_UPDATE_PRE`; DH 0 |

Before the phys binding (Hornet without a body) six of nine corpora ran their full length without a trap (idle/move/walk/jump/dash/attack
240-300 steps); rand1-3 stopped at step 89 on `SendMessage('RelinquishControlNotVelocity')`, which is now dispatched. With the body the
manifold trap is the wall.

### 9.3 Interface requests from this pass (orchestrator / other workers)

1. **`sim/core/sim.c` record slots**: `emit_events` is called once per frame after `late_update`, so every FSM record lands after
   `HC_UPDATE_POST`. The real trace order is FIXED / HC_FIXED_PRE / HC_FIXED_POST / [physics-callback + PlayMakerFixedUpdate +
   PlayMakerFSM.Update records] / HC_UPDATE_PRE / HC_UPDATE_POST / [after-hero + LateUpdate records] / FRAME. Please flush
   (`emit_events`) after `dispatch_phys_events` + `update_before_hero` and before `emit_pose(HC_UPDATE_PRE)` (which means running
   `update_before_hero` before that pose record), and again after `late_update`. Until then DH is 0 by construction.
2. **`sim/phys` trigger events for FSM bodies**: with `HKSIM_FSM_DEBUG=1` the module logs every event it receives — none arrive.
   Expected each fixed step: `TRIGGER_STAY` between `Evade Check` (layer 14 Terrain Detector, box 3.51 x 1 at Hornet + (1.26, 0))
   and the terrain (14 x 8 collides in `physics.json`), which is the real trace's `Evade Check ENTER` on every live frame, and
   `Hero Detector` (13) x `Player` (9) events for the range triggers. Either phys does not generate trigger events for
   non-hero bodies or the core drops events whose `body_a` user is `>= HKSIM_USER_FSM_BASE` before `on_phys_event`.
3. **Q-pphys-4**: polygon (2) vs box (0) solid manifold — Hornet's body vs the arena floor at step 10 of every corpus.
4. **Dumper**: `hierarchy.json.gz` holds only the loaded scene (586 objects, no `DDOL/` paths, no `CameraTarget`); the Knight's
   `Attacks/*` polygons and `Clash Tink` boxes have 0 points in `scene.json` — the P5 slash/HeroBox shapes need the
   DontDestroyOnLoad hierarchy with `PolygonCollider2D.points`.
5. **Core**: trigger events on the hero body whose shape user is `>= HKSIM_USER_FSM_BASE` (the HeroBox / slash shapes once they
   exist) must be forwarded to `on_phys_event` (today `dispatch_phys_events` breaks on non-collision hero events).

### Q-pfsm-16 — AudioPlay clip lengths
`AudioPlay` finishes when `!audio.isPlaying`; the dumps carry no clip lengths, so a state that leaves through the action's
`finishedEvent` (none in the live set) or through all-actions-finished with an `AudioPlay` still playing cannot be timed.

### Q-pfsm-17 — the Knight's attack shapes
`scene.json` has the slash / Clash Tink / HeroBox rows (layers 17 / 16 / 20, on the Knight's body) but 0 polygon points;
`hierarchy.json.gz` does not contain the DontDestroyOnLoad set. Until the points are dumped `slash_set_enabled` only records
the state and the HealthManager hit path stays unreachable in the full stack.

### Q-pfsm-18 — `Input.GetButtonDown("Cast")`
Unity Input Manager buttons are never driven by the oracle's device shim; the action stores false. If HK maps the Input Manager
"Cast" button to the same InControl action the FSM would see it on a keyboard; the recorded corpora never take that branch.

### Q-pfsm-19 — tags of DontDestroyOnLoad objects
`FindGameObject(withTag "CameraTarget")` (Superdash) finds the object by name because no dump carries tags for the
DontDestroyOnLoad set; a scene with two objects of that tag would need the real `FindGameObjectsWithTag` order.

## 10. P5 / P6 / cold-start pass (2026-08-31, later)

### 10.1 What landed

* **Cold start runs the real intro** (`scene_start`): per frame `PlayMakerFixedUpdate` → `phys_step` + the physics callbacks
  routed into the FSM world (`world_phys_step_and_dispatch`; nothing reaches the hero, it keeps its dump pose) → Updates →
  LateUpdate. Two dump facts had to be undone because every dump is taken at SceneReady: Control/GG Intro 1 has already
  applied `Translate(y += 16, World)` and GG Fall has already released the body, so the serialized spawn is the dump pose
  minus 16 (31.12, 28.59476 — floor level) with `Rigidbody2D.isKinematic = true` and the body collider disabled (Q-pfsm-20).
  Hornet now holds (31.12, 44.59476) through the 101-frame intro and starts falling on the SceneReady frame, exactly as the
  first FRAME records (44.5588 one step later).
* **Pre-SceneReady RNG** (Q-pfsm-15): the 52 draws the live FSMs do not account for are consumed at the top of
  `world_cold_start` (`HKSIM_PRE_SCENE_READY_FOREIGN_DRAWS`); the RNG state at the first live frame now equals the trace's
  (r2_move seed 12345: 1560863224, 321765990, 2982297183, 638484994 — `test_fsm.py --cold-start` prints MATCH). The
  interleaving with the 3 FSM draws is unknown and only matters for the Godseeker WaitRandom values (states still match).
* **`iTweenMoveBy`** (Knight/Nail Arts Cyclone End: (0,0,60) over 1 s, linear, World) on the shared iTween runtime
  (`GenerateMoveByTargets` :1440-1478, `ApplyMoveByTargets` :1960-1988: translate by the eased delta each tick).
* **P5**: `bind_hero_body` puts the Knight's child colliders on the hero body (slash polygons, Clash Tink boxes, HeroBox,
  the spell/charm/superdash hit boxes — 37 shapes; rotated children such as `Charm Effects/Thorn Hit/Hit L` (90°) are
  composed as rotated polygons), disabled per the dump until `slash_set_enabled` toggles the NailSlash polygon and its
  Clash Tink. `on_phys_event` routes trigger enter/stay on a `NailSlash` object to `hero_slash_trigger(slash, other layer,
  NonBouncer, …)` and on `HeroBox` to `hero_box_check_for_damage` (damages_hero FSM ints or the DamageHero component),
  and the `damages_enemy` FSMs are live: `Collision2dEvent`, `GetLayer`, `GetName`, `CheckSendEventLimit` (+
  `LimitSendEvents` per owner), `CompareNames` (array variables), `TakeDamage` → `HitTaker.Hit` (self + 2 parents) →
  `HealthManager.Hit`. `EnemyHitEffectsUninfected` data (pitch 0.75–1.25, ghost prefabs with Rigidbody2D) is compiled
  from `hierarchy.json.gz`, so knight hits no longer trap (Q-pfsm-6 closed).
* **P6**: `combat_sources` / `fsm_snapshots` (`fsm_obs.c`) per obs-wire.md §3: bucket = first rule of HO:104-124 on the
  dumped collider, SCENE# order, `isActiveAndEnabled`, `Collider2D.bounds` (box / circle / polygon / edge AABB), HM within 8
  ancestors, `ObserveMaxHp` cache, `prevRelCache` keyed per collider with `MotionTick`, kind = Strip(nearest object with a
  tk2dSpriteAnimator), clip key = `Strip(anim object)|clip`; snapshots (B) boss subtree DFS, (E)/(A) active bucket colliders,
  `NameBlacklist` from FsmObserver.cs. Verified against every OBS payload of the open-loop corpora (§10.2).
* **Generator**: `hierarchy.json.gz` ingestion (transforms/tags/layers for objects without collider rows, objects created
  under the live roots, boss-child animators that share a dumped library — the Sphere Ball now has its own animator on
  `Hornet Boss Anim` clip 17 "Sphere Ball" — and `EnemyHitEffectsUninfected` fields), explicit polygon/edge point lists,
  and a scoping bug fixed: the DamageHero / Recoil / ConstrainPosition component defs were only compiled when the
  component happened to be the last one of the first FSM-carrying row (Hornet's DamageHero, Recoil and ConstrainPosition
  were missing — 7 DamageHero defs and the Recoil (15, 0.15 s) now exist).
* tk2d `Start` on the first activation (`Play(DefaultClip)` iff `playAutomatically`), `Transform.parent` assignment for any
  object (world pose kept), `SetRotation` x/y tilt accepted on non-physical sprites, the HUD `Text/Dialogue Page Control`
  FSM live with `ListenForMenuActions` (Platform.GetMenuAction, controller-implicit, NonJapanese style) and `DialogueBox.*`
  no-ops, the entity block reports the pre-LateUpdate animator state (FRAME is captured before LateUpdate).
* Registry 175 of 312 scene types; the live set's unported types: `FireAtTarget` (disabled), `SpawnRandomObjectsV2` (death),
  `DestroyAllChildren` (HUD conversation end).

### 10.2 Verification

* Open loop (`test_fsm.py`, build `core;fsm`): idle 239/244, move full-length, rand1 / rand2 as before (0 FSM-side
  divergences, hero-side RNG resyncs only); every OBS payload of each corpus decoded with `test_obs.decode` and compared
  row by row: kinds, clip keys and the 14 features agree except the first step's `vel_y` (the reset reply's prevRel is
  sampled at SceneReady, two frames before the harness snapshot) and rows the hero would create (slash swings — the
  harness has no NailSlash). `fsm_snapshots` strings match (B/E/A order) apart from the same slash entries.
* Cold start: 102 frames, 55 draws, RNG MATCH, every Hornet-hierarchy state equal to SceneReady.
* Full stack (`harness/tools/gate.py --tol p4`): every r2 corpus runs full length (240/300 steps, no trap); the first
  divergence is hero-side on every corpus — `hero.anim.clip` at step 1 (HeroAnimationController is not ported:
  the FRAME reports `Idle` / `Turn` / `SlashAlt`, the sim `''`), or the extra hero-side record/draw the input causes at
  step 1 (r2_jump `rng0` one draw behind, r2_dash / r2_rand2 an EVENT the hero raises before `HC_UPDATE_POST`).
  With `hero.anim.*` masked the next divergence is the `Evade Check` `EXIT` one frame late (real at Hornet y 44.055, sim at
  43.839): a trigger-overlap boundary between the detector box and the arena's `Roof Collider` polygon (Q-pfsm-21).

### Q-pfsm-20 — serialized boss state vs the SceneReady dumps
The dumps are taken after the intro FSM ran; the cold start needs the pre-intro values (pose − 16, kinematic, collider
disabled). Anything else Control/Init..GG Intro 1 changes (gravity scale is written to the same value) is covered, but a
dumper pass before the first frame would remove the inference.

### Q-pfsm-21 — trigger-stay boundary
`Evade Check` (Terrain Detector, box 3.51×1 at Hornet + (1.26, 0)) keeps overlapping the roof polygon in sim/phys one frame
longer than in the real game while Hornet falls (last real stay at y 44.235, sim also at 44.055). Contact skin / persistence
semantics of the trigger overlap test (Box2D polygon radius vs Unity's) — for worker P.

### Q-pfsm-22 — hero-driven records at step 1
Input corpora show hero-side effects at the first step that the FSM module cannot produce: one extra RNG draw on a jump
press, an EVENT before `HC_UPDATE_POST` on a dash press (Dash Burst / Superdash effect FSMs bound through hero hooks
FSM_DASH_BURST etc. are not live yet), and the hero animator clip. These gate DH at 0 until the hero side lands them.
