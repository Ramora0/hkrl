# FsmStateAction semantics — top-60 boss-scene backlog + all Hornet types

Scope: the **top 60 action types by boss-scene instance count** from `analysis/specs/fsm-census.md`
(second table, DontDestroyOnLoad excluded) **plus every type used by Hornet's own FSMs**
(dump FSMs whose `path` starts with `Boss Holder/Hornet Boss 1`, 16 FSMs, 91 distinct types).
Union = **105 types**, all covered below.

**Counts in this document are *instance* counts (`b/H` = boss-scene / Hornet), not *enabled* counts.**
`FsmState.ActivateActions` skips a disabled action entirely — no `OnEnter`, marked `Finished`
(`PM:FsmState.cs:292-296`) — so disabled instances never execute and never draw. 26 of Hornet's 558
action instances are `enabled:false`: `SetScale` 8/27, `BoolTest` 5/30, `GetScale` 4/13,
**`SendEvent` 2/2 (both)**, `NextFrameEvent` 1/7, `FindChild` 1/3, `SetBoxCollider2DSizeVector` 1/19,
**`FireAtTarget` 1/1 (the only one)**, `SendEventByName` 1/14, `TransitionToAudioSnapshot` 1/3,
`ApplyMusicCue` 1/2. Every other Hornet type is fully enabled. [D54]

Method (binding, per PLAN.md §2): every semantic statement is an exact restatement of the
decompiled `OnEnter` / `OnUpdate` / `OnFixedUpdate` / `OnLateUpdate` / `OnExit` body at the cited
line range. The FSM dump (`analysis/fsm/GG_Hornet_1.json`) is used **only** to say which field
values are actually instantiated — never to infer semantics. Anything not settled by the code is
`UNKNOWN` + an entry in §11, never a guess.

**All 105 types resolve to `analysis/decomp/Assembly-CSharp/`.** HK compiles the PlayMaker action
library into Assembly-CSharp: `analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/`
holds 1112 action sources, while `analysis/decomp/PlayMaker/HutongGames.PlayMaker.Actions/`
holds exactly one file (`MissingAction.cs`). There is **no** type present in both directories, so
the `HutongGames.PlayMaker.Actions.*` names in the dumps are unambiguous. The PlayMaker assembly
still owns the runtime (`Fsm`, `FsmState`, `FsmStateAction`, `PlayMakerFSM`).

Citation shorthand: `A:` = `analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/`,
`HK:` = `analysis/decomp/Assembly-CSharp/`, `PM:` = `analysis/decomp/PlayMaker/HutongGames.PlayMaker/`.
Line numbers are absolute in the named file.

---

## 0. Base contract (needed before any action entry can be read correctly)

### 0.1 Lifecycle

`PM:FsmStateAction.cs:201-223` declares the six empty virtuals: `OnEnter`, `OnFixedUpdate`,
`OnUpdate`, `OnGUI`, `OnLateUpdate`, `OnExit`. `Awake` (`:172-174`), `Reset` (`:164-166`),
`OnPreprocess` (`:168-170`), `Init` (`:152-158`) are also virtual.

`Finish()` (`PM:FsmStateAction.cs:181-189`) is idempotent: if `!finished` it sets
`active=false; finished=true; State.FinishAction(this)`. The `Finished` setter
(`:124-138`) clears `active` whenever set true.

State entry (`PM:FsmState.cs:267-284`, `ActivateActions` `:286-317`):
- `finishedActions.Clear()`, `RealStartTime = FsmTime.RealtimeSinceStartup`, `StateTime = 0`,
  `ActiveActions.Clear()`.
- For each action in index order: a disabled action is marked `Finished=true` and **skipped
  entirely** (`:292-296`) — no `OnEnter`. Otherwise `Active=true; Finished=false; Init(this);
  Entered=true; OnEnter()`.
- If the action did **not** finish in `OnEnter`, it is appended to `ActiveActions` (`:303-306`).
- **`if (Fsm.IsSwitchingState) return false;`** (`:307-310`) — an action whose `OnEnter` sent an
  event that caused a transition **aborts entry of every later action in the state**. This is the
  single most important rule for RNG-stream parity (see §8.4).
- `isSequence` states stop at the first unfinished action (`:311-314`).

Per-frame (`PM:FsmState.cs:342-355` `OnUpdate`, `:331-340` `OnFixedUpdate`, `:357-366`
`OnLateUpdate`): iterate `ActiveActions` in order, `Init(this)` then the callback, then
`CheckAllActionsFinished()`. `OnUpdate` additionally does `StateTime += Time.deltaTime` and is
skipped entirely once the state is `finished` (`:344-346`).

`CheckAllActionsFinished` (`PM:FsmState.cs:609-620`): removes finished actions from
`ActiveActions`; when the list empties (and, for sequences, no further actions activate) sets
`finished=true` and sends `FsmEvent.Finished` (the `FINISHED` event). `RemoveFinishedActions`
(`:600-607`) drains `finishedActions` into removals.

`OnExit` (`PM:FsmState.cs:622-636`) calls `OnExit()` on **every action with `Entered==true`**,
including ones that already finished.

### 0.2 Which Unity callback an action runs in

Actions run in `Update` by default. `OnFixedUpdate` / `OnLateUpdate` fire **only** if the FSM
opted in, and the opt-in is per-FSM, not per-action:

- `Fsm.HandleFixedUpdate` / `Fsm.HandleLateUpdate` setters: `PM:Fsm.cs:1119-1133`, `:1136-1150`
  (they also propagate to `host`).
- `PlayMakerFSM` adds the proxy components on preprocess: `analysis/decomp/PlayMaker/PlayMakerFSM.cs:286-293`.
- The proxies drive the callbacks: `analysis/decomp/PlayMaker/PlayMakerFixedUpdate.cs:11`,
  `analysis/decomp/PlayMaker/PlayMakerLateUpdate.cs:11`.
- Actions request it from `Awake()` or `OnPreprocess()` (e.g. `A:SetVelocity2d.cs:41-44`,
  `A:DecelerateXY.cs:24-32`, `A:Translate.cs:64-74`).

The dump records the resulting per-FSM flags. For Hornet only three FSMs are `handleFixedUpdate`
and none are `handleLateUpdate` (`analysis/fsm/GG_Hornet_1.json`, `Boss Holder/Hornet Boss 1`):

| FSM | handleFixedUpdate | handleLateUpdate | startState |
|---|---|---|---|
| `Hornet Boss 1` / `Control` | **true** | false | `Pause` |
| `Hornet Boss 1` / `Stun Control` | false | false | `Init` |
| `Hornet Boss 1/Evade Check` / `FSM` | **true** | false | `Init` |
| `Hornet Boss 1/Corpse Hornet GG(Clone)` / `Control` | **true** | false | `Blow` |
| the other 12 Hornet FSMs | false | false | — |

### 0.3 Target resolution

`Fsm.GetOwnerDefaultTarget(FsmOwnerDefault)` (`PM:Fsm.cs:2458-2469`): `null` → `null`;
`OwnerOption != UseOwner` → `ownerDefault.GameObject.Value`; else the FSM's own GameObject.
HK's `FsmOwnerDefault.GetSafe(this)` extension (`HK:FSMUtility.cs:186-193`) is the same rule but
returns `stateAction.Owner` for `UseOwner`.

### 0.4 Event sending

- `Fsm.Event(FsmEvent)` (`PM:Fsm.cs:2192-2198`) is a **no-op when the event is null**. Many
  actions unconditionally call it with an optional field; a `None` event slot means "do nothing".
- `Fsm.Event(FsmEventTarget, string)` (`:2106-2112`) is a no-op on null/empty name.
- `Fsm.Event(FsmEventTarget, FsmEvent)` (`:2126-2182`) dispatches **synchronously**
  (`ProcessEvent` at `:2023+` for `Self`), so a transition can take effect inside the caller's
  `OnEnter` — see the `IsSwitchingState` rule in §0.1.
- `Fsm.DelayedEvent(...)` (`PM:Fsm.cs:2200-2212`) registers a `DelayedEvent`; its `Update()`
  (`PM:DelayedEvent.cs:68-88`) decrements `timer -= Time.deltaTime` and fires when `timer < 0`.
  `DelayedEvent.WasSent(null)` returns **true** (`:95-98`). That null case is **not** what
  finishes the delay-0 branch of `SendEvent`/`SendEventByName`: there `Finish()` runs inside
  `OnEnter` (`A:SendEvent.cs:40-43`, `A:SendEventByName.cs:36-39`), so `OnUpdate` never runs.
  `WasSent(null)` only matters when `everyFrame` is set and `OnUpdate` is reached with no
  delayed event registered. [D50]

### 0.5 Time sources

- `Time.deltaTime` — `Wait`, `WaitRandom`, `FloatAdd(perSecond)`, `FloatSubtract(perSecond)`,
  `Vector3AddXYZ(perSecond)`, `Translate(perSecond)`, `AudioPlayerOneShot(delay)`,
  `AudioPlayerOneShotSingle(delay)`, `EaseFsmAction`, `DelayedEvent`, `FsmState.StateTime`.
- `FsmTime.RealtimeSinceStartup` (`PM:FsmTime.cs:16-31`) = `Time.realtimeSinceStartup` minus a
  paused-time accumulator. Read by `Wait`/`WaitRandom` **only when `realTime` is set**, and
  unconditionally by `EaseFsmAction.OnEnter` and `FsmState.OnEnter` (as `RealStartTime`).
  No Hornet `Wait` (12) or `WaitRandom` (5) instance sets `realTime` — all are `false`.

### 0.6 Shared helpers

- `ComponentAction<T>.UpdateCache(go)` (`A:ComponentAction.cs:25-41`): `null` → false; re-`GetComponent`
  when the cached component is null or the GameObject changed; returns whether a component exists.
- `RigidBody2dActionBase.CacheRigidBody2d(go)` (`A:RigidBody2dActionBase.cs:9-19`): `GetComponent<Rigidbody2D>`,
  warns if missing. **Never clears `rb2d` when `go == null`** — a stale body persists.
- `ActionHelpers.GetRandomWeightedIndex(FsmFloat[])`
  (`HK:HutongGames.PlayMaker/ActionHelpers.cs:73-90`): sums the weights, draws
  **`UnityEngine.Random.Range(0f, sum)` — exactly one float draw** (`:80`), then walks the array
  subtracting weights; returns `-1` if it falls off the end. That happens when `sum == 0`
  **and** when the draw lands exactly on `sum`: float `Range` is max-inclusive (`u = 0` yields
  `hi`; `analysis/open-questions.md` Q15), and the loop test is the strict `num2 < weights[j]`
  (`:83`). Both `-1` paths are live and must be reproduced. [D56]
- `ActionHelpers.GetGameObjectFsm(go, fsmName)`
  (`HK:HutongGames.PlayMaker/ActionHelpers.cs:56-71`): when `fsmName` is non-empty it scans
  `GetComponents<PlayMakerFSM>()` for a name match; **on a miss it logs a warning and falls through
  to `go.GetComponent<PlayMakerFSM>()` (`:70`) — the first FSM on the object.** So the
  "Could not find FSM" branch inside `SetFsmBool`/`SetFsmFloat`/`SetFsmString`/`GetFsmInt` fires
  only when the object carries **no** `PlayMakerFSM` at all; a wrong `fsmName` silently retargets
  the first one. [D52]
- `ActionHelpers.LayerArrayToLayerMask` (`:345-361`): OR of `1 << layer`, optional invert,
  returns `-5` when the mask is 0.
- `EaseFsmAction` (`A:EaseFsmAction.cs`): `OnEnter:123-135` sets `startTime =
  FsmTime.RealtimeSinceStartup`, `percentage = reverse ? 1 : 0`, `delayTime = delay`;
  `OnUpdate:141-203` burns the delay, then `UpdatePercentage` (`:204-235`,
  `runningTime += Time.deltaTime[*speed]`, `percentage = runningTime/time`) and evaluates the
  easing function into `resultFloats`; sets `finishAction` when the percentage passes the bound.
- `GameObject.Spawn(pos, rot)` (`HK:ObjectPoolExtensions.cs:61-64`) → `ObjectPool.Spawn` (pooled
  instantiate). `CreateObject` uses raw `Object.Instantiate` instead.

---

## 1. Control flow

**`Wait`** `A:Wait.cs` · 185 boss-scene / 12 Hornet
- fields: `time:FsmFloat`, `finishEvent:FsmEvent`, `realTime:bool`.
- `OnEnter:27-39`: if `time <= 0` → `Fsm.Event(finishEvent)` **then** `Finish()`, i.e. finishes
  immediately. Else `startTime = FsmTime.RealtimeSinceStartup; timer = 0` and does **not** finish.
- `OnUpdate:41-59`: `realTime ? timer = FsmTime.RealtimeSinceStartup - startTime : timer += Time.deltaTime`;
  when `timer >= time` → `Finish()` **then** `Fsm.Event(finishEvent)` if non-null. Note the order is
  reversed relative to `OnEnter`.
- callback: Update. RNG: none. Hornet: all 12 `realTime=false`.

**`NextFrameEvent`** `A:NextFrameEvent.cs` · 63 / 7
- fields: `sendEvent:FsmEvent`. `OnEnter:15-17` empty (never finishes on entry).
  `OnUpdate:19-23`: `Finish()` then `Fsm.Event(sendEvent)`. One-frame delay. Update. RNG none.

**`BoolTest`** `A:BoolTest.cs` · 160 / 30
- fields: `boolVariable:FsmBool`, `isTrue/isFalse:FsmEvent`, `everyFrame:bool`.
- `OnEnter:29-36`: `Fsm.Event(value ? isTrue : isFalse)`; `Finish()` unless `everyFrame`.
  `OnUpdate:38-41` repeats the send. Update. RNG none. Hornet: 29 `everyFrame=false`, 1 `true`.

**`BoolTestMulti`** `A:BoolTestMulti.cs` · 7 / 3
- fields: `boolVariables:FsmBool[]`, `boolStates:FsmBool[]`, `trueEvent/falseEvent:FsmEvent`,
  `storeResult:FsmBool`, `everyFrame:bool`.
