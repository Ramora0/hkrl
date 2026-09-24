# Engine lifecycle

This file gives Unity's component lifecycle and PlayMaker execution order as `sim/fsm/runtime/lifecycle.c`
implements them. The rules R0–R7 are ports of Unity's native player loop, read from UnityPlayer.dll decompiled with
its PDB: `hksim/analysis/decomp_native/src/playerloop` and the checked spec
`hksim/analysis/native_specs/native-playerloop.md` (PL-n sections). The code and this file cite native functions as
`UP!<addr>`. Each rule also names the measurements that check it. Where the native code cannot settle a question
(the history of the dump, PlayMaker's own C#), an assumption A-n fills the gap. §4 lists where the sim still differs.

Evidence comes from three sources:
- **The native decompile.** It defines the rules. native-playerloop.md reads every ordering and float claim that
  decides a rule against the disassembly, and a second agent checked it (its `## Check` section).
- **Fight recordings.** `analysis/lifecycle/rules.json`, extracted by `tools/lifecycle_rules.py` from
  `analysis/lifecycle/<scene>/*.lifecycle.gz`. That corpus covers 38 recorded fights in GG_Hornet_1,
  GG_Ghost_Xero, GG_Mega_Moss_Charger, GG_Nosk and GG_False_Knight, all at regime R2. Support counts below are
  from that corpus. `tools/lifecycle_compare.py` replays the same episodes through the sim
  (`HKSIM_LIFECYCLE_LOG`) and compares the two logs up to the first gameplay divergence.
- **Conformance probes** (§3). Small synthetic scenarios run inside the real game and mirrored in the sim, one
  engine rule each. They are cited by scenario name, e.g. `[b_reparent]`.

## 1. The recorder and the stage names

`oracle/Record/LifecycleRecorder.cs` records when `HK_ORACLE_LIFECYCLE=1` and `HK_ORACLE_TRACE` are set. It
writes `<trace base>.lifecycle.gz` once per episode. The format is documented in the recorder's `Flush` and
in `tools/lifecycle_rules.py`.

- **Entry hooks** are on every declared `Awake`, `OnEnable`, `Start`, `FixedUpdate`, `Update`, `LateUpdate`,
  `OnDisable`, `OnDestroy`, `OnTrigger*2D`, `OnCollision*2D` and `OnBecame*`. They cover every MonoBehaviour
  in Assembly-CSharp, Assembly-CSharp-firstpass and PlayMaker, plus `MoveNext` of their coroutine iterators.
  They resolve the most-derived declaration per runtime type, because Unity calls only that one.
- **Return hooks** on Awake/OnEnable/OnDisable/Start/OnDestroy give the nesting.
- **A marker before each of the 131 native PlayerLoop subsystems** attributes every event to a stage.
- **Delimiting markers** around `ObjectPool.Spawn`/`Recycle` and PlayMaker
  `ActivateGameObject.DoActivateGameObject`/`OnExit` bound "one activation".
- **FsmPauseGate:** the dispatches that `oracle/Game/FsmPauseGate.cs` swallows while `timeScale <= 0` are
  recorded with `aux = 1`.
- **Inertness:** with the recorder on, every observation from step 2 on is identical to a run with it off.
  So are every gameplay FSM event and every RNG draw. Every on/off difference also occurs between two
  recorder-off runs.

**Stage names** (also `lifecycle.h` `LCS_*`). The delayed stages are passes of Unity's `DelayedCallManager` (R1,
R6, R7); the pass reads one clock and runs the calls whose mode shares a bit with its mask:

| name | native stage | delayed-call pass: mask, clock |
|---|---|---|
| `load` | EarlyUpdate/UpdatePreloading | |
| `startup` | EarlyUpdate/ScriptRunDelayedStartupFrame (UP!0x180765c30) | startup (4), `Time.time` |
| `fixed` | FixedUpdate/ScriptRunBehaviourFixedUpdate | |
| `physics` | FixedUpdate/Physics2DFixedUpdate | |
| `fixed_delayed` | FixedUpdate/ScriptRunDelayedFixedFrameRate (UP!0x180765cf0) | fixed (1), `Time.fixedTime` |
| `update` | Update/ScriptRunBehaviourUpdate | |
| `update_delayed` | Update/ScriptRunDelayedDynamicFrameRate (UP!0x180766110) | dynamic (2), `Time.time` |
| `anim` | PreLateUpdate/DirectorUpdateAnimationBegin + DirectorUpdateAnimationEnd (the Animators, `mecanim.c`) | |
| `late` | PreLateUpdate/ScriptRunBehaviourLateUpdate | |
| `postlate_delayed` | PostLateUpdate/ScriptRunDelayedDynamicFrameRate (UP!0x180766110, the same function) | dynamic (2), `Time.time` |
| `end_of_frame` | PostLateUpdate/PlayerSendFrameComplete (UP!0x180765ed0) | end of frame (32), `Time.time` |

**Script execution orders** of the simulated types, from the game build (any type not listed is 0):
tk2dSpriteAnimator −30095, InControlManager −100, HeroController 208, NailSlash 500, HeroBox 750. PlayMakerFSM,
PlayMakerFixedUpdate, PlayMakerLateUpdate, HealthManager, Recoil, iTween and the other simulated components
are 0.

## 2. Rules

