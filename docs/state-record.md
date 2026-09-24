# State record (`.hkstate`)

The per-frame full-state record of the real game, for the lockstep acceptance test: at the end of every frame it
holds everything the simulator's state must match. The producer is `oracle/Record/StateRecorder.cs` (managed
state), `NativeState.cs` (the engine's native state) and `StateWriter.cs` (the encoder). The reader is
`hkpy/staterec.py`; `tests/test_staterec.py` pins it to the encoder with a fixture the C# encoder wrote
(`tests/fixtures/staterec/`, regenerate with the `gen/` project). `tools/staterec_check.py` checks a recording.

## Recording

Record mode (`docs/oracle.md`) with `HK_ORACLE_STATE=1`. Each episode, from `Hooks.SceneReady` to
`Hooks.EpisodeEnd`, writes `<trace base>.hkstate` (episode 0) or `<trace base>.eN.hkstate`. A player-loop system
appended after `PostLateUpdate`'s last subsystem captures the state, so every frame is recorded, frozen frames
included, after every script, coroutine and physics callback of the frame. The first record is the SceneReady
frame; the last is the frame in which the episode ended.

A recording of a corpus: `tools/rerecord.py <corpus dir> <out> N <tag> --env HK_ORACLE_STATE=1 --timeout 3600`
(the recorder slows every frame; the default instance timeout is too short). A run whose recorder failed to install
still exits 0: check that the `.hkstate` exists.

Cost. Every value is read every frame, except values that are functions of inputs read in the same frame and
unchanged since they were last put (a field that is not put keeps its last value): a Transform's world pose
`wp.* wr.* lossy.* euler.z` (re-read when its local TRS, its parent or an ancestor's world pose changed) and
`leuler.z` (when its local rotation changed); a GameObject's `path` (its parent's path and its name); a string,
Unity-object or `FsmEvent` field whose reference (for an event, its name's reference) is the one last put; a list
of strings or `FsmEvent`s whose element references are those last put. Reflected fields are read through IL
compiled per field path (`FastAccess.cs`) into slots resolved once per class; the gzip stream is written by a
background thread. The trailer's `capture_ms_by_section` splits the capture time (`layout globals gameobjects
fsm mono unity statics engine endframe`); the log prints it every 100 frames with the costliest component types
and slow-path fields. Check it with
`tools/staterec_check.py <out>/<name>.a.hkstate [--spawn] [--respawn]`, and compare two recordings of one corpus with
`--compare`. Two game processes differ in things that are not the episode's state: frame counts and clocks (each
ran a different number of frames before SceneReady) and Unity instance ids. `--compare` pairs frames by position and
entities by identity (below), replaces instance ids by the identity of the entity that carries them (other ids, e.g.
assets, by order of first appearance, in keys, `o` fields and formatted text alike), and accepts a numeric
difference equal to the processes' offset of `Time.frameCount`, the FixedUpdate count, `Time.time`/`fixedTime` or
the double clocks at the first frame (floats within 8 ulps), and a constant offset of a process-lifetime counter
(`DelayedCallManager.timeStamp`, `DelayedCall.timeStamp`, `PhysicsContacts2D.simulationId`,
`b2World.tree.insertionCount`). Anything else is a difference; the verdict names the fields with the most.

Dump mode with `HK_ORACLE_DUMP_NATIVE=1` also writes `<dump dir>/native.hkstate`: one frame with only the native
section, taken at SceneReady (the instant of the other dumps). It is the scene-start snapshot N0 of
`analysis/native_specs/native-box2d.md` §12 (proxy ids, fat AABBs, tree free list, body and contact orders,
`m_Collisions` order), plus the behaviour lists and the delayed-call queue. Check it with
`tools/staterec_check.py <dump dir>/native.hkstate --native-only`.

Inertness: the recorder reads fields and native memory. The only properties it calls are Unity getters that read
(transform, rigidbody, collider, animator state, `tk2dSpriteAnimator.CurrentFrame`). Bypassed through fields or
native memory, because their getters have side effects:
- `Rigidbody2D.position`/`.rotation` and `Collider2D.bounds` call `PhysicsManager2D::AutoSyncTransforms`
  (`UP!0x180c12290`, `UP!0x180c126b0`, `UP!0x180c03a50`), which with `autoSyncTransforms` on (every dumped scene's
  `physics.json`) pushes pending Transform changes into Box2D. The recorder reads `m_Body->m_xf.p` and
  `m_sweep.a * 57.29578` instead, which is what the getters return after the sync.
- `PlayMakerFSM.Fsm` sets `Owner`, `FsmState.Actions` loads actions, `Fsm.ActiveState` caches,
  `PlayMakerGlobals.Instance` initializes, `HeroController.instance`/`GameManager.instance` search and call
  `DontDestroyOnLoad`.
- A static field read runs the type's static constructor if it has not run yet. Statics are read only for types
  without a static constructor and for the listed types, whose initializers the game has run before SceneReady;
  the trailer's `statics_skipped` names the rest.

It draws no `Random`. The check is the replay of one corpus with and without the recorder (window task
"inertness").

## File format

`gzip( "HKST" | i32 version=1 | u32 json_len | json | records... )`. Little-endian. `var` = unsigned LEB128,
`zig` = zigzag LEB128, `str` = var length + UTF-8.

| tag | record | payload |
|---|---|---|
| 1 | STR | var id, str. Ids count up from 1; id 0 is "" |
| 2 | CLASS | var class id, var name id, var first field index, var n, n × (var field name id, u8 type) — classes grow by appending fields |
| 3 | FRAME | var Time.frameCount, zig agent step (-1 before the episode's first), var FixedUpdate count, u8 flags (bit 0: live, deltaTime > 0) |
| 4 | BORN | var eid, var class id, var parent eid (0 none), var key id |
| 5 | DIED | var eid |
| 6 | SET | var eid, then (var field-index delta, value)…, var 0 |
| 7 | END | end of frame |
| 8 | NOTE | var string id (layout-check results, native section off) |
| 9 | TRAILER | var string id of a JSON summary: frames, births, deaths, capture ms, errors, native checks |

Field types: `f` f32 bits (4 bytes), `d` f64 bits (8 bytes), `i` i32 (zig), `l` i64 (zig), `b` u8, `s` string id,
`o` Unity instance id (zig), `e` eid (var); lists `I L F S O E` of the same (var count first). A SET carries only
the fields whose value changed since the entity's last SET (all fields are zero/empty before birth). An entity not
visited in a frame dies at the frame's end. Floats are bit patterns throughout: compare bits, not values.

Header JSON: `format, writer_version, episode, level, scene, trace_base, frames_per_wait, reset_count, seed,
mod_commit, unity_version, capture_dt, fixed_dt, armed_frame, exe, timestamp_utc`.

## Entities

Keys and identity. Every GameObject and component entity has an `iid` field (`GetInstanceID()`); `o` fields refer
to it. A GameObject's key is its path at birth (`HierarchyDumper` paths: scene roots, `DDOL/<root>`) and its
`cloneIndex` is the number of earlier GameObjects born with that path in the episode. A component's key is its
type; `index` is its position in `m_Component`. Plain objects, PlayMaker states and actions are keyed by the
object reference, native objects by address (a reused address with another owner is a new entity).
`tools/staterec_check.py` pairs two records by (class, key, parent identity, birth order).

| class | one per | fields |
|---|---|---|
| `Time` | record | `frameCount time deltaTime unscaledDeltaTime fixedTime fixedDeltaTime timeScale captureDeltaTime maximumDeltaTime timeSinceLevelLoad inFixedTimeStep` |
| `Random` | record | `state` = the four `Random.State` words |
| `Env` | record | `step fixedCount activeScene` |
| `PlayMakerGlobals` | record | `vars` (→ `FsmVariables@…`), `events` |
| `GameObject` | GameObject in the loaded scenes and DontDestroyOnLoad (not the mod's) | `iid cloneIndex name path parent sibling childCount activeSelf activeInHierarchy layer tag scene`, transform `lp.* lr.* ls.*` (local position, rotation quaternion, scale), `wp.* wr.* lossy.*` (world), `leuler.z euler.z`, `components` (O, `m_Component` order) |
| `<component type>` | component | `iid index`; Behaviours `enabled isActiveAndEnabled` (the getter reads `m_IsAdded`) and native `n.valid n.enabled`; MonoBehaviours also `n.didAwake n.didStart n.isDestroying n.addedToManager n.inUpdateList n.inFixedList n.inLateList n.coroutines` (E → `Coroutine`). The `n.*` fields are this frame's reading only while `n.valid`: once the native section is off, `n.valid` is false and the rest keep their last values |
| `UnityEngine.Transform` | | `iid index`; native `n.valid n.dispatchIndex n.index` (its TransformHierarchy's slot in the change dispatch, -1 when the hierarchy has no pending change; its index in the hierarchy), `n.physChanged` (its pending Transform-to-Box2D sync: `systemChanged[index]` restricted to the five physics bits, `PhysicsManager2D.handle.*` name them), `n.physInterest` (`systemInterested[index]`, same bits) |
| `UnityEngine.Rigidbody2D` | | `hasBody position.* rotation` (the body's `m_xf.p`, `m_sweep.a × 57.29578`; with no body the Transform's position and eulerAngles.z), `velocity.* angularVelocity gravityScale mass drag angularDrag inertia centerOfMass.* worldCenterOfMass.* bodyType isKinematic simulated useAutoMass useFullKinematicContacts constraints collisionDetectionMode sleepMode interpolation awake attachedColliderCount` |
| `UnityEngine.*Collider2D` | | `enabled isTrigger offset.* density usedByEffector usedByComposite sharedMaterial attachedRigidbody shapeCount`; box `size.* edgeRadius autoTiling`; circle `radius`; polygon `pathLengths points`; edge `points edgeRadius`; native `n.relativeTransform n.rigidbodyScale n.errorState n.shapeCount` |
| `UnityEngine.Animator` | | `valid speed controller cullingMode updateMode applyRootMotion layerCount`, per layer `L<k>.hash normalizedTime length speed speedMultiplier tag loop weight inTransition next.* transition.*`, parameters `p:<name>` |
| `UnityEngine.AudioSource` / `*Renderer` | | `isPlaying clip` / `enabled` |
| game MonoBehaviour (Assembly-CSharp, -firstpass, PlayMaker; not TMPro/UnityEngine) | | every instance field below `MonoBehaviour`, by reflection (below); `tk2dSpriteAnimator` adds `tk2d.clip tk2d.playing tk2d.currentFrame tk2d.clipTimeSeconds` |
| `PlayMakerFSM` | | `fsm.present fsm.name fsm.active fsm.previous fsm.switchTo fsm.started fsm.finished fsm.initialized fsm.activeStateEntered fsm.switchedState fsm.lastTransition fsm.eventTarget.*` (flattened like an `FsmEventTarget` field, below) `fsm.delayed` (E → `DelayedEvent`), `fsm.vars` (→ `FsmVariables@…`), `fsm.states` (E → every `FsmState`, in `Fsm.states` order), `fsm.state` (→ the active one) |
| `FsmVariables@<hash>` | variable set (per FSM, and the globals) | `<kind>:<name>` per variable: the stored `value` field (FsmEnum `intValue`; FsmArray and FsmVar formatted) |
| `FsmState` | state of an FSM (every state: an action's private fields persist across visits, e.g. `SendRandomEventV3.loops`, and a state entered and left within one frame is never active at a frame's end) | `name active finished activeActionIndex stateTime realStartTime loopCount maxLoopCount actionsLoaded actions` (E → `action:<type>`), `activeAction activeActions finishedActions` (action indices) |
| `action:<type>` | action of a state | `base.enabled base.active base.finished base.entered` + every field of the action type below `FsmStateAction` |
| `DelayedEvent` | pending delayed event | `event target timer delay fired eventData` |
| `plain:<type>` | non-Unity object reached from a recorded field (e.g. `HeroControllerStates`, `PlayerData`, InControl devices and actions, coroutine iterators) | every instance field, by reflection |
| `static:<type>` | game MonoBehaviour type in play without a static constructor, plus `Fsm FsmEvent FsmExecutionStack PlayMakerFSM PlayMakerGlobals iTween iTweenFSMEvents ObjectPool InControl.InputManager HeroBox HeroController GameManager BossSceneController StaticVariableList` | every static field that is not a constant |

Reflection, by the field's declared type: primitives and enums as `b i l f d`; strings `s`; Unity objects `o`;
structs flattened (`name.x`, depth 3); `NamedVariable` fields as their stored value; `FsmOwnerDefault` as
`name.ownerOption name.gameObject`; `FsmEventTarget` as `name.present name.target name.excludeSelf
name.gameObject.ownerOption name.gameObject.gameObject name.fsmName name.sendToChildren name.fsmComponent`;
`FsmEvent` as `s` `ev:<name>` (`null` for none); delegates as `S` of `Type.Method@target` (target `#iid` for a Unity object,
`#<PlayMakerFSM iid>/<state>/<action index>` for a PlayMaker action, else the type name); lists of numbers or vectors `I L F`,
of Unity objects `O`, of plain objects `E` (first 256, with `name#n`); strings, dictionaries, other collections
and `object`/interface fields as formatted strings (`S`, first 256, with `name#n` count and `name#h` hash of all
items). Formatted values are process-independent: floats as bit patterns, Unity objects as `#iid`, never object
hashes. PlayMaker's structure (`Fsm`, `FsmState`, actions, transitions, templates), reflection and
runtime plumbing, yield instructions and curves are formatted, not expanded (the FSM structure is dump data,
`analysis/fsm`). Plain objects are expanded to depth 4 from a component, at most 60000 per frame
(`plain_truncated`, `plain_budget_hits` in the trailer).

## Native section

Read through `UnityEngine.Object.m_CachedPtr` with the layouts of `analysis/decomp_native/types/*.layout.txt`;
globals by RVA from `analysis/decomp_native/symbols.tsv` (`gContext[7]` TimeManager, `gContext[8]`
DelayedCallManager, `s_instance{,Fixed,Late}BehaviourManager`, `s_instanceUpdateManager`). Every read checks the
page (VirtualQuery); a failed read turns the section off for the episode with a NOTE.

| class | fields |
|---|---|
| `TimeManager` | `fixed.* dynamic.* active.*` (`cur last` as f64, `delta`), `firstFrameAfterReset firstFrameAfterPause firstFixedFrameAfterReset frameCount captureDeltaTime sceneLoadOffset useFixedTimeStep timeScale maximumTimestep` |
| `BehaviourManager` ×4 (`Update FixedUpdate LateUpdate UpdateManager`) | `orders` (execution-order buckets, ascending), `q<order>.active q<order>.add` (O, list order). Only the orders in `orders` are current |
| `DelayedCallManager` | `timeStamp`, `queue` (E → `DelayedCall`, multiset order: ascending key, FIFO in a key) |
| `DelayedCall` | `time frame repeatRate repeat call cleanup object mode timeStamp coroutine invoke` — `call` names `MonoBehaviour::DelayedStartCall` (pending Start), `Coroutine::ContinueCoroutine` (a coroutine resume), `DelayedDestroyCallback` (`Destroy(obj, t)`), `ForwardInvokeDelayed` (`Invoke`) |
| `Coroutine` | `behaviour iterator.type refCount doneRunning isIEnumerator asyncOperation continueWhenFinished waitingFor iterator` (E → the iterator object: its `<>1__state`, current yield, locals). The iterator is resolved through the enumerator's GC handle only when the handle has the shape `MonoBehaviour` gives it (strong, 32-bit, a node of its owner's coroutine list, cached object and its vtable readable); otherwise `iterator` is 0 |
| `PhysicsManager2D` | `handle.rigidbodyT/R/S/Anim handle.colliderTRS` (bit positions of the Transforms' `n.physChanged`), `handle.rigidbodyParentHierarchy handle.colliderParentHierarchy`, `physMask`, `dispatch.combinedChanged` (the dispatch's combined changed mask, physics bits), `dispatch.hierarchies` |
| `PhysicsScene2D` | `handle lastSimulationTime lastSimulationDelta runningSimulationStep rigidbodyHierarchyChanged movementStates` |
| `b2World` | `flags bodyCount bodyList nonStaticBodies staticBodies gravity.* allowSleep inv_dt0 warmStarting continuousPhysics subStepping stepComplete discreteIslands continuousIslands contactList contactsNonTOI contactsTOI` (E, in list/array order), broadphase `tree.root tree.nodeCount tree.nodeCapacity tree.freeList tree.path tree.insertionCount proxyCount pairBuffer.size moveBuffer queryProxyId` |
| `b2Body` | `rigidbody ground type flags islandIndex xf sweep v.* w force.* torque mass invMass axisConstraint.* I invI linearDamping angularDamping gravityScale sleepTime worldIndex fixtures contactEdges`, the Rigidbody2D's `rb.linearMove rb.angularMove rb.interpolating rb.linearTarget.* rb.angularTarget rb.movementIndex rb.parentDrivenBy` |
| `b2Fixture` | `collider massData density friction restitution filter isSensor shape.type shape.radius shape.count shape.geometry shape.flags proxies` ((childIndex, proxyId) pairs) `proxyAabbs` |
| `b2Contact` | `flags fixtureA fixtureB indexA indexB islandIndexA islandIndexB manifold manifold.ids manifold.type manifold.pointCount toiCount toi friction restitution tangentSpeed managerIndex userIndex` |
| `b2TreeNode` | one per node slot: `aabb fixture childIndex hasUserData parentOrNext child1 child2 height` |
| `PhysicsContacts2D` | `simulationId`, `collisions` (E → `Collision2D`, `m_Collisions` array order: the order of the step's callbacks) |
| `Collision2D` | `state contactCount colliderA colliderB rigidbodyA rigidbodyB receivingCollider keyA keyB enabled isTrigger flagForRecreate swappedReferences manifold.contacts manifold.ints manifold.floats` |

### Layout check

On the first recorded frame, every 500th and the last, `NativeState.Check` compares each layout read against the
managed API on every live object: vtables of the four behaviour managers, TimeManager, DelayedCallManager and
PhysicsManager2D; the five physics change handles distinct, in 0..63 and in the dispatch's in-use mask;
TimeManager clocks vs `Time.*`; GameObject instance id, layer, activeSelf; the Transform's TransformHierarchy slot
(its pointer back to the Transform, local position/rotation/scale; a `changeDispatchIndex` of -1, not queued, or
the dispatch's slot of that index holding the hierarchy (`TransformChangeDispatch.c:699-701` queues a hierarchy
with pending changes, `:1070-1081` dequeues it to -1); the index below its capacity);
component instance id and GameObject; Behaviour enabled; `m_IsAdded` vs `isActiveAndEnabled` (the getter reads the
same byte, so this pins the offset only) and, independently, `m_IsAdded` = enabled and active in hierarchy for every
Behaviour not being destroyed; MonoBehaviour
`m_AddedToManager`/`m_DidAwake` and each coroutine's owner; Rigidbody2D gravity scale, simulated, body type, and its
b2Body's user data, position, velocity, gravity scale, type, awake flag; Collider2D isTrigger, offset, density, box
size and edge radius, circle radius, edge radius, shape count, each fixture's user data, sensor flag and shape
type; one PhysicsScene2D; body count; each contact in exactly one of the contact arrays at its manager index; the
proxy count; `Collision2D` collider A has the lower instance id; every enabled MonoBehaviour that declares `Update`
in the Update manager's lists; the delayed-call queue sorted, of known callbacks, and of its stated size. Results
go to NOTEs, the log and the trailer (`native.checks`, `layout_mismatches`). A structural failure (vtables, change
handles, a hierarchy's dispatch slot, instance ids, frame count, an unreadable page) turns the native section off.

## Completeness: the sim's state and where it is recorded

The sim's state (`sim/fsm/fsm.h`, `sim/fsm/runtime/lifecycle.c`, `sim/phys/phys_internal.h`, `sim/hero/hero.h`,
`sim/core/rng.h`, `sim/core/sim.c`), field by field.