- `DoAllTrue:49-73`: returns silently if either array is empty or lengths differ; else AND of
  `boolVariables[i]==boolStates[i]`, writes `storeResult`, sends `trueEvent`/`falseEvent`.
  `OnEnter:35-42` + `OnUpdate:44-47`. Update. RNG none.

**`FloatCompare`** `A:FloatCompare.cs` · 99 / 8
- fields: `float1`,`float2`,`tolerance:FsmFloat`, `equal/lessThan/greaterThan:FsmEvent`, `everyFrame`.
- `DoCompare:58-72`: `Mathf.Abs(f1-f2) <= tolerance` → `equal`; else `<` → `lessThan`; else `>` →
  `greaterThan` (exactly one branch). `OnEnter:44-51`, `OnUpdate:53-56`. Update. RNG none.

**`FloatTestToBool`** `A:FloatTestToBool.cs` · 6 / 4
- NOT the same comparison as `FloatCompare`: `DoCompare:58-84` is three INDEPENDENT if/else blocks —
  `equalBool = |float1-float2| <= tolerance` (`:60-66`), `lessThanBool = float1 < float2` raw, no tolerance
  (`:68-74`), `greaterThanBool = float1 > float2` raw (`:76-82`); every bool is overwritten on each call
  (false on the negative branch), so inputs equal within tolerance but not bit-equal set `equalBool`
  AND one of less/greater together (impossible under `FloatCompare`'s exclusive chain `A:FloatCompare.cs:60-68`).
  Sends no events. `OnEnter:44-51`, `OnUpdate:53-56`. (Corrected 2026-08-31 from the decomp-reader delta.)

**`FloatInRange`** `A:FloatInRange.cs` · 8 / 6
- `DoFloatRange:52-67`: no-op if `floatVariable.IsNone`; else
  `v <= upperValue && v >= lowerValue` → `boolVariable=true` + `trueEvent`, else `false` +
  `falseEvent`. `OnEnter:38-45`, `OnUpdate:47-50`. Update. RNG none.

**`IntCompare`** `A:IntCompare.cs` · 30 / 7
- `DoIntCompare:48-62`: `==` → `equal`, `<` → `lessThan`, `>` → `greaterThan`.
  `OnEnter:34-41`, `OnUpdate:43-46`. Update. RNG none.

**`StringCompare`** — not in scope (18 boss-scene, not top-60, unused by Hornet).

**`PlayerDataBoolTest`** `A:PlayerDataBoolTest.cs` · 20 / 1 — see §6 (reads PlayerData).

**`GGCheckIfBossScene`** `HK:GGCheckIfBossScene.cs` · 15 / 1
- fields: `bossSceneEvent`, `regularSceneEvent:FsmEvent`. `OnEnter:16-27`: reads the static
  `BossSceneController.IsBossScene`, sends one of the two events, `Finish()`. Update. RNG none.

**`CallMethodProper`** `A:CallMethodProper.cs` · 56 / 0
- fields: `gameObject:FsmOwnerDefault`, `behaviour:FsmString`, `methodName:FsmString`,
  `parameters:FsmVar[]`, `storeResult:FsmVar`.
- `OnEnter:45-50`: allocate the parameter array, `DoMethodCall()`, `Finish()` — always finishes.
- `DoMethodCall:52-106`: `GetComponent(behaviour) as MonoBehaviour`; caches `MethodInfo` via
  `GetMethod(methodName)` (`DoCache:108-121`); zero-arg → `Invoke(inst, null)`, else evaluates each
  `FsmVar` (`UpdateValue()`, `GetValue()`) in index order and invokes; writes `storeResult` when
  its `Type != Unknown`.
- Reflection into arbitrary game code. The 23 distinct targets used across the four boss scenes
  (none on the Hornet object itself) are all HeroController / GameManager / DialogueBox /
  GameMap / SceneManager methods — scene-transition and journal plumbing, e.g.
  `HeroController.RelinquishControl` ×5, `HeroController.EnterWithoutInput` ×5,
  `GameManager.ChangeToScene` ×5, `HeroController.MaxHealth` ×4, `HeroController.FaceLeft/FaceRight`.
- **Not portable as a generic action.** The sim must implement a per-`behaviour.methodName`
  dispatch table and trap on any unlisted pair (PLAN §2.3).

---

## 2. Variables & math

All of these are pure FSM-variable writes: Update callback, no events, no RNG. `OnEnter` does the
write and `Finish()`es unless `everyFrame`, in which case `OnUpdate` repeats it.

| Type | file:lines (OnEnter / OnUpdate) | fields | operation |
|---|---|---|---|
| `SetFloatValue` | `A:SetFloatValue.cs:23-30 / 32-35` | `floatVariable`, `floatValue:FsmFloat`, `everyFrame` | `var = value` |
| `SetBoolValue` | `A:SetBoolValue.cs:23-30 / 32-35` | `boolVariable`, `boolValue:FsmBool`, `everyFrame` | `var = value` |
| `SetIntValue` | `A:SetIntValue.cs:23-30 / 32-35` | `intVariable`, `intValue:FsmInt`, `everyFrame` | `var = value` |
| `SetColorValue` | `A:SetColorValue.cs:23-30 / 32-35`, `Do:37-43` | `colorVariable`, `color:FsmColor`, `everyFrame` | `var = value` if `colorVariable != null` |
| `SetGameObject` | `A:SetGameObject.cs:22-29 / 31-34` | `variable`, `gameObject:FsmGameObject`, `everyFrame` | `var = value` |
| `FloatAdd` | `A:FloatAdd.cs:32-39 / 41-44`, `Do:46-56` | `floatVariable`, `add:FsmFloat`, `everyFrame`, `perSecond` | `var += add [* Time.deltaTime]` |
| `FloatSubtract` | `A:FloatSubtract.cs:32-39 / 41-44`, `Do:46-56` | `floatVariable`, `subtract`, `everyFrame`, `perSecond` | `var -= sub [* Time.deltaTime]` |
| `FloatMultiply` | `A:FloatMultiply.cs:26-33 / 35-38` | `floatVariable`, `multiplyBy`, `everyFrame` | `var *= by` |
| `FloatClamp` | `A:FloatClamp.cs:33-40 / 42-45`, `Do:47-50` | `floatVariable`, `minValue`, `maxValue`, `everyFrame` | `Mathf.Clamp` |
| `FloatOperator` | `A:FloatOperator.cs:47-54 / 56-59`, `Do:61-86` | `float1`,`float2`,`operation:enum{Add,Subtract,Multiply,Divide,Min,Max}`,`storeResult`,`everyFrame` | per enum; `Divide` is unguarded (`/0` → ±Inf/NaN) |
| `IntAdd` | `A:IntAdd.cs:23-30 / 32-35` | `intVariable`, `add`, `everyFrame` | `var += add` |
| `IntOperator` | `A:IntOperator.cs:42-49 / 51-54`, `Do:56-81` | `integer1`,`integer2`,`operation`,`storeResult`,`everyFrame` | per enum; `Divide` is integer division, unguarded |
| `Vector3AddXYZ` | `A:Vector3AddXYZ.cs:33-40 / 42-45`, `Do:47-58` | `vector3Variable`, `addX/Y/Z:FsmFloat`, `everyFrame`, `perSecond` | `v += (x,y,z) [* Time.deltaTime]` |

**`IntAddV2`** `A:IntAddV2.cs` · 2 / 1 — the same as `IntAdd` but **FixedUpdate**:
`OnPreprocess:23-26` sets `Fsm.HandleFixedUpdate = true`; `OnEnter:28-35` adds and finishes unless
`everyFrame`; `OnFixedUpdate:37-40` adds. Hornet's single instance is `everyFrame=true`.

Hornet field values (`analysis/fsm/GG_Hornet_1.json`): every one of the 14 `SetFloatValue`,
7 `SetBoolValue`, 4 `SetIntValue`, 4 `IntAdd`, 3 `FloatMultiply`, 2 `FloatClamp`, 2 `FloatSubtract`
(`perSecond=false`), 4 `FloatOperator` (`operation=2` = Multiply), 2 `IntOperator`
(`operation=1` = Subtract), 1 `Vector3AddXYZ` (`perSecond=false`) has `everyFrame=false`;
`FloatTestToBool` splits 2 true / 2 false; `FloatCompare` splits 4/4.

---

## 3. Events

**`SendEvent`** `A:SendEvent.cs` · 4 / 2
- fields: `eventTarget:FsmEventTarget`, `sendEvent:FsmEvent`, `delay:FsmFloat`, `everyFrame:bool`.
- `OnEnter:35-49`: **`delay < 0.001f`** → send now, `Finish()` unless `everyFrame`; else register a
  `DelayedEvent` and do not finish.
- `OnUpdate:51-64`: `!everyFrame` → finish once `DelayedEvent.WasSent`; `everyFrame` → resend
  every frame (and never finish). Update. RNG none. Hornet: 2 instances, `delay=0`,
  `everyFrame=false`; **both are `enabled:false`**, so no `SendEvent` ever runs on Hornet. [D54]

**`SendEventByName`** `A:SendEventByName.cs` · 195 / 14 — **the most common boss-scene action**
- identical structure to `SendEvent` but the event is a `FsmString` resolved through
  `FsmEvent.GetFsmEvent(sendEvent.Value)`. `OnEnter:31-45`, `OnUpdate:47-60`. Same `0.001f`
  threshold. Hornet: 14 instances, all `delay=0`, `everyFrame=false` → pure fire-and-finish;
  13 enabled (`Control/Throw` idx 3, an `EnemyKillShake`, is `enabled:false`). [D54]

**`SendRandomEvent`** `A:SendRandomEvent.cs` · 8 / 4 — **RNG**
- fields: `events:FsmEvent[]`, `weights:FsmFloat[]`, `delay:FsmFloat`.
- `OnEnter:24-44`: if `events.Length != 0` → `GetRandomWeightedIndex(weights)` (**1 float draw**);
  if the index is `-1` fall through to `Finish()`; else `delay < 0.001f` → send + `Finish()`,
  otherwise register a `DelayedEvent`. `OnUpdate:46-52` finishes when the delayed event fired.
- Draw count: **exactly 1 per `OnEnter`**, taken *before* any event is sent.

**`SendRandomEventV2`** `A:SendRandomEventV2.cs` · 4 / 2 — **RNG, variable draw count**
- extra fields: `trackingInts:FsmInt[]`, `eventMax:FsmInt[]` (no `delay` branch is reachable —
  `delayedEvent` is declared but never assigned).
- `OnEnter:26-45`: `while (!flag)` { `GetRandomWeightedIndex` (**1 draw per iteration**); if
  `idx != -1 && trackingInts[idx] < eventMax[idx]` → pre-increment `trackingInts[idx]`, zero **all**
  tracking ints, restore the incremented value, send `events[idx]`, exit loop }. Then `Finish()`.
- **Unbounded**: if the sum of weights is 0, `GetRandomWeightedIndex` returns `-1` forever and the
  loop spins, drawing once per iteration. No escape hatch (unlike V3).
- `trackingInts` are ordinary FSM int variables → they persist across state entries and are part of
  the boss's observable state.

**`SendRandomEventV3`** `A:SendRandomEventV3.cs` · 2 / 2 — **RNG, variable draw count**
- extra fields over V2: `trackingIntsMissed:FsmInt[]`, `missedMax:FsmInt[]`.
- `OnEnter:33-86`: `while (!flag)` { `GetRandomWeightedIndex` (**1 draw per iteration**);
  if `idx != -1`: first scan `trackingIntsMissed[i] >= missedMax[i]` (`:43-50`) — **the scan has no
  `break`, so `num` ends as the *highest* over-limit index, not the first** — then force that index
  (`num`), zero all `trackingInts`, increment all `trackingIntsMissed`, zero `trackingIntsMissed[num]`,
  set `trackingInts[num]=1`, send `events[num]`; else if `trackingInts[idx] < eventMax[idx]`,
  the V2 update plus `trackingIntsMissed[k]++` for all and `trackingIntsMissed[idx]=0`, send
  `events[idx]`. `loops++`; `loops > 100` → send `events[0]`, `Finish()`, exit. } Then `Finish()`.
- `loops` is an instance field and is **never reset** between entries, so the 100-iteration budget
  is cumulative over the FSM's lifetime.

**`Trigger2dEvent`** `A:Trigger2dEvent.cs` · 37 / 14
- fields: `trigger:PlayMakerUnity2d.Trigger2DType{OnTriggerEnter2D=0,Stay=1,Exit=2}` (as dumped),
  `collideTag:FsmString`, `collideLayer:FsmString`, `sendEvent:FsmEvent`, `storeCollider:FsmGameObject`.
- `OnEnter:41-60`: gets/adds a `PlayMakerUnity2DProxy` on the **Owner** and registers the matching
  delegate. **Never finishes on entry** — it lives until the state exits.
- `OnExit:62-79`: unregisters.
- Callbacks `:86-93 / 95-102 / 104-111`: fire only for the matching trigger type and when
  `other.tag == collideTag || collideTag.IsNone || collideTag.Value` empty `|| == "Untagged"`;
  then `storeCollider = other.gameObject` and `Fsm.Event(sendEvent)`.
  **`collideLayer` is declared but never read.**
- Dispatched from `PlayMakerUnity2DProxy`, a `MonoBehaviour` with Unity's own `OnTriggerEnter2D`
  (`HK:PlayMakerUnity2DProxy.cs:190`) / `OnCollisionEnter2D` (`:143`) callbacks — **not** from
  `OnUpdate`. Where those callbacks land inside the frame is a Unity semantic, not stated by any
  decompiled source: see Q-fsmact-12 and `analysis/specs/frame-order.md`. [D55]
- Enum order `OnTriggerEnter2D=0, OnTriggerStay2D=1, OnTriggerExit2D=2`
  (`HK:PlayMakerUnity2d.cs:14-19`). Hornet: 7 Enter, 1 Stay, 6 Exit.

**`SendEventToRegister`** `HK:SendEventToRegister.cs` · 48 / 0
- field: `eventName:FsmString`. `OnEnter:13-20`: if non-empty → `EventRegister.SendEvent(name)`,
  `Finish()`. `EventRegister.SendEvent` (`HK:EventRegister.cs:41-51`) is a global name→subscriber
  dictionary; each subscriber's `ReceiveEvent()` does
  `FSMUtility.SendEventToGameObject(gameObject, subscribedEvent)` (`:27`) plus a C# event.
  Global broadcast — must be modelled, not skipped.

**`CheckSendEventLimit`, `SendTrigger2DEventByName`, `GGCheckIfBossSequence`** — outside scope
(not top-60 boss-scene, not used by Hornet).

---

## 4. Transform & physics

### 4.1 Transform reads

**`GetPosition`** `A:GetPosition.cs` · 92 / 8 — fields `gameObject`, `vector:FsmVector3`,
`x/y/z:FsmFloat`, `space:Space`, `everyFrame`. `Do:53-64` reads
`space==World ? transform.position : transform.localPosition` and writes **all four** outputs
unconditionally. `OnEnter:39-46`, `OnUpdate:48-51`. Hornet: 8 instances, all `space=0` (World),
6 `everyFrame=false` / 2 `true`.

**`GetScale`** `A:GetScale.cs` · 34 / 13 — `Do:53-64` reads
`space==World ? transform.lossyScale : transform.localScale` into `vector`,`xScale`,`yScale`,`zScale`.
`OnEnter:39-46`, `OnUpdate:48-51`. Hornet: 13 instances all `everyFrame=false`; 9 World, 4 Self.

**`GetRotation`** `A:GetRotation.cs` · 2 / 1 — `Do:58-82` writes `quaternion`, `vector`,
`xAngle`,`yAngle`,`zAngle` from `rotation`/`eulerAngles` (World) or `Quaternion.Euler(localEulerAngles)`/
`localEulerAngles` (Self). `OnEnter:44-51`, `OnUpdate:53-56`.

**`GetParent`** `A:GetParent.cs` · 25 / 11 — `OnEnter:21-33`: `storeResult = transform.parent?.gameObject`
(null when the target is null), `Finish()`. Always one-shot.

**`GetOwner`** `A:GetOwner.cs` · 64 / 14 — `OnEnter:16-20`: `storeGameObject = Owner`, `Finish()`.

**`FindChild`** `A:FindChild.cs` · 71 / 3 — `OnEnter:29-33` → `Do:35-43`:
`transform.Find(childName)` (supports `A/B/C` paths), stores the GameObject or null. `Finish()`.

**`FindGameObject`** `A:FindGameObject.cs` · 20 / 2 — `OnEnter:28-32` → `Find:34-60`:
`withTag != "Untagged"` and name non-empty → linear scan of `FindGameObjectsWithTag` matching
`name`; tag only → `FindGameObjectWithTag`; else `GameObject.Find(objectName)`. `Finish()`.
**Scene-graph search order matters** (`FindGameObjectsWithTag` array order is unspecified — Q-fsmact-1).

**`GetTag`** `A:GetTag.cs` · 2 / 1 — `Do:37-43`: `storeResult = gameObject.Value.tag` when non-null.
`OnEnter:23-30`, `OnUpdate:32-35`.

### 4.2 Transform writes

**`SetPosition`** `A:SetPosition.cs` · 64 / 11
- fields: `gameObject`, `vector:FsmVector3`, `x/y/z:FsmFloat`, `space:Space`, `everyFrame`, `lateUpdate`.
- `OnPreprocess:53-59` sets `Fsm.HandleLateUpdate` iff `lateUpdate`.
- `OnEnter:61-68`: **only acts when `!everyFrame && !lateUpdate`** (then writes and `Finish()`);
  otherwise it does nothing at all on entry.
- `OnUpdate:70-76` writes when `!lateUpdate`. `OnLateUpdate:78-88` writes when `lateUpdate`, then
  `Finish()` unless `everyFrame` — note this `Finish()` runs **even when `lateUpdate` is false**,
  which is unreachable because the FSM only gets `OnLateUpdate` if some action opted in.
- `Do:90-117`: base = `vector` if set, else current world/local position; per-axis override for
  non-`IsNone` `x/y/z`; assign `transform.position` or `.localPosition`.
- Hornet: 11 instances, all `everyFrame=false`, `lateUpdate=false`; 7 World, 4 Self.

**`SetScale`** `A:SetScale.cs` · 94 / 27 — same shape as `SetPosition` except `OnEnter:57-64`
**always** calls `DoSetScale()` then `Finish()` unless `everyFrame` (no `lateUpdate` guard).
`OnPreprocess:49-55`, `OnUpdate:66-72`, `OnLateUpdate:74-84`, `Do:86-106` (base = `vector` or current
`localScale`, per-axis override, writes `transform.localScale`). Hornet: 27 instances (**19 enabled,
8 disabled**), all `everyFrame=false`, `lateUpdate=false`. This is Hornet's facing mechanism
(x-scale sign). [D54]

**`SetRotation`** `A:SetRotation.cs` · 33 / 16 — `OnPreprocess:59-65`, `OnEnter:67-74` (same
`!everyFrame && !lateUpdate` guard as `SetPosition`), `OnUpdate:76-82`, `OnLateUpdate:84-94`,
`Do:96-123`: base = `quaternion.eulerAngles` if set, else `vector`, else current
`localEulerAngles`/`eulerAngles`; per-axis override; writes `localEulerAngles` or `eulerAngles`.
Hornet: 15 `everyFrame=false` + 1 `true`, all `lateUpdate=false`; 12 World, 4 Self.

**`FlipScale`** `A:FlipScale.cs` · 2 / 2 — `Do:59-75` negates `localScale.x` and/or `.y` per the
plain `bool` fields `flipHorizontally`/`flipVertically`. `OnEnter:30-37` always flips (no
`lateUpdate` guard) then finishes unless `everyFrame`; `OnUpdate:39-45`, `OnLateUpdate:47-57`.
**`lateUpdate` has no `OnPreprocess` opt-in here**, so setting it silently disables the action in
`OnUpdate` without ever enabling `OnLateUpdate`.

**`Translate`** `A:Translate.cs` · 3 / 1
- fields: `gameObject`, `vector`, `x/y/z`, `space`, `perSecond`, `everyFrame`, `lateUpdate`, `fixedUpdate`.
- `OnPreprocess:64-74` opts into FixedUpdate and/or LateUpdate.
- `OnEnter:76-83`: acts only if `!everyFrame && !lateUpdate && !fixedUpdate`.
- `OnUpdate:85-91` (when neither late nor fixed), `OnLateUpdate:93-103`, `OnFixedUpdate:105-115`.
- `Do:117-144`: base = `vector` if set else `(x,y,z)`; per-axis override; then
  `transform.Translate(v [* Time.deltaTime], space)`.
- Hornet's one instance: all four flags false → a single `OnEnter` translate.

**`SetParent`** `A:SetParent.cs` · 35 / 8 — `OnEnter:30-46`: `transform.parent = parent?.transform`
(null detaches), then optional `localPosition = Vector3.zero` (`:38`) / `localRotation = identity`
(`:42`); `Finish()`. The action itself writes **nothing else** — what the `Transform.parent` setter
does to the world pose is a Unity semantic that no decompiled source states: **UNKNOWN**, see
Q-fsmact-11. Hornet's 8 instances must not be ported on an assumption either way. [D04]

**`FaceObject`** `A:FaceObject.cs` · 13 / 11 (Hornet's facing action)
- fields: `objectA:FsmGameObject`, `objectB:FsmGameObject`, `spriteFacesRight:FsmBool`,
  `playNewAnimation:bool`, `newAnimationClip:FsmString`, `resetFrame:bool` (default `true` in the
  field initializer but `false` in `Reset:34-43`), `everyFrame:bool`.
- `OnEnter:45-62`: `_sprite = objectA.GetComponent<tk2dSpriteAnimator>()`; **if null → `Finish()`
  but execution continues**; caches `xScale = |objectA.localScale.x|`; `DoFace()`;
  `Finish()` unless `everyFrame`. `OnUpdate:64-67`.
- `DoFace:69-134`: if `objectB` is null/None → `Finish()` (and continues, dereferencing
  `objectB.Value` on the next line → NRE risk). Compares `A.position.x < B.position.x`; picks
  target `xScale` sign from `spriteFacesRight`; **only when the sign actually changes** does it
  optionally `_sprite.PlayFromFrame(0)` (`resetFrame`) and/or `_sprite.Play(newAnimationClip)`
  (`playNewAnimation`). Finally writes `objectA.transform.localScale` with the new x.
- Hornet: 11 instances, all `everyFrame=false`, `spriteFacesRight=false`, `resetFrame=false`,
  `playNewAnimation=false` → pure x-scale sign flip, no animation side effect.

**`FaceAngle`** `A:FaceAngle.cs` · 2 / 1 — **FixedUpdate** (`Awake:28-31`, `OnPreprocess:33-36`).
`OnEnter:38-47` caches rb2d + target, `DoAngle()`, finish unless `everyFrame`;
`OnFixedUpdate:49-52`. `DoAngle:54-62`: `z = Atan2(v.y, v.x) * 180/π + angleOffset`, writes
`transform.localEulerAngles = (0,0,z)`.

**`iTweenScaleTo`** `A:iTweenScaleTo.cs` · 1 / 1 — `OnEnter:60-68` → `DoiTween:75-88` hands off to
`iTween.ScaleTo` with a hash of options (`scale`, `time`/`speed`, `delay`, `easetype`, `looptype`,
`ignoretimescale`). Never finishes itself; completion arrives via the `iTweenOnComplete` message.
`OnExit:70-73`. **Requires the iTween runtime** — the sim needs the easing curve + coroutine
semantics or a trap. Hornet's single instance: `delay=0`, `realTime=false`.

### 4.3 Rigidbody2D / collision

**`SetVelocity2d`** `A:SetVelocity2d.cs` · 91 / 23 — `ComponentAction<Rigidbody2D>`
- fields: `gameObject`, `vector:FsmVector2`, `x:FsmFloat`, `y:FsmFloat`, `everyFrame:bool`.
- callback: **FixedUpdate** — `Awake:41-44` sets `Fsm.HandleFixedUpdate = true`
  (unconditionally, for every instance, on every FSM that contains one). There is **no `OnUpdate`
  override**.
- `OnEnter:46-53`: `DoSetVelocity()`, `Finish()` unless `everyFrame`.
  `OnFixedUpdate:55-62`: same body, same finish rule.
- `Do:64-80`: `UpdateCache`; base velocity = `vector` if not `IsNone` else the **current**
  `rigidbody2d.velocity`; per-axis override for non-`IsNone` `x`/`y`; assign `rigidbody2d.velocity`.
- Hornet: 23 instances, **all `everyFrame=false`** → each is a single write at state entry.
  (Hornet's `Control` FSM is nonetheless `handleFixedUpdate=true` because of `Awake`.)

**`GetVelocity2d`** `A:GetVelocity2d.cs` · 21 / 1 — `ComponentAction<Rigidbody2D>`, **Update**
(no fixed-update opt-in). `Do:56-70`: reads `rigidbody2d.velocity`, optionally
`transform.InverseTransformDirection` for `Space.Self`, writes `vector`,`x`,`y`.
`OnEnter:42-49`, `OnUpdate:51-54`. Hornet's single instance is `everyFrame=true`, `space=World`.

**`SetVelocityAsAngle`** `A:SetVelocityAsAngle.cs` · 13 / 2 — `RigidBody2dActionBase`,
**FixedUpdate** (`Awake:40-43`, `OnPreprocess:45-48`). `OnEnter:50-58` caches rb2d, sets velocity,
finishes unless `everyFrame`; `OnFixedUpdate:60-67`. `Do:69-80`:
`v = (speed*cos(angle*π/180), speed*sin(angle*π/180))`, assigned to `rb2d.velocity`.

**`SetGravity2dScale`** `A:SetGravity2dScale.cs` · 42 / 11 — `ComponentAction<Rigidbody2D>`.
`OnEnter:24-28`: `Do` + `Finish()` (always one-shot, no `everyFrame`).
`Do:30-37`: `rigidbody2d.gravityScale = gravityScale.Value`.

**`SetIsKinematic2d`** `A:SetIsKinematic2d.cs` · 11 / 4 — `ComponentAction<Rigidbody2D>`.
`OnEnter:24-28` + `Do:30-37`: `rigidbody2d.isKinematic = isKinematic.Value`; always `Finish()`.

**`DecelerateV2`** `A:DecelerateV2.cs` · 6 / 3 — `RigidBody2dActionBase`, **FixedUpdate**
(`Awake:21-24`, `OnPreprocess:26-29`). `OnEnter:31-35` caches rb2d and decelerates once;
**never calls `Finish()`** → runs every FixedUpdate for the whole state (`OnFixedUpdate:37-40`).
`Do:42-82`: each axis independently `v *= deceleration`, clamped to 0 if the sign flipped.
Note the field is a multiplier, not a rate; no `Time` term.

**`DecelerateXY`** `A:DecelerateXY.cs` · 3 / 3 — same as `DecelerateV2` with separate
`decelerationX`/`decelerationY`, each skipped when `IsNone`, plus a `|v| < 0.001f → 0` deadzone
per axis (`:70-73`, `:93-96`). `Awake:24-27`, `OnPreprocess:29-32`, `OnEnter:34-38` (no `Finish`),
`OnFixedUpdate:40-43`, `Do:45-99`.

**`FireAtTarget`** `A:FireAtTarget.cs` · 1 / 1 — **RNG when `spread` is set**, **FixedUpdate**
(`Awake:43-46`, `OnPreprocess:48-51`). `OnEnter:53-62` caches self+rb2d, sets velocity, finish
unless `everyFrame`; `OnFixedUpdate:64-71`. `Do:73-91`:
`angle = Atan2(target.y+position.y-self.y, target.x+position.x-self.x) * 180/π`; **if
`!spread.IsNone` → `angle += Random.Range(-spread, spread)` (1 float draw, `:82`)**; then
`v = speed*(cos,sin)` in radians. Hornet's single instance is **disabled** (`enabled=false`) and has
`spread=0`.

**`CheckCollisionSide`** `A:CheckCollisionSide.cs` · 31 / 7
- fields: `topHit/rightHit/bottomHit/leftHit:FsmBool`, `topHitEvent/…:FsmEvent`, `otherLayer:bool`,
  `otherLayerNumber:int`, `ignoreTriggers:FsmBool`. `RAYCAST_LENGTH = 0.08f` (`:49`).
- `OnEnter:75-120`: caches `Collider2D` from `Fsm.GameObject`, allocates four 3-element ray lists,
  gets/adds `PlayMakerUnity2DProxy` on `Owner`, registers `DoCollisionStay2D`, and sets the four
  `check*` flags from "output bool is not None **or** event is non-null". **Never finishes.**
- `OnUpdate:127-140`: **only re-checks if at least one of the four hit bools is already true**;
  layer = `8` unless `otherLayer`, then `otherLayerNumber`.
- `DoCollisionStay2D:142-155`: on a `layer == 8` (or `otherLayer`) contact, run `CheckTouching`.
- `CheckTouching:165-240`: per enabled side, three ray origins from the collider `bounds`
  (min/center/max along the edge), `Physics2D.Raycast(origin, dir, 0.08f, 1 << layer)`; the hit bool
  is cleared first, then set true + event sent on the first hit that is not an ignored trigger.
  Order: top, right, bottom, left. `OnExit:122-125` unregisters.
- `DoCollisionExit2D:157-163` clears all four bools but **is never registered**, so it never runs.
- Hornet: 7 instances, `otherLayer=false` (layer 8 = Terrain), `ignoreTriggers` 6 false / 1 true.

**`CheckCollisionSideEnter`** `A:CheckCollisionSideEnter.cs` · 31 / 6 — same idea, driven by
`OnCollisionEnter2D` only. `OnEnter:63-72` registers `DoCollisionEnter2D`; `OnUpdate:79-81` is
**empty**; `OnExit:74-77` unregisters. `DoCollisionEnter2D:83-96` gates on
`LayerMask.LayerToName(layer) == "Terrain"` (string compare, not `== 8`) unless `otherLayer`.
`CheckTouching:98-160` always tests all four sides (no `check*` flags), allocating fresh lists,
clearing all four bools first. Never finishes.

**`RayCast2d`** `A:RayCast2d.cs` · 21 / 0
- fields: `fromGameObject`, `fromPosition:FsmVector2`, `direction:FsmVector2`, `space`,
  `distance:FsmFloat`, `minDepth/maxDepth:FsmInt`, `hitEvent`, `storeDidHit`, `storeHitObject`,
  `storeHitPoint`, `storeHitNormal`, `storeHitDistance`, `storeHitFraction`,
  `repeatInterval:FsmInt`, `layerMask:FsmInt[]`, `invertMask:FsmBool`, `debugColor`, `debug`.
- `OnEnter:118-130`: caches the transform, `DoRaycast()`, `Finish()` **only if `repeatInterval == 0`**.
- `OnUpdate:132-139`: `repeat--`; casts when the counter hits 0. (`repeat` is set to
  `repeatInterval` inside `DoRaycast`, so `repeatInterval == 1` casts every frame.)
- `DoRaycast:141-196`: skipped when `|distance| < Mathf.Epsilon`; origin = `fromPosition` +
  transform position (x,y only); length = `+Inf` unless `distance > 0`; direction =
  `direction.normalized`, or `transform.TransformDirection(dir)` for `Space.Self`
  (**note: not renormalized**); `Physics2D.Raycast` with `LayerArrayToLayerMask`, plus the
  depth overload when either depth is set; `Fsm.RecordLastRaycastHit2DInfo`; writes all six
  outputs and sends `hitEvent` on a hit.

**`SetCollider`** `A:SetCollider.cs` · 34 / 8 — `OnEnter:21-32`: first `BoxCollider2D` on the
target → `enabled = active.Value`; `Finish()`. **Dereferences the target without a null check**
(`:25`). Changes what the observation extractor sees.

**`SetBoxColliderTrigger`** `A:SetBoxColliderTrigger.cs` · 7 / 7 — `OnEnter:21-32`:
`BoxCollider2D.isTrigger = trigger.Value`; `Finish()`. Hornet: 6 false, 1 true.

**`SetBoxCollider2DSizeVector`** `A:SetBoxCollider2DSizeVector.cs` · 19 / **19 (all Hornet)** —
`SetDimensions:28-39`: `component.size = size.Value` when `!size.IsNone`,
`component.offset = offset.Value` when `!offset.IsNone`. `OnEnter:41-45` calls it and `Finish()`es.
Hornet resizes her hurtbox per attack state — directly visible in the observation stream.
19 instances, 18 enabled. [D54]

---

## 5. Animation (tk2d)

Clip playback semantics (frame rate, `Playing`, event frames, wrap modes) belong to the tk2d
animator spec; here only the action-side contract.

**`Tk2dPlayAnimation`** `A:Tk2dPlayAnimation.cs` · 180 / 17
- fields: `gameObject:FsmOwnerDefault`, `animLibName:FsmString`, `clipName:FsmString`.
- `OnEnter:39-44`: `_getSprite()` (`GetComponent<tk2dSpriteAnimator>`), `DoPlayAnimation()`,
  **`Finish()` unconditionally**.
- `Do:46-55`: warns and returns if the animator is missing; line `:53` is
  `animLibName.Value.Equals("")` with the result **discarded** — dead code, no effect; then
  `_sprite.Play(clipName.Value)`.

**`Tk2dPlayAnimationWithEvents`** `A:Tk2dPlayAnimationWithEvents.cs` · 50 / 20
- fields: `gameObject`, `clipName`, `animationTriggerEvent:FsmEvent`, `animationCompleteEvent:FsmEvent`.
- `OnEnter:43-47`: `_getSprite()`, `Do…()` — **does not finish**; the action stays active until the
  animator fires completion (or the state exits).
- `Do:49-65`: `_sprite.Play(clipName)`, then assigns `_sprite.AnimationEventTriggered` /
  `AnimationCompleted` delegates (**assignment, not `+=`** — it replaces any existing handler).
- `AnimationEventDelegate:67-73`: writes `Fsm.EventData.IntData/StringData/FloatData` from the
  clip frame's `eventInt`/`eventInfo`/`eventFloat`, then `Fsm.Event(animationTriggerEvent)`.
- `AnimationCompleteDelegate:76-93`: linear scan of `sprite.Library.clips` for the clip index
  (`-1` if not found), writes `Fsm.EventData.IntData`, sends `animationCompleteEvent`.
  **It never calls `Finish()`** — the state advances only via the event's transition.

**`Tk2dWatchAnimationEvents`** `A:Tk2dWatchAnimationEvents.cs` · 56 / 4
- fields: `gameObject`, `animationTriggerEvent`, `animationCompleteEvent`. No clip: it only watches.
- `OnEnter:38-42` registers the same two delegates (`Do:53-68`), does not finish.
- `OnUpdate:44-51`: **`if (!_sprite.Playing) { Fsm.Event(animationCompleteEvent); Finish(); }`** —
  so the completion event can be raised twice (once by the delegate, once by the polling check),
  and `_sprite` is dereferenced without a null guard.

**`Tk2dPlayFrame`** `A:Tk2dPlayFrame.cs` · 28 / 4 — `OnEnter:34-46`: `_getSprite()`; if present
`_sprite.PlayFromFrame(frame.Value)`, else a warning; `Finish()`.

**`Tk2dPlayAnimationV2` / `Tk2dPlayFrameV2` / `SetSpriteRendererSprite`** — outside scope.

---

## 6. HK-specific (gameplay state)

**`SetPlayerDataBool`** `A:SetPlayerDataBool.cs` · 80 / 0
- fields: `boolName:FsmString`, `value:FsmBool`. `OnEnter:21-31`: if `GameManager.instance == null`
  it logs and **returns without `Finish()`** (the action hangs); else
  `GameManager.SetPlayerDataBool(name, value)` → `playerData.SetBool` (`HK:GameManager.cs:849-852`),
  `Finish()`. Writes persistent save state.

**`PlayerDataBoolTest`** `A:PlayerDataBoolTest.cs` · 20 / 1
- fields: `gameObject` (must carry a `GameManager`), `boolName`, `isTrue`, `isFalse`.
- `OnEnter:32-53`: returns early (no `Finish()`) when the target or its `GameManager` is missing;
  else `GetPlayerDataBool` (`HK:GameManager.cs:884-887`) → sends `isTrue`/`isFalse` → `Finish()`.

**`GetPlayerDataInt`** `A:GetPlayerDataInt.cs` · 1 / 1 — `OnEnter:27-41`: `GameManager.GetPlayerDataInt`
(`HK:GameManager.cs:889-892`) into `storeValue`, `Finish()`; returns without finishing if the
`GameManager` is missing.

**`SetInvincible`** `HK:SetInvincible.cs` · 12 / 2 — fields `target:FsmOwnerDefault`,
`Invincible:FsmBool`, `InvincibleFromDirection:FsmInt`. `OnEnter:21-40`: `target.GetSafe(this)` →
`HealthManager`; sets `IsInvincible` and/or `InvincibleFromDirection` (each skipped when `IsNone`);
`Finish()`. Backing setters: `HK:HealthManager.cs:226-236`, `:238-248`.
**Directly drives the `is_invincible` observation column.**

**`SetDamageHeroAmount`** `HK:SetDamageHeroAmount.cs` · 6 / 2 — `OnEnter:18-30`:
`DamageHero.damageDealt = damageDealt.Value` when not `IsNone` (`HK:DamageHero.cs:5`); `Finish()`.
Changes how much HP the knight loses on contact.

**`SetRecoilSpeed`** `HK:SetRecoilSpeed.cs` · 2 / 2 — `OnEnter:18-30`: `Recoil.SetRecoilSpeed(v)`
→ `recoilSpeedBase = newSpeed` (`HK:Recoil.cs:235-238`); `Finish()`. Note this action resolves
`UseOwner` inline (`:20`) rather than via `GetSafe`.

**`SendMessage`** `A:SendMessage.cs` · 91 / 2
- fields: `gameObject:FsmOwnerDefault`, `delivery:{SendMessage,SendMessageUpwards,BroadcastMessage}`,
  `options:SendMessageOptions`, `functionCall:FunctionCall`.
- `OnEnter:37-41`: `DoSendMessage()`, `Finish()`. `Do:43-110`: picks the boxed argument by
  `functionCall.ParameterType` (15-way switch), then dispatches by `delivery`.
- Reflection by name — like `CallMethodProper`, needs a dispatch table. The dumper **does**
  serialise `FunctionCall` (under `value.__fields`). Distinct targets across the four boss scenes:
  `StoryRecord_acquired(string)` ×14, `RelinquishControl()` ×10, `StopAnimationControl()` ×10,
  `SetDarkness(int)` ×6, `RegainControl()` ×5, `StartAnimationControl()` ×5, `TimePasses()` ×5,
  `SetActive(bool)` ×5, `ResetSemiPersistentItems()` ×4, `EnterWithoutInput(bool)` ×4,
  `MaxHealth()` ×4, `AcceptInput()` ×4, `FaceLeft()` ×3, `FreezeMoment(int)` ×2, `FaceRight()` ×2,
  `ColorReturnNeutral()` ×2, and 6 singletons (`AffectedByGravity`, `StopBounce`, `SaveGame`,
  `CharmUpdate`, `RefreshOvercharm`, `flashArmoured`).
- **Hornet's only two instances are both `FreezeMoment(int)` to `_GameManager`**, in
  `Corpse Hornet GG(Clone)/Control/Blow` (both `parameterType="int"`, dump `functionCall.__fields`).
  `GameManager.FreezeMoment(int type)` (`HK:GameManager.cs:2860-2885`) starts a
  `FreezeMoment(rampDown, wait, rampUp, targetScale)` coroutine, i.e. hit-stop via `timeScale`.
  **In the modded game the sim targets, this is inert**: the training/oracle mod IL-hooks
  `GameManager.FreezeMoment*` (`analysis/open-questions.md` Q21, `oracle/Game/TimeScale.cs`), so
  both Hornet calls are no-ops and must be ported as no-ops, not as vanilla hit-stop. [D55]

**`SetFsmBool` / `SetFsmFloat` / `SetFsmString` / `GetFsmInt`** — cross-FSM variable access.
All four share the "resolve the target FSM once, then cache" pattern:
`ActionHelpers.GetGameObjectFsm(go, fsmName)` re-runs whenever the GameObject **or** the fsm name
changed since last call (`GetFsmInt` compares only the GameObject).

| Type | file:lines | write/read | notes |
|---|---|---|---|
| `SetFsmBool` (92/15) | `A:SetFsmBool.cs:43-50 / 52-83 / 85-88` | `FindFsmBool(variableName).Value = setValue` | no-ops if `setValue == null` or target null; warns if FSM/var missing |
| `SetFsmFloat` (29/1) | `A:SetFsmFloat.cs:43-50 / 52-83 / 85-88` | `GetFsmFloat(...).Value = setValue` | same shape (uses `GetFsmFloat`, not `Find`) |
| `SetFsmString` (15/1) | `A:SetFsmString.cs:42-49 / 51-82 / 84-87` | `GetFsmString(...).Value = setValue` | same shape |
| `GetFsmInt` (5/1) | `A:GetFsmInt.cs:38-45 / 47-50 / 52-76` | `storeValue = GetFsmInt(...).Value` | silent when FSM/var missing |

All four: `OnEnter` does the access then `Finish()` unless `everyFrame`; `OnUpdate` repeats.
Hornet: all 15 `SetFsmBool`, the 1 `SetFsmFloat`, 1 `SetFsmString`, 1 `GetFsmInt` are
`everyFrame=false`.

**`ActivateGameObject`** `A:ActivateGameObject.cs` · 155 / 27
- fields: `gameObject`, `activate:FsmBool`, `recursive:FsmBool`, `resetOnExit:bool`, `everyFrame:bool`.
- `OnEnter:37-44`: `DoActivateGameObject()`, `Finish()` unless `everyFrame`. `OnUpdate:46-49`.
- `Do:66-81`: `recursive ? SetActiveRecursively(go, activate) : go.SetActive(activate)`; remembers
  the last target in `activatedGameObject`.
- `SetActiveRecursively:83-90`: sets the object then recurses over **immediate `Transform`
  children** in sibling order (depth-first, parent before children).
- `OnExit:51-64`: if `resetOnExit` and a target was recorded, applies **`!activate`** the same way.
- Hornet: 27 instances (all enabled), all `recursive=false`, `resetOnExit=false`,
  `everyFrame=false`; 16 deactivate / 11 activate. Activation directly changes the collider set the observation sees.

**`ActivateAllChildren`** `A:ActivateAllChildren.cs` · 14 / 5 — `OnEnter:21-32`: iterates the
immediate children of `gameObject.Value` (a plain `FsmGameObject`, not `FsmOwnerDefault`) and calls
`SetActive(activate)` with the plain `bool` field; `Finish()`. Hornet: all 5 `activate=true`.

**`CreateObject`** `A:CreateObject.cs` · 34 / 1 — `OnEnter:50-81`: position/rotation resolved from
`spawnPoint` (+`position` offset, `rotation` overrides `spawnPoint.eulerAngles`) or from
`position`/`rotation` alone; `Object.Instantiate(prefab, pos, Quaternion.Euler(euler))` (`:77`);
`storeObject = clone`; `Finish()`. `networkInstantiate`/`networkGroup` are declared and unused.

**`SpawnObjectFromGlobalPool`** `A:SpawnObjectFromGlobalPool.cs` · 32 / 1 — same position/rotation
logic (default euler is `Vector3.up`, not `zero`, `:46`), but uses the **object pool**:
`gameObject.Value.Spawn(pos, Quaternion.Euler(euler))` (`:69` → `HK:ObjectPoolExtensions.cs:61-64`).
`OnEnter:41-74`, always `Finish()`es.
`ObjectPool.orig_Spawn` (`HK:ObjectPool.cs:472-526`) pops `pooledObjects[prefab][0]` (front of the
list, skipping destroyed entries, `:480-484`) and assigns **only** `transform.parent`,
`localPosition = position` and `localRotation = rotation` (`:487-490`) — note the caller's *world*
position is written to `localPosition`, which coincides only because the `Spawn(pos, rot)` overload
passes `parent = null` (`HK:ObjectPoolExtensions.cs:61-64`). It then either sends the FSM event
`"A SPAWN"` (prefabs with an `ActiveRecycler`, `:491-494`) or `SetActive(true)` (`:497`).
**No other component state is reset anywhere in `orig_Spawn`** — a reused object keeps every field
from its previous life. Pool exhaustion falls back to `Object.Instantiate` with a warning (`:505`);
an unknown prefab calls `CreatePool(prefab, 1)` and recurses (`:523-525`). [D55]

**`DestroyObject`** `A:DestroyObject.cs` · 13 / 1 — `OnEnter:26-45`: `Object.Destroy(go)` when
`delay <= 0` else `Destroy(go, delay)` (`:33`/`:37`); then, if `detachChildren`,
`transform.DetachChildren()` (`:41`) — textually **after** the `Destroy` call; `Finish()`.
`OnUpdate:47-49` is empty. Whether the `DetachChildren` on the next line still observes a live
object (i.e. when `Object.Destroy` actually takes effect) is a Unity semantic no decompiled source
states: **UNKNOWN**, see Q-fsmact-13. [D55]

**`SpawnRandomObjectsV2`** — see §8 (RNG).

**`GetLanguageString`** `A:GetLanguageString.cs` · 25 / 0 — `OnEnter:26-31`:
`Language.Get(convName, sheetName)`, then `Replace("<br>", "\n")`, `Finish()`. Text only.
(Relevant to the P0 wedge history: the `Language` static ctor is a known hazard, but the action
itself has no gameplay effect.)

---

## 7. Audio-visual — NO-OP for the sim, **except the RNG draws**

These actions never touch simulated state. Any that draws from `UnityEngine.Random` **must still be
executed as a draw** so the shared stream stays in phase (§8).

| Type | file:lines | what it does | RNG |
|---|---|---|---|
| `SetMaterialColor` (119/0) | `A:SetMaterialColor.cs:42-50 / 62-65 / 67-96` | `Shader.PropertyToID(namedColor)` once in `OnEnter`, then `material.SetColor`; index 0 uses `renderer.material`, else `renderer.materials[i]` with a write-back of the array | none |
| `GetMaterialColor` (48/0) | `A:GetMaterialColor.cs:40-44 / 46-105` | reads a shader color into `color`; sends `fail` if the property is missing | none |
| `SetTextMeshProColor` (96/0) | `A:SetTextMeshProColor.cs:29-45 / 47-62` | `TextMeshPro.color = color` | none |
| `SetTextMeshProText` (25/0) | `A:SetTextMeshProText.cs:26-39` | `TextMeshPro.text = textString` | none |
| `SetSpriteRenderer` (86/0) | `A:SetSpriteRenderer.cs:20-35` | `SpriteRenderer.enabled = active`; always `Finish()` | none |
| `SetMeshRenderer` (63/11) | `A:SetMeshRenderer.cs:20-35` | `MeshRenderer.enabled = active`; always `Finish()` | none |
| `EaseColor` (33/0) | `A:EaseColor.cs:29-45 / 52-75` (+`A:EaseFsmAction.cs:123-235`) | eases a `FsmColor` variable over `time`; on completion snaps to `to`/`from` per `reverse`, then finishes **one frame later** (`finishInNextStep`) and sends `finishEvent` | none |
| `PlayParticleEmitter` (48/1) | `A:PlayParticleEmitter.cs:21-40` | `ParticleSystem.Play()` when idle and `emit <= 0`, else `Emit(emit)`; `Finish()` | **unknown** — whether the particle system draws from the shared stream is Q-fsmact-2; if it does, this action is not a NO-OP |
| `StopParticleEmitter` (27/0) | `A:StopParticleEmitter.cs:18-33` | `ParticleSystem.Stop()` if playing; `Finish()` | none |
| `AudioPlaySimple` (49/16) | `A:AudioPlaySimple.cs:31-62` | `audio.Play()` when no one-shot clip, then **unconditionally** `PlayOneShot(clip[,volume])` even when `clip == null`; `Finish()` | none |
| `AudioStop` (17/3) | `A:AudioStop.cs:19-31` | `AudioSource.Stop()`; `Finish()` | none |
| `AudioPlayInState` (2/1) | `A:AudioPlayInState.cs:26-45 / 47-53` | `Play()` on enter, `Stop()` on exit; **never finishes** | none |
| `TransitionToAudioSnapshot` (38/3) | `A:TransitionToAudioSnapshot.cs:21-29` | `AudioMixerSnapshot.TransitionTo(t)`; `Finish()` | none |
| `ApplyMusicCue` (23/2) | `A:ApplyMusicCue.cs:27-43` | `GameManager.AudioManager.ApplyMusicCue(...)`; `Finish()` | none |
| **`AudioPlayerOneShotSingle`** (62/3) | `A:AudioPlayerOneShotSingle.cs:42-50 / 52-64 / 66-93` | pooled `Spawn` of an audio player, `Random.Range(pitchMin,pitchMax)` at `:81`, `PlayOneShot` | **1 float draw** per play |
| **`AudioPlayerOneShot`** (13/7) | `A:AudioPlayerOneShot.cs:49-57 / 59-71 / 73-97` | `GetRandomWeightedIndex(weights)` at `:85`, then `Random.Range(pitchMin,pitchMax)` at `:91` | **2 float draws** (weighted index first, then pitch) — but the pitch draw is **inside** `if (audioClip != null)`, so a null clip slot costs only 1 |
| **`AudioPlayRandom`** (5/1) | `A:AudioPlayRandom.cs:34-38 / 40-58` | same two draws (`:47`, `:53`) on the target's own `AudioSource`; early-returns before any draw if `audioClips.Length == 0` | **2 float draws** |

The two `AudioPlayerOneShot*` actions also gate their play on `delay`: `OnEnter` plays immediately
and finishes only when `delay == 0`; otherwise `OnUpdate` accumulates `Time.deltaTime` and plays
once the timer reaches `delay`. All 10 Hornet audio instances have `delay = 0`.

**All Hornet audio instances have `pitchMin == pitchMax == 1.0`** — and the draw still happens:
float `Range(a, a)` always advances the generator (`analysis/open-questions.md` Q15, closed
2026-08-31 against `analysis/dumps/GG_Hornet_1/rng_probe.json`, 6/6 probes). [D53]

---

## 8. RNG — every draw, in order

### 8.1 The draw primitives

- `UnityEngine.Random.Range(float min, float max)` — used by `RandomFloat`, `WaitRandom`,
  `GetRandomWeightedIndex`, the pitch of the three audio actions, `FireAtTarget` spread,
  and four of the five `SpawnRandomObjectsV2` draws.
- `UnityEngine.Random.Range(int min, int max)` — used once, by `SpawnRandomObjectsV2` (spawn count).
- No `System.Random` appears in any of the 105 types.

### 8.2 Draw table

| Type | draws per `OnEnter` | order and ranges | citation |
|---|---|---|---|
| `RandomFloat` | 1 | `Random.Range(min, max)` → `storeResult` | `A:RandomFloat.cs:28` |
| `WaitRandom` | 1 | `time = Random.Range(timeMin, timeMax)`; if `time <= 0` sends `finishEvent` + finishes | `A:WaitRandom.cs:34` |
| `SendRandomEvent` | 1 (0 if `events.Length == 0`) | `GetRandomWeightedIndex(weights)` | `A:SendRandomEvent.cs:28` → `ActionHelpers.cs:80` |
| `SendRandomEventV2` | ≥1, unbounded | 1 per `while` iteration until an index under its `eventMax` is picked | `A:SendRandomEventV2.cs:31` |
| `SendRandomEventV3` | ≥1, ≤(100 − prior `loops`) | 1 per `while` iteration; the missed-max branch can fire on the first draw | `A:SendRandomEventV3.cs:40` |
| `AudioPlayerOneShot` | 2 (1 if the picked clip is null; 0 if `audioClips.Length == 0`) | weighted index, **then** pitch | `A:AudioPlayerOneShot.cs:85, 91` |
| `AudioPlayRandom` | 2 (1 if picked clip null; 0 if no clips) | weighted index, **then** pitch | `A:AudioPlayRandom.cs:47, 53` |
| `AudioPlayerOneShotSingle` | 1 | pitch only (no weighted array) | `A:AudioPlayerOneShotSingle.cs:81` |
| `FireAtTarget` | 1 if `!spread.IsNone`, else 0 | `Random.Range(-spread, spread)` added to the aim angle | `A:FireAtTarget.cs:82` |
| `ShakePositionV2` † | **3 per `UpdateShaking`**, and `UpdateShaking` runs once in `OnEnter` **and** once per `OnUpdate` | `Vector3(Random.Range(-1f,1f), Random.Range(-1f,1f), Random.Range(-1f,1f))` — x, y, z in argument order; the draws happen **before** the duration check, so the terminating call draws too | `A:ShakePositionV2.cs:62, 68, 96, 98` |
| `SpawnRandomObjectsV2` | `1 + 4n` where `n` = the drawn count | `n = Random.Range(spawnMin, spawnMax+1)` **(int)**; then per clone, in this exact order: `originVariationX` → `originVariationY` → `speed` → `angle` | `A:SpawnRandomObjectsV2.cs:86, 96, 101, 109, 110` |

`SpawnRandomObjectsV2` detail (`:86-116`): the X and Y variation draws are guarded by
`originVariationX != null` / `originVariationY != null` — a **reference** check on the `FsmFloat`
object, not `IsNone`, so a configured `0.0` field still draws. Both Hornet instances have
`originVariationX = originVariationY = 0.0` **and** the fields are present, so all four per-clone
draws occur. `originAdjusted` is never reset between clones. Each clone is
`Object.Instantiate` (not pooled), `CacheRigidBody2d`, then
`rb2d.velocity = speed * (cos, sin)` in degrees→radians.

† `ShakePositionV2` is **outside the 105-type scope of this document** — see §8.3 for why, and why
that scope was wrong for RNG purposes.

### 8.3 Draws outside the boss FSMs: camera shake [D05]

**Scope bug.** The backlog in `analysis/specs/fsm-census.md` is its *boss-scene-only* table, which
excludes `DontDestroyOnLoad` FSMs by construction. `_GameCameras/CameraParent` / `CameraShake`
carries `"scene": "DontDestroyOnLoad"` in `analysis/fsm/GG_Hornet_1.json`, so its 15
`ShakePositionV2` instances contribute **0** to that table (they are in the full census: 60
instances / 4 FSMs / 4 scenes, `analysis/specs/fsm-census.md:148`). `ShakePositionV2` therefore
never entered this document's 105-type union — while being, per fight, the **largest single consumer
of the shared RNG stream**. §8.2 above is complete for the 105 types; it is **not** a complete
per-fight draw census, and the P4 "seeded RNG" gate cannot be evaluated on it alone.

**The action** (`A:ShakePositionV2.cs`, an HK-specific action in `Assembly-CSharp`):
- fields: `Target:FsmOwnerDefault`, `Extents:FsmVector3`, `Duration:FsmFloat`, `IsLooping:FsmBool`,
  `StopEvent:FsmEvent`, `FpsLimit:FsmFloat`, `IsCameraShake:FsmBool`.
- `OnEnter:48-63`: `timer = 0`, resolve `Target.GetSafe(this)`, cache
  `startingWorldPosition = target.position`, then call `UpdateShaking()` — **the entry frame already
  draws**. Does not `Finish()`.
- `OnUpdate:65-69`: `UpdateShaking()` every frame. Update callback (no fixed/late opt-in).
- `UpdateShaking:77-111`: `timer += Time.deltaTime` (`:81`); **if `FpsLimit > 0` and
  `Time.unscaledTime < nextUpdateTime` it returns before drawing** (`:82-89`); otherwise
  `num = IsLooping ? 1 : Clamp01(1 - timer/Duration)`, times `ConfigManager.CameraShakeMultiplier`
  when `IsCameraShake` (`:90-95`); then **three `Random.Range(-1f, 1f)` draws** as the x, y, z
  arguments of a `Vector3` (`:96`); writes `target.position = startingWorldPosition + vector*num`
  (`:97`); and only then, if `!IsLooping && timer > Duration`, `StopAndReset()` +
  `Fsm.Event(StopEvent)` + `Finish()` (`:98-103`).
- `OnExit:71-75` → `StopAndReset:113-120` restores `target.position`.

**Under R2 the FPS gate is off.** All 15 instances in the Hornet scene dump have
`FpsLimit = $FPS Limit = 0.0`, so `:82` is false and **every frame draws 3** while the shake state is
active. (This is the same `Time.unscaledTime` path that broke the P0 gate before the oracle pinned
it — STATE.md 08-30 23:40; with `FpsLimit = 0` the wall-clock dependence is gone but the draws
are not.)

The dumped configurations (`analysis/fsm/GG_Hornet_1.json`, `_GameCameras/CameraParent` /
`CameraShake`, all `enabled:true`, all `IsCameraShake=true`, all `FpsLimit=0`):

| state | Extents | Duration | IsLooping | draws while active (dt = 0.02) |
|---|---|---:|---|---|
| `ShakingKill` (`EnemyKillShake`) | 0.105 | 0.5 | false | 25 updates × 3 = **75** (f32 Σ0.02 first exceeds 0.5 at k=25; REVIEW-p1 R2-4) |
| `ShakingAverage` (`AverageShake`) | 0.15 | 1.0 | false | 51 × 3 = **153** |
| `ShakingBig` (`BigShake`) | 0.5 | 1.0 | false | 51 × 3 = **153** |
| `ShakingSmall` (`SmallShake`) | 0.08 | 0.5 | false | 25 × 3 = **75** |
| `ShakingHuge` / `Shaking Super Dash` | 0.65 / 0.25 | 1.0 | false | 51 × 3 = **153** |
| `Blizzard Shake` | 0.15 | 3.0 | false | 151 × 3 = **453** |
| `Tram Shake` | 0.075 | 2.5 | false | 126 × 3 = **378** |
| `Rumbling{Small,Med,Big,Huge,Focus,Focus 2,Fall}` | 0.015–1.0 | 1.0 | **true** | **3 per frame, unbounded** — `IsLooping` skips the `timer > Duration` exit entirely (`:98`), so these end only when an event leaves the state |

Update counts are `k` such that the accumulated `timer` first exceeds `Duration`; `timer` is an f32
running sum of `Time.deltaTime`, so the exact `k` follows the same f32 accumulation the
`Wait`/tk2d timing analyses use (`analysis/specs/tk2d-animator.md`,
`analysis/specs/fsm-runtime.md`) — the
numbers above are `ceil(Duration/dt) + 1` and must be confirmed against a trace, not assumed.

**Who fires it in a Hornet fight.** The `CameraShake` FSM's *global* transitions include
`EnemyKillShake → To Kill Shake` and `AverageShake → To Average Shake` (dump `globalTransitions`).
Senders reaching it in `GG_Hornet_1` (68 `SendEventByName` sites for shake events: 35 `AverageShake`,
26 `EnemyKillShake`, 7 `BigShake`):
- **Hornet's attacks** — `Control/G Dash` idx 3, `Control/A Dash` idx 2, `Control/Sphere` idx 3,
  `Control/Sphere A` idx 3 all `SendEventByName "EnemyKillShake"` with
  `eventTarget.target = GameObject`, `gameObject = _GameCameras/CameraParent` (verified in the dump's
  `eventTarget.__fields`) → `Fsm.BroadcastEventToGameObject` (`PM:Fsm.cs:2142-2147`).
  **≈ 75 draws per Hornet attack.** (`Control/Throw` idx 3 is a fifth site but `enabled:false`.)
- **Every hit the knight lands** — `HK:HealthManager.cs:372` calls
  `GameCameras.instance.cameraShakeFSM.SendEvent("EnemyKillShake")` from C#, alongside
  `GameManager.instance.FreezeMoment(1)` on the line above (`HK:HealthManager.cs:371`).
  ≈ 75 draws per landed nail hit.
- **Every hit the knight takes** — `Knight/Effects/Damage Effect` / `Knight Damage` / `Gen` sends
  `AverageShake` (`analysis/fsm/GG_Hornet_1.json`, `SendEventByName`, `enabled:true`).
  **≈ 153 draws per knight hit.**
- Knight abilities the agent can use also fire it: `Nail Arts` (`Activate Slash`, `Dash Slash`,
  `G Slash`), `Spell Control` (`Focus Heal`, `Focus Heal 2`, `Quake Antic`), `Superdash`
  (`Air Cancel`, `Ground Charged`, `Wall Charged`), `Dream Nail` (11 sites), `Hero Death Anim`,
  `Thorn Counter`, `Blocker Shield` (all from `analysis/fsm/GG_Hornet_1.json`).
- Hornet's corpse (`Corpse Hornet GG(Clone)/Control/Blow` idx 11) sends `AverageShake` on death
  (`analysis/fsm/GG_Hornet_1.json`).

**Consequence.** In a live fight the camera-shake stream dominates: a single Hornet dash (≈75) plus
one landed nail hit (≈75) plus one knight hit (≈153) is ≈303 draws, against the ≈19 audio draws and
the handful of decision draws catalogued in §8.2. Any sim that omits `CameraShake` desynchronises
the stream within the first attack. Reproducing it requires the `CameraShake` FSM, `ShakePositionV2`,
and the `Time.deltaTime` accumulation — but **not** the position writes, which are camera-only.
The full reachability census is Q-fsmact-10.

### 8.4 Why draw order is fragile

An action that sends an event can transition the FSM synchronously (`PM:Fsm.cs:2126-2182` →
`ProcessEvent` `:2023+`), and `FsmState.ActivateActions` then **skips every remaining action in the
state** (`PM:FsmState.cs:307-310`). So the number of draws taken on entering a state depends on
which branch the *earlier* draws chose.

Worked example — Hornet `Control` state **`Idle`** (`analysis/fsm/GG_Hornet_1.json`,
`Boss Holder/Hornet Boss 1` / `Control`; `isSequence=false`; transitions `RUN`→`Flip?`,
`FINISHED`→`G Sphere?`, `TOOK DAMAGE`→`Dmg Response`, `EVADE`→`Evade Antic`). Nine enabled actions:
`FaceObject`, `SetFloatValue`, `SetBoxCollider2DSizeVector`, `Tk2dPlayAnimation`, `SetVelocity2d`,
`BoolTest`, **`SendRandomEvent`** (`events=[<none>, RUN]`, `weights=[0.5,0.5]`),
**`SendRandomEventV2`** (`events=[IDLE, RUN]`, `weights=[0.5,0.5]`, `trackingInts=[Ct Idle, Ct Run]`,
`eventMax=[2,2]`), **`WaitRandom`** (`timeMin=$Idle Wait Min`, `timeMax=$Idle Wait Max`).
- If `SendRandomEvent` picks slot 1 (`RUN`), the FSM transitions inside its `OnEnter` and the last
  two actions never run → **1 draw** on this entry.
- If it picks slot 0 (a `None` event → `Fsm.Event(null)` is a no-op, `PM:Fsm.cs:2192-2198`),
  `SendRandomEventV2` runs (≥1 draw) and then `WaitRandom` runs (1 draw) → **≥3 draws**.

Every other Hornet RNG site, verbatim from the dump (`~` = `Boss Holder/Hornet Boss 1`):

| FSM / state | action | parameters |
|---|---|---|
| `~` `Control` / `Idle` | `SendRandomEvent` | `events=[none, RUN]`, `weights=[0.5, 0.5]`, `delay=0` |
| `~` `Control` / `Idle` | `SendRandomEventV2` | `events=[IDLE, RUN]`, `weights=[0.5,0.5]`, `eventMax=[2,2]` |
| `~` `Control` / `Idle` | `WaitRandom` | `[$Idle Wait Min=0.5, $Idle Wait Max=0.75]` |
| `~` `Control` / `Run` | `WaitRandom` | `[$Run Wait Min=0.5, $Run Wait Max=1.0]` |
| `~` `Control` / `Flip?` | `SendRandomEvent` | `events=[none, FINISHED]`, `weights=[0.5,0.5]` |
| `~` `Control` / `Aim Jump` | `RandomFloat` | `[$Left X=16.06, $Right X=36.53]` → `$Jump X` |
| `~` `Control` / `Aim Sphere Jump` | `RandomFloat` | `[$Left X, $Right X]` → `$Jump X` |
| `~` `Control` / `Jump` | `RandomFloat` | `[41.0, 41.0]` → `$Jump Y` |
| `~` `Control` / `Set ADash` | `RandomFloat` | `[0.15, 0.4]` → `$Air Dash Pause` |
| `~` `Control` / `Move Choice A` | `SendRandomEventV3` | `events=[AIRDASH, SPHERE A, G DASH, THROW]`, `weights=[.25,.25,.25,.25]`, `eventMax=[2,1,2,1]`, `missedMax=[5,7,5,3]` |
| `~` `Control` / `Move Choice B` | `SendRandomEventV3` | `events=[AIRDASH, SPHERE A, G DASH]`, `weights=[.33,.33,.34]`, `eventMax=[2,1,2]`, `missedMax=[5,7,5]` |
| `~` `Control` / `G Sphere?` | `SendRandomEventV2` | `events=[SPHERE G, FINISHED]`, `weights=[0.2, 0.8]`, `eventMax=[1,5]` |
| `~` `Control` / `Dmg Response` | `SendRandomEvent` | `events=[EVADE, JUMP, ATTACK, IDLE]`, `weights=[.3,.15,.15,.4]` |
| `~` `Control` / `After Evade` | `SendRandomEvent` | `events=[ATTACK, IDLE]`, `weights=[0.5, 0.5]` |
| `~` `Control` / `Dmg Idle` | `WaitRandom` | `[0.25, 0.4]` |
| `~` `Control` / `Fire` | `FireAtTarget` | **`enabled=false`**, `spread=0` — never executes, never draws |
| `~/Evade Range` `Fluctuate` / `Off` | `WaitRandom` | `[2.0, 3.0]` |
| `~/Evade Range` `Fluctuate` / `On` | `WaitRandom` | `[1.0, 2.0]` |
| `~/Corpse Hornet GG(Clone)` `Control` / `Blow` | `SpawnRandomObjectsV2` ×2 | `spawn=[8,8]` and `[2,3]`; `speed=[2,35]`, `angle=[0,360]`, `originVariation=0/0` → 33 and 9–13 draws |
| audio: `GDash Antic`, `Jump`, `ADash Antic`, `Sphere Antic G`, `Sphere Antic A`, `Throw Antic`, `Evade` | `AudioPlayerOneShot` ×7 | 2–4 clips, uniform weights, `pitch=[1,1]` → **2 draws each** |
| `Stun Start` | `AudioPlayRandom` | 2 clips, `pitch=[1,1]` → **2 draws** |
| `Flourish`, `GG Fall`, `Corpse…/Blow` | `AudioPlayerOneShotSingle` ×3 | `pitch=[1,1]` → **1 draw each** |

**Consequence for the sim: the audio actions cannot be dropped.** Eleven of Hornet's 31 RNG action
instances are audio (7 `AudioPlayerOneShot` × 2 draws + 3 `AudioPlayerOneShotSingle` × 1 +
1 `AudioPlayRandom` × 2 = **19 draws** if every site fires once), interleaved with the combat draws
in the same stream. All eleven have `pitchMin == pitchMax`, which does **not** save a draw
(Q15). [D53]

---

## 9. Master table — sim relevance and implementation size

Relevance: **STATE** (FSM/PlayerData/component variables), **PHYSICS** (rb2d/transform/collider),
**ANIM** (tk2d), **RNG** (draws), **EVENT** (event dispatch/transitions), **NOOP** (audio/visual
only). Size: XS ≤10 LOC, S ≤25, M ≤60, L >60 or needs an external subsystem.

| # | Type | boss-scene | Hornet | relevance | size |
|---:|---|---:|---:|---|---|
| 1 | `SendEventByName` | 195 | 14 | EVENT | S |
| 2 | `Wait` | 185 | 12 | STATE, EVENT | XS |
| 3 | `Tk2dPlayAnimation` | 180 | 17 | ANIM | XS |
| 4 | `BoolTest` | 160 | 30 | EVENT | XS |
| 5 | `ActivateGameObject` | 155 | 27 | STATE, PHYSICS | S |
| 6 | `SetMaterialColor` | 119 | 0 | NOOP | XS |
| 7 | `FloatCompare` | 99 | 8 | EVENT | XS |
| 8 | `SetTextMeshProColor` | 96 | 0 | NOOP | XS |
| 9 | `SetScale` | 94 | 27 | PHYSICS | S |
| 10 | `GetPosition` | 92 | 8 | STATE | XS |
| 11 | `SetFsmBool` | 92 | 15 | STATE | S |
| 12 | `SendMessage` | 91 | 2 | STATE (dispatch table) | L |
| 13 | `SetVelocity2d` | 91 | 23 | PHYSICS | S |
| 14 | `SetSpriteRenderer` | 86 | 0 | NOOP | XS |
| 15 | `SetPlayerDataBool` | 80 | 0 | STATE | XS |
| 16 | `SetFloatValue` | 74 | 14 | STATE | XS |
| 17 | `FindChild` | 71 | 3 | STATE | XS |
| 18 | `GetOwner` | 64 | 14 | STATE | XS |
| 19 | `SetPosition` | 64 | 11 | PHYSICS | S |
| 20 | `NextFrameEvent` | 63 | 7 | EVENT | XS |
| 21 | `SetMeshRenderer` | 63 | 11 | NOOP | XS |
| 22 | `AudioPlayerOneShotSingle` | 62 | 3 | **RNG**, NOOP | XS |
| 23 | `CallMethodProper` | 56 | 0 | STATE (dispatch table) | L |
| 24 | `FloatAdd` | 56 | 0 | STATE | XS |
| 25 | `Tk2dWatchAnimationEvents` | 56 | 4 | ANIM, EVENT | S |
| 26 | `Tk2dPlayAnimationWithEvents` | 50 | 20 | ANIM, EVENT | S |
| 27 | `AudioPlaySimple` | 49 | 16 | NOOP | XS |
| 28 | `GetMaterialColor` | 48 | 0 | NOOP | XS |
| 29 | `PlayParticleEmitter` | 48 | 1 | NOOP | XS |
| 30 | `SendEventToRegister` | 48 | 0 | EVENT (global) | S |
| 31 | `SetGravity2dScale` | 42 | 11 | PHYSICS | XS |
| 32 | `RandomFloat` | 39 | 4 | **RNG**, STATE | XS |
| 33 | `TransitionToAudioSnapshot` | 38 | 3 | NOOP | XS |
| 34 | `Trigger2dEvent` | 37 | 14 | EVENT, PHYSICS | M |
| 35 | `SetParent` | 35 | 8 | PHYSICS | XS |
| 36 | `CreateObject` | 34 | 1 | STATE, PHYSICS | S |
| 37 | `GetScale` | 34 | 13 | STATE | XS |
| 38 | `SetCollider` | 34 | 8 | PHYSICS | XS |
| 39 | `EaseColor` | 33 | 0 | NOOP (STATE: writes an FSM color var) | M |
| 40 | `SetRotation` | 33 | 16 | PHYSICS | S |
| 41 | `SetColorValue` | 32 | 0 | NOOP | XS |
| 42 | `SpawnObjectFromGlobalPool` | 32 | 1 | STATE, PHYSICS | S |
| 43 | `CheckCollisionSide` | 31 | 7 | PHYSICS, EVENT | M |
| 44 | `CheckCollisionSideEnter` | 31 | 6 | PHYSICS, EVENT | M |
| 45 | `IntCompare` | 30 | 7 | EVENT | XS |
| 46 | `SetFsmFloat` | 29 | 1 | STATE | S |
| 47 | `WaitRandom` | 29 | 5 | **RNG**, STATE, EVENT | XS |
| 48 | `SetGameObject` | 28 | 2 | STATE | XS |
| 49 | `Tk2dPlayFrame` | 28 | 4 | ANIM | XS |
| 50 | `SetBoolValue` | 27 | 7 | STATE | XS |
| 51 | `StopParticleEmitter` | 27 | 0 | NOOP | XS |
| 52 | `GetLanguageString` | 25 | 0 | NOOP | XS |
| 53 | `GetParent` | 25 | 11 | STATE | XS |
| 54 | `SetTextMeshProText` | 25 | 0 | NOOP | XS |
| 55 | `FloatMultiply` | 24 | 3 | STATE | XS |
| 56 | `ApplyMusicCue` | 23 | 2 | NOOP | XS |
| 57 | `SetIntValue` | 23 | 4 | STATE | XS |
| 58 | `GetVelocity2d` | 21 | 1 | STATE, PHYSICS | XS |
| 59 | `RayCast2d` | 21 | 0 | PHYSICS, EVENT | M |
| 60 | `FindGameObject` | 20 | 2 | STATE | S |
| — | `SetBoxCollider2DSizeVector` | 19 | 19 | PHYSICS | XS |
| — | `FaceObject` | 13 | 11 | PHYSICS, ANIM | M |
| — | `AudioPlayerOneShot` | 13 | 7 | **RNG**, NOOP | S |
| — | `SetBoxColliderTrigger` | 7 | 7 | PHYSICS | XS |
| — | `FloatInRange` | 8 | 6 | STATE, EVENT | XS |
| — | `ActivateAllChildren` | 14 | 5 | STATE, PHYSICS | XS |
| — | `IntAdd` | 12 | 4 | STATE | XS |
| — | `SendRandomEvent` | 8 | 4 | **RNG**, EVENT | XS |
| — | `FloatOperator` | 16 | 4 | STATE | S |
| — | `FloatTestToBool` | 6 | 4 | STATE | S |
| — | `SetIsKinematic2d` | 11 | 4 | PHYSICS | XS |
| — | `DecelerateXY` | 3 | 3 | PHYSICS | S |
| — | `BoolTestMulti` | 7 | 3 | STATE, EVENT | S |
| — | `DecelerateV2` | 6 | 3 | PHYSICS | S |
| — | `AudioStop` | 17 | 3 | NOOP | XS |
| — | `SpawnRandomObjectsV2` | 2 | 2 | **RNG**, PHYSICS | M |
| — | `FloatClamp` | 11 | 2 | STATE | XS |
| — | `IntOperator` | 5 | 2 | STATE | S |
| — | `SendRandomEventV2` | 4 | 2 | **RNG**, EVENT, STATE | S |
| — | `FloatSubtract` | 14 | 2 | STATE | XS |
| — | `GetAngleToTarget2D` | 3 | 2 | STATE | XS |
| — | `SetVelocityAsAngle` | 13 | 2 | PHYSICS | S |
| — | `SetRecoilSpeed` | 2 | 2 | STATE | XS |
| — | `FlipScale` | 2 | 2 | PHYSICS | XS |
| — | `SendEvent` | 4 | 2 | EVENT | S |
| — | `SendRandomEventV3` | 2 | 2 | **RNG**, EVENT, STATE | M |
| — | `SetInvincible` | 12 | 2 | STATE (i-frames) | XS |
| — | `SetDamageHeroAmount` | 6 | 2 | STATE (damage) | XS |
| — | `DestroyObject` | 13 | 1 | STATE | XS |
| — | `iTweenScaleTo` | 1 | 1 | PHYSICS (needs iTween) | L |
| — | `IntAddV2` | 2 | 1 | STATE (FixedUpdate) | XS |
| — | `GetTag` | 2 | 1 | STATE | XS |
| — | `PlayerDataBoolTest` | 20 | 1 | STATE, EVENT | XS |
| — | `FireAtTarget` | 1 | 1 | **RNG**, PHYSICS | S |
| — | `FaceAngle` | 2 | 1 | PHYSICS | XS |
| — | `GetRotation` | 2 | 1 | STATE | XS |
| — | `Vector3AddXYZ` | 1 | 1 | STATE | XS |
| — | `AudioPlayInState` | 2 | 1 | NOOP | XS |
| — | `CheckTargetDirection` | 17 | 1 | STATE, EVENT | S |
| — | `AudioPlayRandom` | 5 | 1 | **RNG**, NOOP | S |
| — | `GGCheckIfBossScene` | 15 | 1 | EVENT | XS |
| — | `GetPlayerDataInt` | 1 | 1 | STATE | XS |
| — | `GetFsmInt` | 5 | 1 | STATE | S |
| — | `SetFsmString` | 15 | 1 | STATE | S |
| — | `Translate` | 3 | 1 | PHYSICS | M |

Totals: 105 types. **NOOP-only: 15.** RNG-drawing: **10** (within scope; `ShakePositionV2` is an
11th drawing type that this scope wrongly excluded — §8.3).
XS: 64, S: 29, M: 9, L: 3
(`SendMessage`, `CallMethodProper`, `iTweenScaleTo` — each needs an external subsystem or a
name-dispatch table plus a trap).

---

## 10. Hornet action types **not** in the top-60 (45 types, one line each)

Each is fully specified above; this is the coverage checklist. Counts are `boss-scene / Hornet`.

| Type | b/H | one-line semantics |
|---|---|---|
| `SetBoxCollider2DSizeVector` | 19/19 | `OnEnter` writes `BoxCollider2D.size`/`.offset` (each skipped when `IsNone`), finishes — `A:…:28-45`. |
| `FaceObject` | 13/11 | flips `objectA.localScale.x` to face `objectB`; optional `PlayFromFrame(0)`/`Play(clip)` only when the sign changes — `A:…:45-134`. |
| `AudioPlayerOneShot` | 13/7 | pooled audio player; **2 RNG draws** (weighted clip index, then pitch); `delay` gates via `Time.deltaTime` — `A:…:49-97`. |
| `SetBoxColliderTrigger` | 7/7 | `BoxCollider2D.isTrigger = trigger`; finishes — `A:…:21-32`. |
| `FloatInRange` | 8/6 | inclusive range test → bool + `trueEvent`/`falseEvent`; `everyFrame` repeats — `A:…:38-67`. |
| `ActivateAllChildren` | 14/5 | `SetActive(activate)` on immediate children of `gameObject.Value`; finishes — `A:…:21-32`. |
| `IntAdd` | 12/4 | `intVariable += add`; `everyFrame` repeats — `A:…:23-35`. |
| `SendRandomEvent` | 8/4 | **1 draw** (`GetRandomWeightedIndex`), sends the picked event (delay <0.001 → immediate) — `A:…:24-52`. |
| `FloatOperator` | 16/4 | Add/Sub/Mul/Div/Min/Max into `storeResult`; unguarded divide — `A:…:47-86`. |
| `FloatTestToBool` | 6/4 | NOT `FloatCompare` semantics: three INDEPENDENT sequential if/else blocks, tolerance only on the first — `equalBool = |f1-f2| <= tolerance` (`A:FloatTestToBool.cs:60-66`), `lessThanBool = f1 < f2` raw (`:68-74`), `greaterThanBool = f1 > f2` raw (`:76-82`); each bool is written false on its negative branch, so within-tolerance-but-unequal inputs set equalBool AND one of less/greater in the same call (impossible under `FloatCompare`'s exclusive chain `A:FloatCompare.cs:60-68`). Sends nothing. (Correction 2026-08-31, decomp reader delta.) |
| `SetIsKinematic2d` | 11/4 | `rigidbody2d.isKinematic = value`; finishes — `A:…:24-37`. |
| `DecelerateXY` | 3/3 | **FixedUpdate**, never finishes; per-axis `v *= decel` with a 0.001 deadzone — `A:…:24-99`. |
| `BoolTestMulti` | 7/3 | AND over paired bool arrays → `storeResult` + `trueEvent`/`falseEvent` — `A:…:35-73`. |
| `DecelerateV2` | 6/3 | **FixedUpdate**, never finishes; `v *= deceleration` per axis, sign-flip clamp — `A:…:21-82`. |
| `AudioStop` | 17/3 | `AudioSource.Stop()`; finishes. NO-OP — `A:…:19-31`. |
| `SpawnRandomObjectsV2` | 2/2 | **1 int + 4×n float draws**; instantiates n clones and sets each `rb2d.velocity` from speed/angle — `A:…:67-119`. |
| `FloatClamp` | 11/2 | `Mathf.Clamp(var, min, max)`; `everyFrame` repeats — `A:…:33-50`. |
| `IntOperator` | 5/2 | integer Add/Sub/Mul/Div/Min/Max into `storeResult` — `A:…:42-81`. |
| `SendRandomEventV2` | 4/2 | **≥1 draw**, loops until an index under `eventMax`; zeroes all `trackingInts` then restores the winner — `A:…:26-45`. |
| `FloatSubtract` | 14/2 | `var -= subtract [* Time.deltaTime]` — `A:…:32-56`. |
| `GetAngleToTarget2D` | 3/2 | `Atan2(dy+offsetY, dx+offsetX)` in degrees, normalised to `[0,360)` by a `while` loop — `A:…:41-77`. |
| `SetVelocityAsAngle` | 13/2 | **FixedUpdate**; `rb2d.velocity = speed*(cos,sin)` from degrees — `A:…:40-80`. |
| `SetRecoilSpeed` | 2/2 | `Recoil.SetRecoilSpeed(v)` → `recoilSpeedBase`; finishes — `HK:SetRecoilSpeed.cs:18-30`. |
| `FlipScale` | 2/2 | negates `localScale.x`/`.y` per plain bool fields; `lateUpdate` has **no** opt-in — `A:…:30-75`. |
| `SendEvent` | 4/2 | send now if `delay < 0.001f`, else `DelayedEvent`; `everyFrame` resends forever — `A:…:35-64`. |
| `SendRandomEventV3` | 2/2 | **≥1 draw**; missed-max branch forces the **highest** over-limit index (scan has no `break`, `A:…:43-50`), then the V2 branch; cumulative `loops > 100` escape — `A:…:33-86`. |
| `SetInvincible` | 12/2 | `HealthManager.IsInvincible` / `.InvincibleFromDirection` — `HK:SetInvincible.cs:21-40`. |
| `SetDamageHeroAmount` | 6/2 | `DamageHero.damageDealt = value` — `HK:SetDamageHeroAmount.cs:18-30`. |
| `DestroyObject` | 13/1 | `Object.Destroy(go[, delay])`, then optional `DetachChildren()`; finishes — `A:…:26-45`. |
| `iTweenScaleTo` | 1/1 | hands scale animation to the iTween runtime; never self-finishes — `A:…:60-88`. |
| `IntAddV2` | 2/1 | `IntAdd` on **FixedUpdate** (`OnPreprocess` opt-in) — `A:…:23-40`. |
| `GetTag` | 2/1 | `storeResult = go.tag` — `A:…:23-43`. |
| `PlayerDataBoolTest` | 20/1 | `GameManager.GetPlayerDataBool(name)` → `isTrue`/`isFalse`; no finish if GM missing — `A:…:32-53`. |
| `FireAtTarget` | 1/1 | **FixedUpdate**; aims at target, optional `Random.Range(-spread, spread)`; Hornet's instance is disabled — `A:…:43-91`. |
| `FaceAngle` | 2/1 | **FixedUpdate**; `localEulerAngles.z = Atan2(v.y, v.x) + angleOffset` — `A:…:28-62`. |
| `GetRotation` | 2/1 | writes quaternion + euler + three floats, world or local — `A:…:44-82`. |
| `Vector3AddXYZ` | 1/1 | `v += (x,y,z) [* Time.deltaTime]` — `A:…:33-58`. |
| `AudioPlayInState` | 2/1 | `Play()` on enter, `Stop()` on exit; **never finishes**. NO-OP — `A:…:26-53`. |
| `CheckTargetDirection` | 17/1 | four independent strict comparisons of self vs target x/y → four bools + four events — `A:…:48-109`. |
| `AudioPlayRandom` | 5/1 | **2 draws** (weighted clip, pitch) on the target's own `AudioSource`. NO-OP otherwise — `A:…:34-58`. |
| `GGCheckIfBossScene` | 15/1 | branches on the static `BossSceneController.IsBossScene` — `HK:GGCheckIfBossScene.cs:16-27`. |
| `GetPlayerDataInt` | 1/1 | `GameManager.GetPlayerDataInt(name)` → `storeValue` — `A:…:27-41`. |
| `GetFsmInt` | 5/1 | reads an int variable from another FSM (caches the FSM by GameObject only) — `A:…:38-76`. |
| `SetFsmString` | 15/1 | writes a string variable in another FSM — `A:…:42-87`. |
| `Translate` | 3/1 | `transform.Translate(v [* Time.deltaTime], space)`; four independent callback flags — `A:…:64-144`. |

---

## 11. Open questions

Format per the P1 review: `Q-fsmact-<n>`. Three questions from the first draft (float/int
`Random.Range(a,a)`, the generator algorithm, shared streams) are **CLOSED** by
`analysis/open-questions.md` Q15 and are not restated here — see the closure note below. [D53]

**CLOSED → Q15** (`analysis/open-questions.md`, closed 2026-08-31 against
`analysis/dumps/GG_Hornet_1/rng_probe.json`): `UnityEngine.Random` is Marsaglia xorshift128 over the
recorded `Random.state` `(x,y,z,w)`, one draw = one step, `value = (float)(w & 0x7FFFFF)/8388607.0f`;
`Range(float lo, hi)` **always draws** (including `lo == hi`) and returns
`(float)(u*lo) + (float)((1-u)*hi)`; `Range(int lo, hi)` with `lo == hi` returns `lo`
**without drawing**; int and float share one stream; `InitState(seed)` expansion pinned. Effects on
this document: all 11 Hornet audio sites and `Control/Jump`'s `RandomFloat [41,41]` **do** draw;
`SpawnRandomObjectsV2`'s `Range(spawnMin, spawnMax+1)` draws for `[8,9]` but its second instance's
`[2,4]` likewise draws; a hypothetical `spawnMin == spawnMax` int site would not. §8.2 already
reflects this.

### Q-fsmact-1 — `GameObject.Find` / `FindGameObjectsWithTag` iteration order
`A:FindGameObject.cs:34-60` returns the first tag match whose `name` equals `objectName`, and
otherwise `GameObject.Find(objectName)`; both orders are Unity-internal and stated nowhere in the
decomp. 20 boss-scene instances. Either an experiment pins the order, or each of the 20 call sites is
shown to have a unique match (in which case order is irrelevant and the sim can use any stable
lookup). Until then the sim must not assume scene-graph order.

### Q-fsmact-2 — Does `ParticleSystem` draw from the shared `UnityEngine.Random` stream?
`A:PlayParticleEmitter.cs:31/35` calls `ParticleSystem.Play()` / `Emit(n)`; 48 boss-scene instances,
1 on Hornet. Unity particle systems carry their own `randomSeed`/`useAutoRandomSeed`, which suggests
a separate stream, but no decompiled source states it. If the streams are shared, `PlayParticleEmitter`
and `StopParticleEmitter` are **not** NO-OPs and §7 and the §9 relevance column are wrong for them.
Same probe methodology as Q15 (snapshot `Random.state` around a `Play()`).

### Q-fsmact-3 — Unity's `SendMessage` / `BroadcastMessage` receiver ordering
`A:SendMessage.cs:97-108`. Matters only when several components on one GameObject implement the same
handler name. 91 boss-scene instances. Hornet's two are `FreezeMoment(int)` on `_GameManager`
(single receiver), so Hornet is unaffected and this does not block P4 for Hornet.

### Q-fsmact-4 — `GameObject.SetActive` → `OnEnable` / `Start` synchrony
`ActivateGameObject.SetActiveRecursively` (`A:ActivateGameObject.cs:83-90`) walks immediate children
in sibling order, parent before children; `ActivateAllChildren` (`A:…:21-32`) walks one level.
Whether Unity runs `OnEnable` (and therefore `PlayMakerFSM.OnEnable` → `Fsm.OnEnable`, which restarts
an FSM whose `RestartOnEnable` is set) **synchronously inside the `SetActive` call** is not settled by
any decompiled source. This is load-bearing, not academic: per REVIEW-p1 D03, Hornet's
`Control/Throw` runs `SetPosition($Needle)` at action index 6 and `ActivateGameObject($Needle, true)`
at index 7, and `Needle`'s own `Control` FSM has `restartOnEnable:true` — so whether the restarted
FSM's `Init` reads the position written one action earlier depends entirely on this answer.

### Q-fsmact-5 — Lifetime and scope of `Fsm.RecordLastRaycastHit2DInfo`
`A:RayCast2d.cs:175` writes an FSM-level "last raycast hit" that a later `GetRaycastHit2dInfo` reads.
None of the 105 types here reads it, but other boss-scene FSMs may. Whether the store is per-`Fsm`,
per-`FsmState`, or process-wide, and when it is cleared, is unresolved; the sim should trap on the
first read rather than model it speculatively.

### Q-fsmact-6 — `Physics2D.Raycast` tie-breaking and `queriesHitTriggers`
`CheckCollisionSide` (`A:…:176, 194, 212, 232`) and `CheckCollisionSideEnter` (`A:…:122, 132, 142, 152`)
take the single `RaycastHit2D` returned by the non-allocating overload and then filter triggers
**after** the cast — so a trigger collider can shadow a solid one and silently suppress a hit,
depending on the project's `Physics2D.queriesHitTriggers` and on which of several overlapping
colliders Unity returns. Neither is in these sources; both must come from
`analysis/dumps/<scene>/physics.json` plus an experiment (cf. `analysis/open-questions.md` Q16, which
already owns Box2D contact behaviour).

### Q-fsmact-7 — iTween semantics
`A:iTweenScaleTo.cs:86` hands off to `iTween.ScaleTo` with an option hash (easing curve, `time` vs
`speed`, `delay`, loop type, `ignoretimescale`) and never self-finishes; completion arrives as the
`iTweenOnComplete` message. One instance in the four scenes — Hornet's `Sphere Ball` growth, which is
a **live hitbox radius**, so it is not cosmetic. The sim should trap on it until a trace shows the
required fidelity.

### Q-fsmact-8 — Completeness of the `SendMessage` / `CallMethodProper` dispatch tables
The tables in §1 and §6 enumerate every target in the four dumped boss scenes (23 distinct
`behaviour.method` pairs, 22 distinct `functionName(parameterType)` pairs) and nothing more. A new
scene can introduce new targets. Per PLAN §2.3 the sim must **trap** on an unlisted pair rather than
no-op it; this question stays open for as long as new scenes can be added.

### Q-fsmact-9 — Actions that can hang a state, and whether the sim should reproduce the hang
`SetPlayerDataBool` (`A:…:24-28`), `PlayerDataBoolTest` (`A:…:35-38`) and `GetPlayerDataInt`
(`A:…:30-31`) return **without** `Finish()` when the `GameManager` is missing, and
`SendRandomEventV2` (`A:…:29-43`) loops forever — drawing once per iteration — when the weights sum
to zero or the draw lands on the sum (Q15 / D56). All are unreachable in a healthy scene. Whether the
port reproduces the hang bit-for-bit or traps is a policy call for the orchestrator, not a fact to
discover.

### Q-fsmact-10 — Full RNG reachability census beyond the boss FSMs
§8.3 shows the boss-scene-only census silently excluded `DontDestroyOnLoad` FSMs and with them
`ShakePositionV2`, the largest per-fight consumer of the shared stream (≈75 draws per Hornet attack,
≈78 per landed nail hit, ≈153 per knight hit, unbounded while a `Rumbling*` state is active). The
same exclusion may hide other drawing actions on persistent objects. What is needed: a census of
`UnityEngine.Random` call sites over **every** FSM in `analysis/fsm/<scene>.json` (not just
boss-scene ones) **plus** the C# call sites that send events into them (e.g. `HealthManager.cs:372`),
ranked by expected draws per fight. **The P4 "seeded RNG" gate cannot be evaluated until this
exists.** Note this is scoped to fsm-actions; `analysis/open-questions.md` Q14 already tracks the
shake issue at project level.

### Q-fsmact-11 — What `Transform.parent` does to the world pose
`SetParent` (`A:SetParent.cs:35`) assigns `transform.parent` and, only when the corresponding
`FsmBool` is set, `localPosition = Vector3.zero` (`:38`) / `localRotation = identity` (`:42`). The
action writes nothing else, so whether reparenting preserves or changes the world pose is entirely a
Unity `Transform.parent` semantic, and **no decompiled source in `analysis/decomp/` states it**. The
first draft of this document asserted "reparenting changes world transform unless the offsets are
reset"; that was uncited and is withdrawn (REVIEW-p1 D04). 35 boss-scene instances, 8 on Hornet.
Must be pinned by experiment before any of them is ported.

### Q-fsmact-12 — Where 2D trigger/collision callbacks land inside the frame
`Trigger2dEvent`, `CheckCollisionSide` and `CheckCollisionSideEnter` do their work from
`PlayMakerUnity2DProxy`'s `OnTriggerEnter2D` / `OnCollisionEnter2D` / `OnCollisionStay2D`
(`HK:PlayMakerUnity2DProxy.cs:143, 190`), not from `OnUpdate` — that much is cited. Their position
in the frame relative to `FixedUpdate`, `Update` and the FSM's own callbacks is a Unity semantic;
it belongs to `analysis/specs/frame-order.md` and must be resolved there before these three actions
can be ordered correctly against `SetVelocity2d` (FixedUpdate) writes in the same state.

### Q-fsmact-13 — When `Object.Destroy` takes effect
`DestroyObject` (`A:DestroyObject.cs:33/37`) calls `Object.Destroy(go)` and then, on the next line
(`:41`), `go.transform.DetachChildren()`. Whether that second call still sees a live object — i.e.
whether `Destroy` is immediate or deferred to end-of-frame — is a Unity semantic no decompiled
source states. 13 boss-scene instances, 1 on Hornet. The observable difference is whether the
children survive the destroy.

---

## Review fixes

Applied 2026-08-31 in response to `analysis/specs/REVIEW-p1.md` (verdict for this file:
PASS-WITH-FIXES, 54/56 cites confirmed). Every row below was re-verified against the decomp or the
dump before editing; nothing was taken on the review's word alone.

| defect | what was wrong | change |
|---|---|---|
| **D04** | §4.2 `SetParent` claimed "reparenting changes world transform unless the offsets are reset" — uncited, and contrary to the engine default. | Claim withdrawn. The entry now states only what `A:SetParent.cs:30-46` writes (`parent`, and `localPosition`/`localRotation` when the flags are set) and marks the world-pose question **UNKNOWN** → new **Q-fsmact-11**. |
| **D05** | §8 claimed to be "every draw, in order" but omitted `ShakePositionV2` / the `CameraShake` FSM, because the boss-scene-only census table excludes `DontDestroyOnLoad` FSMs. | New **§8.3** (30+ lines): the action's full semantics (`A:ShakePositionV2.cs:48-120`), the 3 draws per `UpdateShaking` at `:96`, the `FpsLimit` gate at `:82-89` (all 15 dumped instances have `FpsLimit=0`), a per-state draw table, the 68 shake-send sites in `GG_Hornet_1` (4 enabled `EnemyKillShake` sends from Hornet's `G Dash`/`A Dash`/`Sphere`/`Sphere A`, `HealthManager.cs:372` per landed hit, `Knight Damage/Gen` per knight hit), and the arithmetic (≈78 / ≈78 / ≈153 draws). Row added to the §8.2 table with a `†` scope footnote. Census gap escalated as **Q-fsmact-10** ("the P4 seeded-RNG gate cannot be evaluated until this exists"). |
| **D50** | §0.4 gave `DelayedEvent.WasSent(null) == true` as the reason the delay-0 branch of `SendEvent`/`SendEventByName` finishes. | Corrected: `Finish()` runs inside `OnEnter` (`A:SendEvent.cs:40-43`, `A:SendEventByName.cs:36-39`), so `OnUpdate` never runs there; `WasSent(null)` matters only on the `everyFrame` path. |
| **D51** | §3 and §10 said `SendRandomEventV3`'s missed-max scan forces "that index". | The scan (`A:SendRandomEventV3.cs:43-50`) has no `break`, so it forces the **highest** over-limit index. Fixed in both places. |
| **D52** | The four cross-FSM rows omitted `GetGameObjectFsm`'s fallback. | §0.6 now documents `HK:…/ActionHelpers.cs:56-71`: a name miss logs a warning and falls through to `go.GetComponent<PlayMakerFSM>()` (`:70`) — the **first** FSM — so `SetFsm*`'s "Could not find FSM" branch fires only when the object has no `PlayMakerFSM` at all, and a wrong `fsmName` silently retargets. |
| **D53** | Q1–Q3 were superseded by `analysis/open-questions.md` Q15. | Q1–Q3 deleted; §11 opens with the Q15 closure and its consequences for this file. "Subject to Q1" removed from §7 and §8.2; the audio-pitch note now cites Q15 affirmatively. |
| **D54** | Instance counts were presented without noting disabled instances. | New paragraph at the top of the document: counts are **instance** counts, disabled actions never execute (`PM:FsmState.cs:292-296`), and the full 26/558 disabled breakdown for Hornet. Call-outs added inline for `SendEvent` (2/2 disabled), `SendEventByName` (1/14), `SetScale` (8/27), `GetScale`, `SetBoxCollider2DSizeVector`, `ActivateGameObject`. |
| **D55** | Five uncited engine claims. | `FreezeMoment` now cites `HK:GameManager.cs:2860-2885` **and** records that the mod IL-hooks it to a no-op (`analysis/open-questions.md` Q21). Pool "residual state" replaced by the cited mechanism (`HK:ObjectPool.cs:472-526`: only `parent`/`localPosition`/`localRotation` assigned, `"A SPAWN"` vs `SetActive(true)`, exhaustion → `Instantiate`). Trigger dispatch now cites `HK:PlayMakerUnity2DProxy.cs:143,190` with frame position → **Q-fsmact-12**. `Object.Destroy` deferral → **Q-fsmact-13**. Particle seed remains **Q-fsmact-2**. |
| **D56** | §0.6 said `GetRandomWeightedIndex` returns −1 only when `sum == 0`. | Also returns −1 when the draw lands exactly on `sum`: float `Range` is max-inclusive (Q15) and the loop test is the strict `num2 < weights[j]` (`ActionHelpers.cs:83`). Both paths noted as live. |
| — | Open questions were numbered `Q1–Q12`. | Renumbered to `Q-fsmact-1 … Q-fsmact-13`, one paragraph each, per the coordinator's consolidation format. `open-questions.md` was **not** edited. |

Not changed: the 105-type scope, the §9 master table (105 rows), and the per-action semantics, which
the review confirmed at 54/56 with 105/105 dump-value checks matching.