### R0. Frame shape and clocks
- `PlayerLoop` (UP!0x18075c4c0) runs the stages in the order of §1. The FixedUpdate group loops while
  `TimeManager::StepFixedTime` (UP!0x18052c7f0) returns true: while `Time.time ≥ Time.fixedTime + fixedDeltaTime`.
- `TimeManager::Update` (UP!0x18052c970) adds `(double)(captureDeltaTime · timeScale)` to `Time.time`, as a
  **double**. It adds 0 on a frozen frame (`timeScale` 0). A fixed step adds `(double)fixedDeltaTime` to
  `Time.fixedTime`. `Time.frameCount` advances on every frame, frozen ones included.
- Under R2 both steps are `(double)0.02f`. So the gap `r = Time.time − Time.fixedTime` never changes. Every live
  frame runs exactly one fixed step and every frozen frame none (70,360/70,360 env resumes in `update_delayed`).
- `r` is a per-run constant in `[0, 0.02)`, left over from boot. It is not 0: the polbat traces show 0.00469,
  0.01778, 0.00822. In the probes it splits the game processes in two (§3). Only a timed `Destroy` depends on it (R7).
- `SetTimeScale` (UP!0x18052c4c0) only stores the value, so a `timeScale` written mid-frame takes effect next
  frame. `anim` advances every Normal-mode Animator by the frame's `Time.deltaTime` as sampled at frame start (0.02
  live, 0 frozen), then writes its bound properties (native-animator.md N-AN-1, N-AN-2).
- **The sim.** `lifecycle.c` keeps `Time.time` and `Time.fixedTime` as doubles (`lc_frame_begin`), for the delayed
  calls' keys. `lc_set_clock` sets the time and `r`. hksim carries no `fixedTime`, so a scene starts at
  `(time0, r = 0)`, the limit of a small gap (§4). The conformance harness passes each game process's `r`.

### R1. When Start runs (support 6,100)
Every enable queues a `DelayedStartCall` (UP!0x1808ac680). `MonoBehaviour::AddToManager` (UP!0x1808ab340) queues it
before anything else, with key `min(Time.fixedTime, Time.time) − 10` and mode startup|fixed|dynamic. The call runs
in the first delayed pass that begins after the enable, before everything else in that pass. `CallUpdateMethod`
(UP!0x1808ac150) runs a missing Start just before a component's first FixedUpdate, Update or LateUpdate. Whichever
comes first wins.