| sim state | recorded as |
|---|---|
| `go_inst` active_self, active_in_hierarchy, destroyed | `GameObject.activeSelf activeInHierarchy`; destroyed = the entity dies |
| `go_inst` parent, first_child, next_sibling | `GameObject.parent sibling childCount` |
| `go_inst` local_pos/scale/euler_z, world_pos, lossy_scale, world_euler_z, world_lin | `GameObject.lp.* ls.* lr.* leuler.z wp.* lossy.* wr.* euler.z` (the world matrix from `wr`/`lossy`) |
| `go_inst` vel, gravity_scale, kinematic, has_rb, body | `UnityEngine.Rigidbody2D.*`, `b2Body` |
| `go_inst` shapes_dirty; `fsm_world` dirty_bits, drain_bits (transform moved since the last shape flush, and the drain order) | the Transform's `n.physChanged` (per physics system, bits named by `PhysicsManager2D.handle.*`), set until `PhysicsManager2D::SyncTransforms` drains it, also for a moved-and-restored transform and a collider on the ground body; drain order = (`n.dispatchIndex`, `n.index`); a reparent: `PhysicsScene2D.rigidbodyHierarchyChanged` |
| `go_inst` fol_* (parent pose a nested body was synced against) | `b2Body.xf` vs the parent's `GameObject.wp/wr` |
| `go_inst` tag_override, layer_override | `GameObject.tag layer` |
| `go_inst` tink_ok_frame | `TinkEffect` fields (`nextTinkTime`) |
| `go_inst` alert_in_range, nonbouncer_active | `AlertRange` / `NonBouncer` fields |
| `go_inst` mesh_renderer_enabled | `UnityEngine.MeshRenderer.enabled` |
| `go_inst` dlg (PlayMakerUnity2DProxy delegate lists) | `PlayMakerUnity2DProxy` delegate fields (S of `Type.Method@target`) |
| `go_inst` lse, tt_* | `LimitSendEvents`, `TrackTriggerObjects` fields |
| `col_inst` enabled, is_trigger, offset, size, radius | `*Collider2D.enabled isTrigger offset size radius`, `b2Fixture.isSensor shape.*` |
| `col_inst` shape, body | `b2Fixture` (collider = iid; parent = its `b2Body`) |
| `col_inst` prev_tick, prev_rel | `HitboxObserver` is the mod's observer: its output is the OBS record in `.hktrace` |
| `fsm_inst` active_state, previous_state, switch_to, last_transition | `PlayMakerFSM.fsm.active fsm.previous fsm.switchTo fsm.lastTransition` |
| `fsm_inst` started, finished, active_state_entered, switched_state | `fsm.started fsm.finished fsm.activeStateEntered fsm.switchedState` |
| `fsm_inst` component_enabled, live, in_fsm_list | `PlayMakerFSM.enabled isActiveAndEnabled`; `static:PlayMakerFSM.fsmList` |
| `fsm_inst` vals, dangling | `FsmVariables@…` (dangling: fresh instances FsmVariables.cs:1195 creates are not stored in the FSM, so they are not state) |
| `fsm_inst` event_target | `fsm.eventTarget` |
| `fsm_inst` delayed (`delayed_ev`: event, target, timer, delay, fired, captured event data) | `fsm.delayed` → `DelayedEvent` |
| `fsm_inst` handle_fixed/late/2d | static configuration: `analysis/fsm` |
| `state_inst` active, finished, state_time, loop_count, max_loop_count_seen, active_action_index, active_action, active_actions, finished_actions | `FsmState.*` (every state) |
| `act_inst` enabled, active, finished, entered, private state (`st`) | `action:<type>.base.*` and the action's own fields (every action of every state) |
| `anim_inst` cur_clip, clip_time, clip_fps, previous_frame, state, enabled | `tk2dSpriteAnimator.currentClip clipTime clipFps previousFrame state enabled`, `tk2d.*` |
| `anim_inst` sprite_id, sprite_def | `tk2dSprite._spriteId collection` |
| `anim_inst` started, start_pending | `n.didStart`, a `DelayedCall` of `MonoBehaviour::DelayedStartCall` on it |
| `anim_inst` completed_kind/owner/event, triggered_* | `tk2dSpriteAnimator.AnimationCompleted AnimationEventTriggered` (the owner action by FSM, state and index) |
| `hm_inst` (hp, is_dead, invincible, invincible_from_direction, evasion_by_hit_remaining, direction_of_last_attack, stun control, death_event_sent, special death) | `HealthManager` fields; `EnemyHitEffectsUninfected.didFireThisFrame` |
| `hm_inst` bound, bound_max_hp, obs_max_hp | TrainingEnv / HitboxObserver bookkeeping (mod): `.hktrace` OBS |
| `dh_inst` damage_dealt, enabled | `DamageHero.damageDealt enabled` |
| `recoil_inst` | `Recoil` fields |
| `autorecycle_inst` timer, armed | `AutoRecycleSelf` fields, its `Coroutine` and the resume's `DelayedCall.time` |
| `constrain_inst` | `ConstrainPosition` fields |
| `grimmball_inst` force, tween_y, max_life, elapsed, shrink, phase | `GrimmballControl` fields and its coroutines' iterators |
| `flare_pillars` (Mecanim damage window) | the pillar's `UnityEngine.Animator` (`L0.hash normalizedTime`, parameters) and its FSM |
| `itween_inst` (id, events_id, alive, running, physics, reverse, looping, time, delay, running_time, percentage, from/to, ease, loop, kind, space, amount, last) | `iTween` fields (incl. `tweenArguments`), `iTweenFSMEvents` fields; `static:iTween` (`tweens`), `static:iTweenFSMEvents.itweenIDCount` |
| `pd` (PlayerData store) | `plain:PlayerData` (every field) |
| `fsm_list`, `gvals`, `event_registered`, `ev_reg`, `svars` | `static:PlayMakerFSM.fsmList`, `PlayMakerGlobals.vars`, `static:HutongGames.PlayMaker.FsmEvent._eventLookup`, `EventRegister` fields + statics, `static:StaticVariableList` |
| `stack`, `ev_*` (Fsm.EventData) | `static:HutongGames.PlayMaker.FsmExecutionStack`, `static:HutongGames.PlayMaker.Fsm.<EventData>` (→ `plain:…FsmEventData`) |
| `dt fixed_dt frame time phase` | `Time.*`, `TimeManager.*` (the double clocks), FRAME header |
| `is_boss_scene bosses_dead_signal scene_left boss_level can_transition` | `BossSceneController` fields and statics |
| `hero_box_inactive` | `static:HeroBox.inactive` |
| `agent_step`, `n_bound_bosses`, `damage_landed_step` | `Env.step`; the rest is TrainingEnv bookkeeping (mod): `.hktrace` |
| `lc_comp` enabled, in_lists, awake, started, start_queued, dead, iid | `enabled`, `isActiveAndEnabled` (`m_IsAdded`)/`BehaviourManager` lists, `n.didAwake`, `n.didStart`, `DelayedStartCall` entries, death, `iid` |
| `lc_comp` serial (enable order, R3) | position in `BehaviourManager.q<order>.active/add` |
| `lc_state` lists (Fixed/Update/Late by execution order) | `BehaviourManager` `FixedUpdate Update LateUpdate` |
| `lc_state` pend (Start queue) | `DelayedCall` with `call = MonoBehaviour::DelayedStartCall`, in queue order |
| `lc_coro` (kind, comp, acc/wait, frame, stage, fixed_seq) | `n.coroutines` → `Coroutine` → iterator state; the resume key `DelayedCall.time/frame/mode` |
| `lc_destroy_req` (go, delay, acc) | `DelayedCall` with `call = DelayedDestroyCallback` (`time` = due key) |
| `lc_state` frame, fixed_seq | FRAME header, `Env.fixedCount` |
| `hero.f` (HeroController fields) | `HeroController` fields |
| `hero.cs` | `plain:HeroControllerStates` |
| `hero.pd` | `plain:PlayerData` |
| `hero.in` (InputManager tick, device controls, DPad, PlayerActions, moveVector, shim keys and commit state) | `static:InControl.InputManager` (`currentTick`, `devices` → `plain:HKOracle.Game.InputDeviceShim` and its controls), `InputHandler.inputActions` → `plain:HeroActions` and its `PlayerAction`s |
| `hero.co` (recoil, invulnerable, thunk, die, hazard-respawn coroutines) | the Knight's `HeroController.n.coroutines` → `Coroutine` iterators; their `DelayedCall`s; `GameManager` coroutines |
| `hero.slash[]`, `slashComponent` | `NailSlash` fields per slash object; `HeroController.slashComponent` |
| `hero.box` | `HeroBox` fields |
| `hero.anim` | `HeroAnimationController` fields |
| `hero` clocks, gm_*, bsc_* | `Time.*`; `GameManager` fields (`isPaused` …); `BossSceneController` |
| `hero` col_offset/size/edgeRadius, hazardRespawnLocation, artCharge*Active | the Knight's `BoxCollider2D`; `PlayerData.hazardRespawnLocation`; the objects' `activeSelf` |
| `body_t` alive, simulated, awake, type, cd, p, c, c0, lc, alpha0, q, rot_deg, v, gravity_scale, mass, inv_mass, force, layer, island | `b2Body` (`flags`: awake 2, bullet 8, active 32; `xf`, `sweep`), `Rigidbody2D.*`, `GameObject.layer` |
| `shape_t` alive, enabled, is_trigger, type, body, user, layer, offset, size, radius, edge_radius, points, pieces | `b2Fixture` (one per piece), `*Collider2D`, `GameObject.layer` |
| `shape_t` bake_gen | recreate = fixture and contact deaths and births |
| `contact_t` seq, sa/sb, pa/pb, ba/bb, m, touching/enabled/filter/island, toi_flag/toi/toi_count, friction, restitution | `b2World.contactList` order, `b2Contact.*` (`flags`: touching 2, enabled 4, filter 8, toi 32, toi candidate 128) |
| `contact_t` touch_start, detect_order; `ended`, `events` | step-local; their outcome is `Collision2D.state` and the callback order `PhysicsContacts2D.collisions` |
| `phys_world` gravity, inv_dt0, step_complete, new_fixture | `b2World.gravity inv_dt0 stepComplete flags` |
| `phys_world` vel_iters, pos_iters, layer_mask | static configuration: `dumps/<scene>/scene.json` |
| `hk_rng` x, y, z, w | `Random.state` |
| `hk_rng` mode, occurrence counters, oracle table | the gate's replay machinery, not game state |
| `hksim` frame, fixed_count, step, time, fixed_time, tsll | FRAME header, `Time.*`, `Env.*` |
| `hksim` obs scratch, terrain rows | the observation: `.hktrace` OBS |

Not recorded: rendering, audio and UI beyond `enabled`/`isPlaying` (out of scope); the Animator's native graph
(its controller state is read through the managed API); the mod's own objects (the env, the observer, the
recorders), whose effects are the `.hktrace` records.