| enabled during | Start runs in |
|---|---|
| `physics` (a trigger/collision callback) | same frame `fixed_delayed` |
| `fixed` | same frame `fixed_delayed` `[a_enable_fixed_live]` |
| `update` | same frame `update_delayed` |
| `late` | same frame `postlate_delayed` |
| `update_delayed` | same frame `postlate_delayed`, or `late` just before the first LateUpdate if the type has one |
| `fixed_delayed` | same frame `update_delayed`, or `update` just before the first Update if the type has one |
| `startup` | same frame `fixed_delayed`; `update_delayed` on a frozen frame (no fixed step) |
| `load` (scene load) | same frame `startup` |
| `postlate_delayed` | next frame `startup` |
| `anim` (an Animator's `m_IsActive` write) | same frame `late` just before the first LateUpdate if the type has one, else same frame `postlate_delayed` (A-25) |

- **Inserted this pass, run next pass.** A pass skips the calls inserted during itself (the timestamp gate), so an
  enable inside a delayed pass gets its Start in the next one.
- **The call checks, it does not cancel.** `DelayedStartCall` runs Start only if the component is still added
  and not started (`m_AddedToManager && !m_DidStart`). A component disabled before its call loses its Start until
  its next enable queues another `[a_start_dropped, a_start_requeue]`. A disable plus re-enable before the call
  leaves two calls: the older one starts it, at its own position, and the newer does nothing.
- **Start comes first.** Its key is 10 s below every other key, so inside `update_delayed` every pending Start runs
  before every coroutine resume, the env coroutine included (1,079/1,079).
- **Once per component.** `m_DidStart` is never cleared. A `Start` called explicitly by game code
  (`SpriteFlash.Start`, SpriteFlash.cs:232) is not engine dispatch.

### R2. First tick after an enable (support 23,237, exceptions 0)
`BaseBehaviourManager::CommonUpdate` (UP!0x180626ba0) appends the components added since its last pass
(`IntegrateLists`, UP!0x18062a5b0) once, when the pass begins. So a component's first callback of each kind comes
at the next run of that stage that starts after the enable.

- **Update:** same frame if enabled in `physics`, `fixed_delayed`, `startup` or `load`. Next frame if enabled
  in `update` (8,234/8,234: never in the pass that enabled it), `update_delayed`, `late` or `postlate_delayed`.
- **LateUpdate:** same frame if enabled in `update`, `physics`, `update_delayed` or `load`. Next frame if
  enabled in `late` or `postlate_delayed`.
- **FixedUpdate:** the next fixed step. Because frozen frames run no fixed step, that is 1 or 2 frames later.
- A component disabled before its turn in a pass is skipped. So is one disabled and re-enabled before its turn:
  it is back in the add list.

### R3. Order within FixedUpdate, Update and LateUpdate (40,200,997 adjacent pairs, 0 violations)
`BaseBehaviourManager` keeps one list per script execution order (`std::map<int, list>`, walked in ascending
order) for all types. `AddBehaviour` (UP!0x180628060) unlinks the component and appends it at the tail. So within
one order the tie-break is **most recent enable last**, and a disable + re-enable moves a component to the tail of
its bucket. The Update list, the FixedUpdate list and the LateUpdate list run the same code. Membership depends only
on whether the type declares the method (`AddBehaviourCallbacksToManagers`, UP!0x1808ab000), not on OnEnable or
OnDisable.

Measured: Update 717,546/0, LateUpdate 67,228/0, FixedUpdate 8,425/0 on types that declare both OnEnable and
OnDisable `[c_tick_order, c_fixed_reenable]`. So HealthManager, Recoil and PlayMakerFixedUpdate interleave with the
PlayMakerFSMs by enable order, and PlayMakerFixedUpdate/Recoil (0) run before HeroController.FixedUpdate (208),
which runs before NailSlash (500). The tie-breaks tested and rejected: instance id, hierarchy pre-order and
creation order.

### R4. Activation: SetActive, spawn, Instantiate, reparenting
- **The walk.** `GameObject.SetActive` (`SetSelfActive`, UP!0x180580c90) writes `activeSelf` first and then runs
  `ActivateAwakeRecursively` (UP!0x18057dac0). So does `Transform.parent =`: `Transform::SetParent`
  (UP!0x1807e9f80) ends in `TransformParentHasChanged` (UP!0x180580e70). Moving an active object under an inactive
  parent deactivates it, and moving it under an active parent activates it `[b_reparent, b_spawn]`.
  `ActivateAwakeRecursivelyInternal` (UP!0x18057db40) visits each object as follows:
  1. It recomputes the object's cached `activeInHierarchy`.
  2. It recurses into the children in child order.
  3. Only then, if the object's state changed, it handles the object's components in component order.
  The sim's walk is `lc_go_active_changed`.
- **Deactivation** runs OnDisable depth-first **post-order**: children before the parent, siblings in order, one
  object's components in component order. Execution order is not used (1,063/1,063).
  - Each MonoBehaviour's coroutines stop just before its OnDisable (`MonoBehaviour::Deactivate`, UP!0x1808ac610).
  - `RemoveFromManager` (UP!0x1808b05e0) takes the component out of the dispatch lists before its OnDisable runs.
- **Flags inside OnDisable.** Each object's cache is recomputed on entry, before its children. So inside
  OnDisable the deactivated object reads `activeSelf` and `activeInHierarchy` false. Its children read `activeSelf`
  true and `activeInHierarchy` false `[b_setactive_tree, g_flags, b_nested_disable]`. A sibling subtree the walk
  has not reached yet still reads its old `activeInHierarchy`. A `Behaviour.enabled = false` reads `enabled` false
  and both active flags true: `SetEnabled` (UP!0x18062ad80) writes `m_Enabled` first.
- **Activation** queues the whole activated subtree and then runs `AwakeFromLoadQueue::AwakeFromLoad`
  (UP!0x180906060).
  - The MonoBehaviour queue is sorted by ascending execution order, then **descending** instance id (UP!0x18090a610;
    5,080/5,080 delimited groups; hierarchy pre-order fails `[b_setactive_idorder]`). Runtime ids fall with
    creation (R4 ids below), so this is creation order.
  - Each component then gets `MonoBehaviour::AwakeFromLoad` (UP!0x1808ab4f0). If it is enabled and its object is
    still active, it gets `AddToManager`. Otherwise, if its object is active and it never Awoke, it gets Awake
    alone. So a disabled component Awakes with its object, and gets its OnEnable only when it is enabled
    `[a_behaviour_enable]`.
- **AddToManager's order** (UP!0x1808ab340):
  1. queue the DelayedStartCall;
  2. join the dispatch lists;
  3. Awake, if it never ran;
  4. OnEnable.

  So each component's Awake is immediately followed by its own OnEnable. An enable nested inside another
  component's Awake or OnEnable lands after it both in the Update order and in the Start queue
  `[b_nested_enable]`.
- **Scene load** (`PersistentManagerAwakeFromLoad`, UP!0x180909890) sorts every queue by **ascending** instance id
  alone (UP!0x18090a690). It does not consult execution order. The 18/18 measured agreement with "execution order,
  then ascending id" holds because the one nonzero-order scene type (tk2dSpriteAnimator) has ids below the FSMs'.
- **Colliders** of the whole activated subtree join physics before the first OnEnable. The Rigidbody2D queue (11)
  and the Collider2D queue (13) run before the MonoBehaviour queue (20). On deactivation a Collider2D leaves at its
  own component position (`Collider2D::Deactivate`, UP!0x180c02500), after its children's OnDisable
  `[g_query_in_callbacks, g_flags]`. The sim adds an object's body and colliders when the walk enters the object,
  and removes them before its scripts (A-6).
- **Instance ids** (PL-10). `AllocateNextLowestInstanceID` (UP!0x1805791d0) hands every runtime object the next id
  **down by 2**. `CollectAndProduceGameObjectHierarchy` (UP!0x1806294f0) clones in transform pre-order: each object,
  then each of its components in component order. So on a clone, component `j` has id `go − 2(j+1)`: 203/203
  instantiated objects with FSMs in the dumps. Scene objects have positive ids assigned at load. Components
  added at runtime (PlayMaker's proxies, iTween) take whatever the counter holds. The sim's ids are A-1.

### R5. Physics callbacks
- Enter and Stay run only in `physics`, after every FixedUpdate of the step.
- **Order within a step:** Unity walks its touching contacts in the order they **began touching**, oldest
  first, and delivers Stay, or Exit in place for one that stopped. Contacts that begin touching in this step
  follow, with Enter; of those, a pair created in a later step comes first `[f_stay_vs_enter, f_staggered,
  f_touch_vs_creation, f_same_step_new_touch, f_enter_same_step_*, f_two_receivers_staggered]`. The contact
  list is also what `Collider2D.GetContacts` returns (`watch` samples, compared between game processes only).
- **Pairs created in the same step** (one broadphase batch) come in an order set by the broadphase's proxy ids.
  Box2D hands those out from a LIFO free list (`b2DynamicTree::AllocateNode`/`FreeNode`,
  `analysis/upstream/box2d-v2.3.1/Box2D/Box2D/Collision/b2DynamicTree.cpp:53-99`), so they depend on what the
  scene freed before.
  - Without a drain, `f_two_receivers` flipped in 1 of 16 quiet-world processes (`analysis/conformance/run2`,
    rep 2). `f_sleep`'s two pairs swapped once it had a drain, and `f_same_batch` (no drain) swapped once the
    drained scenarios before it had changed the free list.
  - With a drain (§3) the ids ascend with fixture creation, and every process agrees: R2's pair before R1's when
    R1 is created first, R1's when R2 is `[f_two_receivers, f_two_receivers_swapped]`, and A's before B's
    `[f_same_batch_drain]` (`analysis/conformance/drain_contrast` has each case with and without a drain).
  - These orders fit "the order the step's broadphase queries find the pairs": each moved proxy queries the tree
    in move-buffer order, a pair of two moved proxies is found by the later one's query, and the tree is walked
    second child first (`b2DynamicTree.h:172-197` pushes child1, then child2). That reading is a hypothesis,
    because the queries are not observable.
- **The sim's model** is Unity's list of touching collider pairs (`PhysicsContacts2D::m_Collisions`,
  `analysis/specs/port-phys.md` U4): trigger reports precede collision reports, each group walks the list in
  the order the pairs began touching, and a pair that exits hands its slot to the newest pair.
- **Order within a pair:** the collider with the **lower instance id** receives first, whatever its role
  (trigger, static, solid, dynamic) and whichever fixture is newer.
  - Probes: runtime ids fall with creation, so the later-created collider hears first `[f_receiver_*]`.
    Activating the collider that was created first later gives it the newer fixture and, with the drain, the
    higher proxy id; it still hears second `[f_receiver_act_solid_first, f_receiver_act_trigger_first]`. So the
    order is not the Box2D fixture / proxy order.
  - Fight corpus: the Knight's colliders exist before the boss scene loads, have lower ids than the boss
    scene's colliders, and hear first. Lower id first holds in 120,337 of 120,381 contacts in the 38 fights, and
    the GameObject ids give the same order in all of them (`tools/receiver_order.py`). So it is not "later
    created first" either. The 44 exceptions that were inspected are two contacts the pairing cannot tell apart:
    a callback forwarded to a parent Rigidbody2D's object that also has a collider of its own, or a compound
    PolygonCollider2D reporting twice in one step.
- **Exit fires synchronously** when a collider is disabled (`Collider2D.enabled`), its object is deactivated or
  destroyed, to both colliders of the pair, in the caller's stage `[f_exit_col_enabled_*, f_exit_set_active_*,
  f_exit_destroy_*]`. `Rigidbody2D.simulated = false` ends the contact at the next step instead
  `[f_exit_simulated_*]`. The sim delivers the synchronous Exits through `phys_take_exit_events`
  (`analysis/specs/port-phys.md` U7, A-12). `Behaviour.enabled = false` on the receiver changes nothing: disabled
  MonoBehaviours still receive physics messages `[f_exit_behaviour_*, f_disabled_receiver]`.
- A collider disabled and re-enabled in one frame gets its Exit at once and a new Enter at the next step
  `[f_reenable_same_frame, f_fixture_recreate]`. A facing flip, a scale write and `BoxCollider2D.size` keep the
  contact: no Exit, no Enter. So does an `isTrigger` toggle while the partner is a trigger, since the pair stays
  a trigger pair `[f_fixture_recreate]`. An `isTrigger` toggle that turns a kinematic trigger against a
  kinematic solid into two kinematic solids ends the contact with an Exit in that step's `physics`, and toggling
  back begins a new one with an Enter in that step `[f_trigger_toggle_pair_kind]`.
- Trigger pairs report with a kinematic or static partner: kinematic trigger vs static or kinematic collider,
  static trigger vs dynamic or kinematic body, trigger vs trigger. A kinematic solid vs a static solid reports
  nothing, and neither do two kinematic solids `[f_pair_*, f_trigger_toggle_pair_kind]`.
- A sleeping body gets no Stay; Stay resumes when it wakes, without Exit or Enter. A resting dynamic body with
  `StartAwake` falls asleep after 25 steps (`timeToSleep` 0.5 s) `[f_sleep]`. With the boss active
  (`analysis/conformance/run1`) one game process of three kept both bodies awake throughout. `sim/phys` does
  not port sleep (`analysis/specs/port-phys.md` Q-pphys-8, CF-13).
- A **Discrete** Rigidbody2D gets no continuous collision, not even against static or kinematic bodies: at 0.6
  per step it passes through a 0.1 thick wall, and a solid approach reports Enter at the first step whose
  start pose touches `[f_tunnel_discrete, f_pair_kin_solid_vs_dyn, f_trigger_and_collision]`. A Continuous one
  stops at the wall `[f_tunnel_continuous]`. The watched positions are compared with the sim.
- **Teleports** (`Rigidbody2D.position` or `transform.position`, with the same timing): a body teleported into
  a trigger gets its Enter at the first step after the write. That is the same step when the write is in
  FixedUpdate, and the next frame's step when it is in Update. Teleported into a static solid, it gets its
  Enter one step later than that. Teleporting out of the trigger ends the contact at that same first step
  `[f_teleport_{rigidbody,transform}_{fixed,update}]`.
- Some contacts end with no Exit in the fight corpus (214 against 17,469 Exits).

### R6. Delayed calls and coroutines
One queue holds every pending Start, coroutine resume and Destroy, and the env coroutine: `DelayedCallManager`
(native-playerloop.md §4).
- **Insert:** `CallDelayed` (UP!0x180628490) keys a call on a double time.
  - The key is `(double)delay` plus the base clock. The base is `Time.fixedTime` for mode fixed,
    `min(Time.fixedTime, Time.time)` for mode dynamic, and the active clock with neither.
  - With kWaitForNextFrame, the call waits for `frameCount + 1`.
  - The multiset insert (UP!0x180627ca0) puts a new call after every call with an equal key: FIFO.
- **Pass:** `DelayedCallManager::Update` (UP!0x18062ae00) reads `now` once. It walks the queue in key order until
  the first key above `now`. It runs each call whose mode shares a bit with the pass's mask, whose frame gate is
  reached, and which an earlier pass inserted (the timestamp). Each call is removed before it runs.
- **Yields** (`ProcessCoroutineCurrent` UP!0x1808a7ab0, `HandleIEnumerableCurrentReturnValue` UP!0x1808a72c0):

  | yield | key | runs in |
  |---|---|---|
  | `null`, any other object | `Time.time` | a dynamic pass (`update_delayed`) of a later frame |
  | `WaitForSeconds(s)` | `Time.time + (double)s` | the first dynamic pass whose `Time.time` reaches it |
  | `WaitForFixedUpdate` | `Time.fixedTime` | the next `fixed_delayed`; the same step's when yielded in `fixed` or `physics` |
  | `WaitForEndOfFrame` | active clock − 1 | the next `end_of_frame`; the same frame's when yielded before it |

- The first `MoveNext` runs inside `StartCoroutine`, in the caller's stage. Because of the frame gate, the first
  `yield null` / `WaitForSeconds` resume is in a later frame `[d_yields_from_*]`.
- `WaitForSeconds` compares absolute double time, and frozen frames add no time `[d_frozen]`. So 0.4 s, 0.5 s and
  1.3 s take 21, 26 and 65 frames, where a float sum of dt takes 20, 25 and 66.
- **Resume order** within a pass is by key, FIFO within a key. That is the order the routines last yielded. A
  routine started inside another's resume, or one that waited on `WaitForFixedUpdate` in between, can overtake an
  older one `[d_nested_start, d_requeue, d_fifo]`.
- **The env coroutine** (TrainingEnv's step loop; the probes' driver) is an ordinary `yield return null` in the
  queue. Its key is the previous frame's time, so:
  - a `yield null` from the previous frame's passes runs before the env if it was inserted before the env's own
    re-yield, and after it otherwise;
  - a `yield null` from this frame's physics or FixedUpdate waits one more frame, and then runs before the env;
  - a `WaitForSeconds` that comes due runs after the env has written FRAME / OBS.

  `lc_stage(update_delayed)` stops at the env. The core runs it, and `lc_delayed_end` re-inserts its yield and
  finishes the pass.
- **Cancellation.** Deactivating the object stops its coroutines, just before each component's OnDisable
  (`MonoBehaviour::Deactivate`, UP!0x1808ac610). Disabling one Behaviour does not. `StopAllCoroutines` does
  `[d_cancel]`.

### R7. Destroy
`Object.Destroy(obj, t)` is `Scripting::DestroyObjectFromScripting` (UP!0x1808c27e0).
- **At the call, `t ≤ 0`:** `Scripting::DisableBehaviours` (UP!0x1808c2b10) runs `SetEnabled(false)` on every
  Behaviour of the object and of its **direct** children, in the caller's stage.
  - Colliders are Behaviours: their fixtures go, and their Exits fire now (UP!0x180c07f70).
  - Each OnDisable reads `enabled` false and both active flags true. The disabled components tick no more.
  - Deeper descendants keep running, and coroutines keep resuming, until the destroy pass
    `[e_destroy_stages, e_destroy_in_callback, g_flags, f_exit_destroy_*]`.
- **The destroy pass:** `DestroyObjectDelayed` (UP!0x180629da0) queues the destroy with key
  `min(Time.fixedTime, Time.time) + t = Time.fixedTime + t`, in mode fixed|dynamic. So it runs in the first
  `fixed_delayed`, `update_delayed` or `postlate_delayed` pass that begins after the call and whose clock has
  reached the key. `startup` is not one of them.
  - `Destroy(obj)` in `physics` or `fixed` runs in the same step's `fixed_delayed`.
  - In Update it runs in the same frame's `update_delayed`, after the env on a live frame and before it on a
    frozen one.
  - In LateUpdate, or in a coroutine in `update_delayed`, it runs in the same frame's `postlate_delayed`.
- **What the pass does** (`DestroyObjectHighLevel_Internal`, UP!0x18074e7f0):
  1. `GameObject::Deactivate` (UP!0x18057ef70): `activeSelf` false, then R4's post-order OnDisable walk for
     whatever is still enabled.
  2. `PreDestroyRecursive` (UP!0x18074fee0): OnDestroy, pre-order, each object's components in component order,
     for every component that Awoke.
  3. The object is gone: it compares `== null`.

  A component that never Awoke gets no callback.
- **`Destroy(obj, t > 0)`** does nothing until its key `Time.fixedTime + t` is reached. `fixed_delayed` compares
  `Time.fixedTime`, and the dynamic passes compare `Time.time = Time.fixedTime + r`. So the frame it fires in, and
  whether that is `fixed_delayed` or `update_delayed`, depends on the process's gap `r`. `Destroy(obj, 0.05)` from
  an Update fires two live frames later in `update_delayed` when `r ≥ 0.01`, and three live frames later in
  `fixed_delayed` otherwise. The probe processes split exactly so (§3) `[e_destroy_delayed*]`.
- `Destroy(component)` disables the component at the call and runs its OnDestroy at the pass
  `[e_destroy_component]`.
- Scene unload runs OnDestroy at `load`, usually preceded by OnDisable in the same pass.

## 3. Conformance probes

`oracle/Probe/` is a launch mode of the mod (`HK_ORACLE_PROBE`, `docs/oracle.md`).
- After the boss scene loads (regime R2), it deactivates the boss. It builds each scenario of
  `hkpy/conformance_scenarios.py` from plain GameObjects, Rigidbody2D / BoxCollider2D and probe MonoBehaviours, on
  a private layer far from the arena.
- It runs each scenario for its frame shape (live and frozen frames, as the env steps).
- The probes log every callback with its stage (the recorder's player-loop markers,
  `oracle/Record/LoopMarkers.cs`). Ops fire from probe callbacks or from a driver's hook in each stage.

`tools/conformance.py run` records the scenarios in N game processes into `analysis/conformance/<run>/` and
checks that the processes agree. `tests/test_conformance.py` runs every scenario through the sim's own lifecycle
and physics (`hkpy/conformance.py` over `sim/fsm/runtime/conformance_api.c`). It compares the two logs event by
event, and the watched bodies' positions too. A scenario the sim does not reproduce is a strict xfail naming its CF
entry (§4). The sim's colliders take the game's instance ids: a scenario object's collider takes its GameObject's
logged id (only their order matters: runtime ids fall with creation).

**The clock gap.** The probe logs do not record a process's `r` (R0). Only the timed Destroys depend on it
(`test_residual_only_times_destroys`), so they run over a grid of `r` in `[0, 0.02)`, and every other scenario runs
at `r = 0.01`. One `r` reproduces all three timed-Destroy scenarios of a process (`test_residual_one_per_process`):
`r < 0.01` for process 0 of `2026-09-23-drain`, `r ≥ 0.01` for processes 1–3. No process fits every `r`.

Every physics scenario except `f_same_batch` starts with a **drain**: 400 static colliders, created before the
scenario's objects and never destroyed. They take every broadphase node the scene freed before, so the
scenario's proxy ids come from the tree's never-used tail, which `AllocateNode` links in ascending order.
Without a drain, the order of pairs created in the same step depends on the scene's history (R5).

## 4. Where the sim differs

Measured against `analysis/conformance/2026-09-23-drain` (101 scenarios, 4 game processes). Every process logs the
same events and watch samples, except in the `Destroy(obj, t)` scenarios (their `r`, §3).

| id | the game | the sim | where | scenarios |
|---|---|---|---|---|
| CF-13 | sleeping bodies get no Stay (R5) | no sleep (Q-pphys-8) | `sim/phys` | `f_sleep` |

The port of the native player loop fixed CF-1 to CF-7 and CF-14, and each of those scenarios now matches:
- CF-1: the flags inside OnDisable;
- CF-2: a synchronous Destroy;
- CF-3: the timing of a timed Destroy;
- CF-4: the Awake of a disabled component;
- CF-5: the nested Start order;
- CF-6: the resume order;
- CF-7: reparenting;
- CF-14: `Destroy(component)`.

`sim/phys`'s port of Unity's contact pipeline fixed CF-8 to CF-12, CF-15 and CF-16 (`analysis/specs/port-phys.md`
U4, U6-U8). Everything else the scenarios exercise matches as well.

The native code shows more differences that no probe covers:
- **HeroController's coroutines** keep their own schedule (`sim/hero`: `hero_coroutine_phase`, run just before the
  env; `hero_coroutine_after_env`, just after). They are not in the delayed-call queue. So three native rules do
  not reach them yet:
  - the invulnerability clear after `WaitForSeconds(1.3)`, which should run after the env (B17, native-playerloop.md
    §7);
  - the `WaitForSeconds` tick counts of the hazard respawn: 2.6 s is 131 frames as a float sum and 130 by the double
    rule (NB-2);
  - the key order of its resumes against the lifecycle's (NB-3).
- **hksim has no `r`.** `hksim_config` carries `time0` only, so every scene runs at `r = 0` (R0). A timed Destroy
  in a gameplay FSM fires as it would in a process with a small gap. A replay of a recording would need the
  trace's `time − fixed_time` (`lc_set_clock`).
- **Component instance ids** are synthetic (A-1). The clone rule `go − 2(j+1)` needs each component's index in
  `m_Component`, which the generated tables do not carry. So the activation order among one clone's components
  follows A-1's per-type placement.
- **Collider order within an activation or deactivation.** Native adds all the subtree's bodies (queue 11), then
  all its colliders (queue 13) in post-order, and removes a collider at its own component position. The sim adds
  each object's body and colliders when its walk enters it, and removes them before its scripts (A-6). This order
  sets broadphase proxy ids (R5), and on deactivation it decides whether an Exit comes before or after a script's
  OnDisable on the same object.
- **EnemyHitEffectsUninfected.Update** has its own list node natively. The sim runs it in its HealthManager's
  slot (A-8).

## 5. Assumptions

Each assumption fills a gap that neither the native code nor a measurement closes. Code cites them as `A-n`. The
last column says what settles or would settle it. Rows marked **native** are now rules (§2) and are kept for the
code's and older documents' citations.

| id | assumption | status |
|---|---|---|
| A-1 | **Synthetic instance ids.** PlayMakerFSM and GameObject ids are the dumped ones. Other components get an id placed where the recordings put their type relative to the object's FSMs. Scene objects: HealthManager / Recoil / LimitSendEvents / ConstrainPosition / AutoRecycleSelf / GrimmballControl above the highest FSM id, tk2dSpriteAnimator below the lowest. Instantiated objects are the reverse: `go_uid - 2*(comp_idx+1)` for a clone with no FSM, otherwise `hi + 2*(k+1)` for tk2dSpriteAnimator and `lo - 2*(k+1)` for the rest. PlayMaker proxies take the first-awoken FSM's id ∓ 1. iTweens count down from negative ids per launch. | **Native in part** (R4 ids). A clone's component `j` is `go − 2(j+1)`, which needs the `m_Component` index (the dumps' `hierarchy.json.gz` has each component's `instanceID` and `index`; the tables do not carry them yet). A runtime-added component takes the global counter, so the proxies' and iTweens' ids stay dump data or assumption. |
| A-2 | **Enable order at the dump instant**, in groups: boot DontDestroyOnLoad objects (ascending id); DontDestroyOnLoad under `_GameCameras/HudCamera` (an activation); the boss scene (ascending id); objects Instantiated during the load (an activation); DontDestroyOnLoad runtime clones (an activation). A load group is ordered by ascending id alone. An activation group is ordered by execution order, then descending id (R4). Scene objects the intro activated after the load stay in load order. | Native for the order within a group (R4). The groups reconstruct a history the dump does not record. Settled by a dump of each dispatch list (`m_UpdateNode` chain, native-playerloop.md P1). |
| A-3 | **Started at the dump** for components without a `started` flag. A HealthManager is started iff its dumped `evasionByHitRemaining != 0`. A component on an object whose FSMs are enabled but not started is not started. Everything else active is started. For an inactive component: a PlayMakerFSM uses its own dumped `started`, and a non-FSM component is started iff an FSM on the same GameObject is (GG_Hornet_1's detached `Needle`). | Mechanism native (`m_DidStart` at `+0x125`, R1). Settled by dumping that byte (P1). A re-dump already records `enabled` / `isActiveAndEnabled` and a PlayMakerFSM's `fsmInitialized` / `fsmStarted`. A never-activated pooled clone is dumped holding its template FSM's serialized stub, because `InitTemplate` runs in Awake; the tables give it the template's body (gen_tables.py load_pool). |
| A-4 | `activeSelf` / `activeInHierarchy` still read true inside the OnDisable of a `SetActive(false)`. | **Refuted**, native (R4 flags). |
| A-5 | A component joins the dispatch lists before its OnEnable body runs. | **Native** (R4, AddToManager): lists and the Start queue, before Awake too. |
| A-6 | Colliders/bodies join physics before the activation's scripts run OnEnable. On deactivation they leave after the children's OnDisable and before the object's own scripts' OnDisable. | Activation half native (R4). The deactivation half is native only when the colliders precede the scripts in `m_Component`, which the tables do not record (§4). |
| A-7 | PlayMakerFixedUpdate / PlayMakerLateUpdate iterate their TargetFSMs in the object's component order. | Not native (PlayMaker's C#). A hook on `Fsm.FixedUpdate` per FSM in a fight recording settles it. |
| A-8 | `EnemyHitEffectsUninfected.Update` (didFireThisFrame reset) runs in its HealthManager's Update slot. | **Refined**, native (R3): it has its own node at its own enable position (§4). |
| A-9 | Deactivating an object stops its components' coroutines. Disabling one Behaviour does not. | **Native** (R6). |
| A-10 | WaitForFixedUpdate resumes in `fixed_delayed` after the next physics step since the yield. | **Native** (R6). |
| A-11 | `Object.Destroy` takes effect at the end of the next `*_delayed` stage; `Destroy(obj, t)` after `t` scaled seconds in `update_delayed`. | **Refuted**, native (R7). |
| A-12 | Exit on disable is delivered inside the call that ended the contact: `lc_physics_exit_on_disable` drains `phys_take_exit_events` after every collider disable and object deactivation in the FSM world. Both colliders receive it, the lower instance id first, as in the physics stage. | Measured `[f_exit_col_enabled_*, f_exit_set_active_*, f_exit_destroy_*]`. `simulated = false` ends the contact at the next step (R5). Native removal is synchronous (UP!0x180c07f70); the rest is physics2d (native-physics2d.md). |
| A-13 | A component enabled during `fixed` gets Start at `fixed_delayed` and its first FixedUpdate at the next step. | **Native** (R1, R2). |
| A-14 | FixedUpdate tie-break = most recent enable last, one list for all order-0 types. | **Native** (R3). |
| A-15 | Types without OnEnable/OnDisable enter and leave the lists at their object's activation, like visible types. | **Native** (R3). |
| A-16 | OnDestroy of an in-fight Destroy runs at the destroy pass, after the object's OnDisable. | **Native**, refined (R7). |
| A-17 | Coroutine resumes inside `update_delayed` run in a fixed routine order. | **Refuted**, native (R6: key order). HeroController's routines still run at the env's position (§4). |
| A-18 | HeroAnimationController.Update (550) runs in HeroController's slot (208). | Native in effect: its own bucket 550, and nothing simulated has an Update between 208 and 550. |
| A-19 | A pending Start whose component is disabled when its stage runs is dropped. The next enable queues it again. | **Native** (R1). |
| A-20 | The frozen frame's LateUpdate runs with `Time.deltaTime` 0 and FsmPauseGate open. The step's last live LateUpdate runs with 0.02 and the gate closed. | Native for `deltaTime` (R0: `timeScale` takes effect next frame). The gate is the mod's FsmPauseGate. |
| A-21 | The dump frame's tail (its LateUpdate and `postlate_delayed`) is run at scene start for components the dump shows enabled but not started. Restored components skip it. | Mechanism native (R1). A lifecycle recording armed at `reset` on GG_Ghost_Markoth / Hu / Marmu settles the dump specifics. |
| A-22 | **An object's component order** (the OnDisable walk, pre-destroy, the activation collector) is the registration order: the object's live PlayMakerFSMs, then PlayMakerFixedUpdate / PlayMakerLateUpdate, tk2dSpriteAnimator, HealthManager, Recoil, ConstrainPosition, LimitSendEvents, AutoRecycleSelf, GrimmballControl, DeactivateAfter2dtkAnimation, then iTweens in launch order. | Native: the order is `m_Component` (R4), measured for AddComponent order `[b_component_order]`. `analysis/assets` carries every object's `m_Component` order (tools/extract_assets.py), and a re-dump carries each component's `index`. The tables do not carry it yet. Checked against the dumps: 9,076 of 9,150 scene objects list the same order; the other 74 differ only by a `PlayMakerUnity2DProxy` added at runtime, which Unity appends last. Known counter-example to the registration order: on GG_Hornet_1 `Needle`'s deactivation runs the iTween's OnDisable before the FSM's. |
| A-23 | **The scene-load restore runs no OnEnable body except PlayMakerFSM's.** Everything the dump shows enabled joins the lists with its serial, and `fsm_on_enable` runs. `Recoil.CancelRecoil`, `LimitSendEvents.OnEnable`, `AutoRecycleSelf.StartTimer`, `HealthManager.CheckPersistence` and `GrimmballControl.OnEnable` do not run. The dump is post-OnEnable state, so re-running them would overwrite it. | Not native: it is about restoring from the dump. A lifecycle recording armed at `reset` settles it. |
| A-24 | **A component started at the dump is never Started again.** For a PlayMakerFSM, a later activation takes `Fsm.OnEnable`'s `ActiveState == null -> startState; if (Started) Start()` branch (Fsm.cs:1847-1855), not Unity's Start (PlayMakerFSM.cs:355-361). | **Native** (`m_DidStart` is never cleared, R1); depends on A-3. |
| A-25 | **An object an Animator activates** (a `GameObject.m_IsActive` curve, written in `DirectorUpdateAnimationEnd`) gets its OnEnable there, synchronously (R4); Start before its first LateUpdate in the same frame if the type declares LateUpdate, else in the same frame's `postlate_delayed`; its first Update next frame; its first FixedUpdate at the next fixed step. | R1/R2 applied to the `anim` stage (native-animator.md §11). A lifecycle recording of GG_Hive_Knight's Mouth Swarm (native-animator.md P4) settles it. |

## 6. Reproduce

    python tools/rerecord.py analysis/polbat_GG_Nosk analysis/lifecycle/GG_Nosk 4 lc --limit 4 --env HK_ORACLE_LIFECYCLE=1
    python tools/lifecycle_rules.py                 # -> analysis/lifecycle/rules.json
    python tools/lifecycle_compare.py               # the sim against every recorded episode
    python tools/conformance.py run --reps 4        # the probes in the game -> analysis/conformance/<date>_<git>
    pytest tests/test_conformance.py                # the sim against the probes (GAME_RUN)
    python tools/receiver_order.py                  # within-contact receiver order over the fight corpus

`--env HK_ORACLE_LIFECYCLE_ARM=reset` arms at ResetBegin, so the scene load is captured too.

## 7. Limits

- The fight corpus has no enables inside `fixed`; the probes do (A-13).
- In the fight recorder, types without OnEnable/OnDisable have invisible enables and disables, and
  `Collider2D.enabled` toggles and `SetActive` calls outside `ActivateGameObject`/`ObjectPool` are not
  delimited. The probes measure those directly.
- The probes use execution order 0 only (a mod type cannot set its own); ordering across execution orders
  rests on the native code and the fight corpus (R3).
- The drain takes the scene's history out of the proxy ids, but it does not show the ids. The order of pairs
  created in the same step (R5) rests on four measured orders, not on a model checked against the ids.
- UnityEngine.UI and mod components are not hooked by the recorder.
- Everything is measured at regime R2 only, in GG_Hornet_1 with the boss deactivated.
