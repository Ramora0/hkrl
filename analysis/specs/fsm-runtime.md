# PlayMaker FSM runtime — the contract the native interpreter implements

P1 discovery, 2026-08-30. Owner constraint: **the interpreter is an exact port of the PlayMaker
decomp**, never of behaviour inferred from traces. Every rule below is a decomp line. Traces
(§6) only *verify* the port; nothing in §1–§4 is derived from them. Anything the decomp does not
state is UNKNOWN and appears in §8, not as a guess.

Decompiled target: `hollow_knight.exe` FileVersion 2020.2.2.426360, ilspycmd 10.0.1 (STATE.md).
PlayMaker core lives in `analysis/decomp/PlayMaker/`; the ~2500 concrete `FsmStateAction`
subclasses were merged into Assembly-CSharp by HK's build and live in
`analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker.Actions/` contains only `MissingAction.cs`).

---

## 1. Lifecycle

### 1.1 Component boot

`PlayMakerFSM` is a `MonoBehaviour`
(`analysis/decomp/PlayMaker/PlayMakerFSM.cs:9`). Unity drives it:

| Unity callback | body |
|---|---|
| `Awake` | `PlayMakerGlobals.Initialize(); if (!IsEditor) FsmLog.LoggingEnabled = false; Init();` — `analysis/decomp/PlayMaker/PlayMakerFSM.cs:134-142` |
| `OnEnable` | `fsmList.Add(this); fsm.OnEnable();` — `analysis/decomp/PlayMaker/PlayMakerFSM.cs:363-367` |
| `Start` | `if (!fsm.Started) fsm.Start();` — `analysis/decomp/PlayMaker/PlayMakerFSM.cs:355-361` |
| `Update` | `if (!fsm.Finished && !fsm.ManualUpdate) fsm.Update();` — `analysis/decomp/PlayMaker/PlayMakerFSM.cs:369-375` |
| `OnDisable` | `if (fsm.Started) fsm.Event(FsmEvent.Disable); fsmList.Remove(this); if (fsm != null && !fsm.Finished) fsm.OnDisable();` — `analysis/decomp/PlayMaker/PlayMakerFSM.cs:392-403` |
| `OnDestroy` | `fsmList.Remove(this); fsm.OnDestroy();` — `analysis/decomp/PlayMaker/PlayMakerFSM.cs:405-412` |
| `OnBecameVisible/Invisible` | `fsm.Event(FsmEvent.BecameVisible/BecameInvisible)` — `analysis/decomp/PlayMaker/PlayMakerFSM.cs:481-489` |

`PlayMakerFSM.FsmList` is a static `List<PlayMakerFSM>` maintained by OnEnable/OnDisable/OnDestroy
(`analysis/decomp/PlayMaker/PlayMakerFSM.cs:13,365,398,407`). It is the enumeration domain for
`BroadcastEvent` (§3.6), so **enable order is broadcast order**. The `Fsm` property getter
re-assigns `fsm.Owner = this` on every access (`analysis/decomp/PlayMaker/PlayMakerFSM.cs:55-62`).

### 1.2 Init / Preprocess / Awake (data load)

`PlayMakerFSM.Init` (`analysis/decomp/PlayMaker/PlayMakerFSM.cs:158-181`):
1. `if (fsmTemplate != null) { if (Application.isPlaying) InitTemplate(); } else InitFsm();`
2. `fsm.Init(this)` (`analysis/decomp/PlayMaker/PlayMakerFSM.cs:176`).
3. `if (!eventHandlerComponentsAdded || !fsm.Preprocessed) AddEventHandlerComponents();`
   (`analysis/decomp/PlayMaker/PlayMakerFSM.cs:177-180`).

`Fsm.Init` (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:1604-1613`) =
`owner = component; InitData(); if (!preprocessed) Preprocess(); Awake();`.

`Fsm.InitData` (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:1621-1682`), idempotent
behind `initialized` (`:1627-1631`):
- canonicalises every entry of `events[]` through `FsmEvent.GetFsmEvent` (`:1632-1635`) and
  `ExposedEvents` (`:1636-1642`);
- per state: `state.Fsm = this` (`:1648`), `state.LoadActions()` (`:1649`), and for each
  transition resolves `ToFsmState = GetState(ToState)` (`:1653-1656`) and
  `FsmEvent = FsmEvent.GetFsmEvent(EventName)` (`:1657-1661`);
- same for `globalTransitions`, additionally appending each global-transition event to `events`
  if absent (`:1664-1680`).

`Fsm.Preprocess` (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:1570-1584`): for every
state, every action → `Init(state)`, `OnPreprocess()`; then `CheckFsmEventsForEventHandlers()`;
`preprocessed = true`.
`Fsm.Awake` (`:1586-1602`): for every state, every action → `Init(state)`, `Awake()`, and then
`if (!preprocessed) CheckFsmEventsForEventHandlers();` (`:1598-1601`). That second scan is not
redundant: the `HandleFixedUpdate`/`HandleLateUpdate` setters clear `preprocessed` (hazard below),
so an action that opts in from `Awake()` re-arms the handler scan for an FSM whose `Preprocess`
already ran.

`CheckFsmEventsForEventHandlers` (`:1684-1837`) walks `Events` and, for each *system* event
present, sets the matching `RootFsm.Handle*` flag (e.g. `FsmEvent.TriggerEnter2D` →
`RootFsm.HandleTriggerEnter2D = true`, `:1715-1718`), recursing into `SubFsmList` (`:1833-1836`).

`PlayMakerFSM.AddEventHandlerComponents` (`analysis/decomp/PlayMaker/PlayMakerFSM.cs:212-315`)
then adds one proxy `MonoBehaviour` per set flag and registers `this` as a target
(`:317-324`, `:326-342`); the two that matter for simulation are
`HandleFixedUpdate → PlayMakerFixedUpdate` (`:286-289`) and
`HandleLateUpdate → PlayMakerLateUpdate` (`:290-293`). See §2.5.

Ordering hazard to port faithfully: `Fsm.HandleFixedUpdate`'s **setter** sets
`preprocessed = false` and propagates to `host` (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:1119-1134`;
`HandleLateUpdate` identically at `:1136-1151`). An action that opts in during `Awake()` therefore
leaves `preprocessed == false` after `Fsm.Init` — which is exactly why
`analysis/fsm/GG_Hornet_1.json` reports `preprocessed: false` for `Hornet Boss 1 / Control` while
also reporting `handleFixedUpdate: true`. `preprocessed` in the dumps is **not** a reliable
"did Preprocess run" flag; do not use it as one.

### 1.3 Start and initial state entry

`Fsm.Start` (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:1863-1893`):
```
Started = true; Finished = false;
FsmExecutionStack.PushFsm(this);
if (ActiveState == null) { ActiveState = GetState(startState); activeStateEntered = false; }
if (breakpoint) DoBreakpoint(); else { switchToState = ActiveState; UpdateStateChanges(); }
FsmExecutionStack.PopFsm();
```
So the start state is entered through the *normal* `SwitchState → EnterState` path (§2.2), and
because `activeStateEntered` is false, `SwitchState`'s exit branch
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:2351-2354`) is skipped: no `ExitState`
on first entry. `GetState` is a linear name scan returning `null` when absent
(`:2471-2481`); `ActiveState`'s setter mirrors the name into `activeStateName` (`:562-577`).

### 1.4 Disabled / inactive objects

**ENGINE ASSUMPTION (Q-fsmrt-2), not a decomp fact:** that Unity never calls
`Awake/OnEnable/Start/Update` on a component of an inactive GameObject. No file in
`analysis/decomp/` states it; it is asserted here because the dump is consistent with it — 116
FSMs per scene had `Fsm.Initialized == false` at SceneReady
(`analysis/fsm/GG_Hornet_1.json` `counts.initDataForced`, key `initializedBeforeDump`). Granting it,
`Fsm.Initialized` stays false, `FsmState.fsm` stays null, and `FsmState.Actions` cannot
deserialize — `ActionData.CreateAction` requires `state.Fsm`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/ActionData.cs:661-668`). The oracle's dumper had
to force `Fsm.InitData()` for 116 FSMs per scene to read their actions at all
(`oracle/Oracle/FsmDumper.cs:158-163`; `analysis/fsm/GG_Hornet_1.json` `counts.initDataForced` = 116).

Disable path (`analysis/decomp/PlayMaker/PlayMakerFSM.cs:392-403` →
`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:1970-1990`):
`Event(FsmEvent.Disable)` → `Fsm.OnDisable()` → `Stop()`:
`if (RestartOnEnable) StopAndReset(); Finished = true;`.
`StopAndReset` (`:1992-2004`) runs `ExitState(ActiveState)` when
`ActiveState != null && activeStateEntered`, then nulls `ActiveState`, `LastTransition`,
`SwitchedState`, `HitBreakpoint`.
Note the ordering: the DISABLE event is dispatched **while the FSM is still live**, so a
`DISABLE` state/global transition fires normally before the FSM stops.

### 1.5 Re-enable (pooled objects)

`Fsm.OnEnable` (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:1839-1856`):
```
Finished = false;
if (HandleLevelLoaded) resubscribe SceneManager.sceneLoaded;
if (ActiveState == null || RestartOnEnable) {
    ActiveState = GetState(startState); activeStateEntered = false;
    if (Started) Start();
}
```
`RestartOnEnable` is a public field defaulting to `true`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:113`). With it true the pooled object
restarts in `startState` and `Start()` re-enters it. With it false the FSM keeps the state it was
disabled in and only clears `Finished`; `activeStateEntered` was never cleared, so **no `OnEnter`
re-runs** — actions resume mid-state with their `Finished`/`ActiveActions` bookkeeping from before
the disable.

**The per-FSM value is dumped and must be read, not assumed.** `FsmDumper.One` emits it as
`restartOnEnable` (`oracle/Oracle/FsmDumper.cs:178`), present on 962/962 FSMs of
`analysis/fsm/GG_Hornet_1.json`. Census: **true on 954, false on 8** (15 FK / 8 Gruz / 8 MMC —
`analysis/open-questions.md` Q17). The eight in the Hornet scene, with their dumped `startState`
and `activeState`:

| path | fsmName | startState | activeState at dump |
|---|---|---|---|
| `Knight` | `Map Control` | `Pause` | `Inactive` |
| `Knight` | `Spell Control` | `Pause` | `Inactive` |
| `Knight` | `Nail Arts` | `Init` | `Inactive` |
| `Knight` | `Superdash` | `Init` | `Inactive` |
| `Knight` | `Surface Water` | `Init` | `Inactive` |
| `_GameCameras/HudCamera/Inventory/Charms` | `Initial Activation` | `State 1` | `State 2` |
| `_Managers/PlayMaker Unity 2D` | `PlayMaker Unity 2D` | (empty) | (empty) |
| `GG_Arena_Prefab/Darkness Region` | `Darkness Region` | `Pause` | `Enter` |

Five of the eight are the Knight's own FSMs (§5 *Port scope*), so the resume-mid-state branch sits
on the hero path, not in an exotic corner. Everything else — every pooled boss add included, e.g.
`Needle / Control` — is `restartOnEnable: true` and restarts in `startState` on every enable.

### 1.6 Templates

`PlayMakerFSM.InitTemplate` (`analysis/decomp/PlayMaker/PlayMakerFSM.cs:183-197`) replaces the
component's `fsm` with `new Fsm(fsmTemplate.fsm, fsm.Variables)`, carrying over `Name`,
`EnableDebugFlow`, `EnableBreakpoints`, `ShowStateLabel` and setting `UsedInTemplate = null`. It
runs only when `Application.isPlaying` (`analysis/decomp/PlayMaker/PlayMakerFSM.cs:162`).
`FsmTemplate` is a `ScriptableObject` holding one `Fsm` (`analysis/decomp/PlayMaker/FsmTemplate.cs:6-32`).

**Interpreter consequence:** the dumper reads `comp.Fsm` (`oracle/Oracle/FsmDumper.cs:148`), i.e.
the *post-`InitTemplate`* instance, so `analysis/fsm/*.json` already contains the instantiated
states/actions and the interpreter never resolves templates. 511/962 FSMs in `GG_Hornet_1` were
templated **and** initialized before the dump; 68 were templated and *not* initialized (their
`comp.Fsm` is the serialized component `fsm`, not the template instance). Those 68 still carry
non-empty state lists (min 1, median 7 states, 1390 actions total), and for the 4 templates that
appear in both groups the state-name lists are identical — evidence, not proof, that the
serialized component copy matches the template. Q-fsmrt-1.

---

## 2. The update algorithm

### 2.1 `Fsm.Update` — the per-frame driver

`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:1895-1922`:
```
FsmTime.RealtimeBugFix();                       // :1897   sets a static bool, no state
if (owner == null) return;                      // :1898
if (HitBreakpoint) return;                      // :1903   (editor-only, see 2.7)
PushFsm(this);                                  // :1905
if (!activeStateEntered) Continue();            // :1906-1909  → EnterState(ActiveState)
UpdateDelayedEvents();                          // :1910
if (ActiveState != null) UpdateState(ActiveState);  // :1911-1914
PopFsm();                                       // :1915
```
`UpdateState` (`:2406-2411`) = `state.Fsm = this; state.OnUpdate(); UpdateStateChanges();`.

`FsmState.OnUpdate` (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmState.cs:342-355`):
```
if (finished) return;                                  // :344  state-level latch
StateTime += Time.deltaTime;                           // :346
for (i = 0; i < ActiveActions.Count; i++) {            // :347  live count, forward index
    ActiveActions[i].Init(this); ActiveActions[i].OnUpdate();
}
CheckAllActionsFinished();                             // :353
```

**Which actions are skipped.** `ActiveActions` is the per-entry working list, not `Actions`.
`FsmState.OnEnter` clears it (`:279`) and `ActivateActions` appends an action only if it is still
un-`Finished` after its `OnEnter` (`:303-306`). An action that calls `Finish()`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmStateAction.cs:181-189`) is appended to the
state's `finishedActions` list and removed from `ActiveActions` by `RemoveFinishedActions`
(`FsmState.cs:600-607`), which runs from `CheckAllActionsFinished` (`:613`) — i.e. **after** the
whole `OnUpdate` sweep, never during it. So an action that finishes at index *i* is still iterated
this frame and skipped from the next frame on. Disabled actions never enter the list at all:
`ActivateActions` sets `Finished = true` and `continue`s (`FsmState.cs:292-296`).

### 2.2 When a mid-action-list event is committed

`FsmState.CheckAllActionsFinished` (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmState.cs:609-620`):
```
if (finished || !active || fsm.IsSwitchingState) return;
RemoveFinishedActions();
if (ActiveActions.Count == 0 &&
    (!isSequence || ++activeActionIndex >= actions.Length || ActivateActions(activeActionIndex))) {
    finished = true;
    fsm.Event(FsmEvent.Finished);
}
```
`Fsm.Event(FsmEvent)` → `Event(EventTarget, fsmEvent)`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:2192-2198`) → `ProcessEvent` for a self
target (`:2139-2141`) → `ActiveState.OnEvent` then global transitions then state transitions
(`:2042-2078`) → `DoTransition` (`:2327-2345`):
```
if (transition.ToFsmState == null) return false;   // :2329-2333
LastTransition = transition;                       // :2334
switchToState = toFsmState;                        // :2339
if (EventData.SentByFsm != this) UpdateStateChanges();   // :2340-2343  ← THE HINGE
return true;
```
`EventData.SentByFsm` is stamped at the top of `Event(FsmEventTarget, FsmEvent)` from
`FsmExecutionStack.ExecutingFsm` (`:2128` → `:2082-2087` →
`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmExecutionStack.cs:9-19`). Hence:

- **Self-raised while this FSM is executing** (`ExecutingFsm == this`, the ordinary
  `Update`/`FixedUpdate`/`LateUpdate` path, which pushed `this` at
  `Fsm.cs:1905`/`:1952`/`:1962`): `SentByFsm == this` → the transition is **queued** in
  `switchToState` and committed by the *enclosing* `UpdateStateChanges()` after the action loop
  returns. The `if (FsmExecutionStack.ExecutingFsm != this)` tail of `Event` (`:2176-2181`) is
  also skipped.
- **Raised from outside this FSM's execution** (another FSM's action, a Unity physics callback, HK
  C# such as `PlayMakerFSM.SendEvent`, mod code): `SentByFsm != this` → `DoTransition` calls
  `UpdateStateChanges()` **immediately**, so `ExitState` + `EnterState` (and everything they
  cascade) run synchronously inside the caller's stack frame, before the caller's next statement.

`FsmState.OnEvent` (`FsmState.cs:319-329`) gives every entry of `ActiveActions` a chance to consume
the event via `FsmStateAction.Event` (base returns `false`,
`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmStateAction.cs:176-179`); a true return makes
`ProcessEvent` skip transition matching entirely (`Fsm.cs:2042-2046`).

**Port hazard — the consumed-flag is NOT an OR.** The loop body is
`flag = fsmStateAction.Event(fsmEvent);` (`FsmState.cs:326`) — a plain *assignment*, so every
iteration overwrites the previous result. `flag` is initialised `false` at `:321` and the return is
`fsm.IsSwitchingState || flag` at `:328`. The flag therefore reflects **only the last entry of
`ActiveActions`**; an earlier action that returned true is silently discarded unless it also set
`switchToState`. Port the assignment verbatim — writing `flag |= …` would suppress transitions the
real runtime still takes, changing which events reach the transition tables. With `ActiveActions`
empty the result is just `IsSwitchingState`.

### 2.3 Transition chaining within one Update — bounded by `MaxLoopCount`

`Fsm.UpdateStateChanges` (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:2314-2325`):
```
while (IsSwitchingState && !HitBreakpoint) SwitchState(switchToState);
for (i = 0; i < States.Length; i++) States[i].ResetLoopCount();
EventTarget = null;
```
`IsSwitchingState` is `switchToState != null` (`:560`). `SwitchState` (`:2347-2365`) exits the old
state, assigns `ActiveState`, and `EnterState` (`:2375-2397`) clears `switchToState = null`
(`:2380`) *before* `state.OnEnter()` (`:2396`). So a state whose `OnEnter` raises an event that
matches a transition sets `switchToState` again and the `while` loop takes another lap.
**Chaining is unbounded per frame except by the loop guard**:
```
if (state.loopCount >= MaxLoopCount) { Owner.enabled = false; MyLog.LogError(...); return; }
```
(`Fsm.cs:2385-2390`). `FsmState.OnEnter` increments `loopCount` (`FsmState.cs:269-273`);
`UpdateStateChanges` resets **every** state's `loopCount` after the drain (`Fsm.cs:2320-2323`,
`FsmState.cs:638-641`). So the bound is *per drain*: any single state may be entered at most
`MaxLoopCount` times inside one `UpdateStateChanges` call, and exceeding it **disables the
PlayMakerFSM component** rather than throwing. `MaxLoopCount` = `maxLoopCount` if > 0 else `1000`
(`Fsm.cs:595-605`; `const int DefaultMaxLoops = 1000` at `:25`). Both halves are dumped
(`oracle/Oracle/FsmDumper.cs:181-182`) and measured: `maxLoopCountOverride` = 0 and `maxLoopCount`
= 1000 on **962/962** FSMs of `analysis/fsm/GG_Hornet_1.json` — 1000 is the value, not an
assumption. `FsmState.IsBreakpoint` is dumped too (`:236`) and false on 0/7539 states, so the
breakpoint branches of §2.7 are provably dead in these scenes.

Second, independent chaining mechanism: `ActivateActions`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmState.cs:286-317`) aborts the moment a
transition is pending —
```
if (Fsm.IsSwitchingState) return false;      // :307-310
if (!action.Finished && isSequence) return false;   // :311-314
```
— so actions after the one that fired the event **never run** for that entry, and returning
`false` makes `OnEnter` skip `CheckAllActionsFinished` (`FsmState.cs:280-283`), i.e. no spurious
FINISHED.

### 2.4 `NextFrameEvent`-style deferral

There is no engine-level deferral queue. `NextFrameEvent`
(`analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/NextFrameEvent.cs:15-23`) has an
empty `OnEnter` and, in `OnUpdate`, `Finish(); Fsm.Event(sendEvent);`. The one-frame delay is
purely "OnEnter this frame, OnUpdate next frame" — the state is entered inside
`UpdateStateChanges` (after the action loop) and its first `OnUpdate` is the *next*
`PlayMakerFSM.Update`. 1408 instances across the four boss scenes
(`analysis/specs/fsm-census.md`), so this is a first-class part of the timing model.

The genuine timed deferral is `DelayedEvent` (`Fsm.cs:2200-2212`), ticked only from
`Fsm.UpdateDelayedEvents` (`:1924-1943`) at the top of `Fsm.Update` (`:1910`), over a snapshot copy
of `delayedEvents` (`:1927-1928`). `DelayedEvent.Update`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/DelayedEvent.cs:68-88`) decrements by
`Time.deltaTime`, and on expiry swaps in the captured `FsmEventData`, calls `fsm.Event(...)`, then
`fsm.UpdateStateChanges()` explicitly, then restores `Fsm.EventData`. Delayed events are killed on
every state exit unless `keepDelayedEventsOnStateExit` (`Fsm.cs:2430-2433`, `:1329-1332`); that
flag is dumped (`oracle/Oracle/FsmDumper.cs:180`) and **false on 962/962** FSMs of
`analysis/fsm/GG_Hornet_1.json`, so the port unconditionally kills delayed events on state exit.

### 2.5 FixedUpdate / LateUpdate proxies

`Fsm.FixedUpdate` / `Fsm.LateUpdate`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:1950-1968`) are **never called by
`PlayMakerFSM`** — it has no `FixedUpdate`/`LateUpdate` method
(`analysis/decomp/PlayMaker/PlayMakerFSM.cs` has none). They are called by separate proxy
`MonoBehaviour`s:
```
PlayMakerFixedUpdate.FixedUpdate:  for each TargetFSMs[i]
    if (fsm != null && fsm.Fsm != null && fsm.Active && fsm.Fsm.HandleFixedUpdate) fsm.Fsm.FixedUpdate();
```
(`analysis/decomp/PlayMaker/PlayMakerFixedUpdate.cs:6-16`; `PlayMakerLateUpdate.cs:6-16` is
identical with `HandleLateUpdate`/`LateUpdate`). Targets are registered by
`PlayMakerProxyBase.AddTarget` (`analysis/decomp/PlayMaker/PlayMakerProxyBase.cs:34-40`) from
`AddEventHandlerComponents` (§1.2), one proxy component per GameObject shared by all its FSMs.

```
Fsm.FixedUpdate: PushFsm; if (ActiveState != null && activeStateEntered) FixedUpdateState(ActiveState); PopFsm;
FixedUpdateState:  state.Fsm = this; state.OnFixedUpdate(); UpdateStateChanges();
```
(`Fsm.cs:1950-1958`, `:2399-2404`; LateUpdate `:1960-1968`, `:2413-2418`.) Note the extra
`activeStateEntered` guard, absent from `Update`, and note that these paths **do** commit
transitions — `UpdateStateChanges()` is called, so an FSM can change state in FixedUpdate or
LateUpdate exactly as in Update.

`FsmState.OnFixedUpdate` / `OnLateUpdate`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmState.cs:331-340`, `:357-366`) iterate
`ActiveActions` and call `CheckAllActionsFinished`, but — unlike `OnUpdate` — **do not check the
`finished` latch and do not advance `StateTime`**.

**How an action opts in.** By assigning `Fsm.HandleFixedUpdate`/`HandleLateUpdate` from
`OnPreprocess` or `Awake`. **51** action classes in Assembly-CSharp set `HandleFixedUpdate` — 50 in
`analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/` plus
`analysis/decomp/Assembly-CSharp/CheckTrackTriggerCount.cs:42`, which sits outside that directory —
and 13 set `HandleLateUpdate`. Canonical shapes:
- unconditional, in `Awake`: `SetVelocity2d.Awake { Fsm.HandleFixedUpdate = true; }`
  (`analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/SetVelocity2d.cs:41-44`); its
  `OnEnter` and `OnFixedUpdate` both call `DoSetVelocity()` and `Finish()` unless `everyFrame`
  (`:46-62`). So a non-`everyFrame` `SetVelocity2d` writes the velocity **twice**: once at
  `OnEnter` and once in the following `FixedUpdate` (`Finish()` only removes it from
  `ActiveActions` at the next `CheckAllActionsFinished`).
- field-conditional, in `OnPreprocess`: `Translate.OnPreprocess { if (fixedUpdate) …HandleFixedUpdate = true; if (lateUpdate) …HandleLateUpdate = true; }`
  (`analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/Translate.cs:64-74`), with
  `OnUpdate`/`OnLateUpdate`/`OnFixedUpdate` each gated on the same fields (`:85-115`).
- the generic base `FsmStateActionAdvanced` sets `HandleFixedUpdate = true` in `OnPreprocess`
  unconditionally and dispatches `OnActionUpdate` by an `updateType` enum
  (`analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/FsmStateActionAdvanced.cs:26-66`).

The flag is per-FSM, not per-action: **one** opting-in action makes the whole FSM tick in
FixedUpdate every frame. Measured incidence (`analysis/fsm/*.json`, field `handleFixedUpdate` /
`handleLateUpdate`): GG_Hornet_1 79/962 and 1/962; GG_Gruz_Mother 77/941 and 1/941;
GG_False_Knight 88/985 and 1/985; GG_Mega_Moss_Charger 80/947 and 1/947.
`Hornet Boss 1 / Control` has `handleFixedUpdate: true`.

### 2.6 The full per-frame algorithm (port target)

```
UPDATE(f):                    # PlayMakerFSM.Update; "once per rendered frame" = Q-fsmrt-2
  if f.Finished or f.ManualUpdate: return                      # PlayMakerFSM.cs:371
  push(f)                                                      # Fsm.cs:1905
  if not f.activeStateEntered: f.activeStateEntered=True; ENTER(f, f.ActiveState)   # :1906-1909, :2980-2986
  TICK_DELAYED(f)                                              # :1910, :1924-1943
  s = f.ActiveState
  if s is not None:
     if not s.finished:                                        # FsmState.cs:344
        s.StateTime += dt                                      # :346
        for a in s.ActiveActions (live count, ascending): a.OnUpdate()   # :347-352
        CHECK_FINISHED(s)   # → f.Event(FINISHED) if ActiveActions empty # :353, :609-620
     DRAIN(f)                                                  # :2410 → 2314-2325
  pop(f)

DRAIN(f):  while f.switchToState: SWITCH(f, f.switchToState)   # Fsm.cs:2316-2319
           for st in f.States: st.loopCount = 0 ; f.EventTarget = None      # :2320-2324
SWITCH(f,to): if f.ActiveState and f.activeStateEntered: EXIT(f, f.ActiveState)  # :2351-2354
              f.ActiveState = to ; ENTER(f, to)                                  # :2355-2363
ENTER(f,s):   f.EventTarget=None; f.SwitchedState=True; f.activeStateEntered=True; f.switchToState=None   # :2377-2380
              if s.loopCount >= f.MaxLoopCount: f.Owner.enabled=False; return    # :2385-2390
              s.loopCount++; s.active=True; s.finished=False; s.finishedActions.clear()                  # FsmState.cs:269-276
              s.StateTime=0; s.ActiveActions.clear()                                                     # :277-279
              if ACTIVATE(s, 0): CHECK_FINISHED(s)                                                       # :280-283
EXIT(f,s):    f.PreviousActiveState=s; f.ActiveState=None; s.active=False; s.finished=False              # :2422-2428, FsmState.cs:624-625
              for a in s.Actions where a.Entered: a.OnExit()                                             # :626-635
              if not f.keepDelayedEventsOnStateExit: f.delayedEvents.clear()                             # Fsm.cs:2430-2433
ACTIVATE(s,i0): for i in i0..len(s.Actions)-1:
                  a=s.Actions[i]; if not a.Enabled: a.Finished=True; continue                            # :292-296
                  a.Active=True; a.Finished=False; a.Init(s); a.Entered=True; a.OnEnter()                # :297-302
                  if not a.Finished: s.ActiveActions.append(a)                                           # :303-306
                  if f.switchToState: return False                                                       # :307-310
                  if not a.Finished and s.isSequence: return False                                       # :311-314
                return True
FIXED(f)/LATE(f): if f.Active and f.HandleFixedUpdate/LateUpdate and f.ActiveState and f.activeStateEntered:
                     push(f); for a in ActiveActions: a.OnFixedUpdate()/OnLateUpdate(); CHECK_FINISHED; DRAIN(f); pop(f)
                                                    # PlayMakerFixedUpdate.cs:6-16, Fsm.cs:1950-1968, :2399-2418
```

`EXIT` detail worth flagging: it iterates **`s.Actions`**, not `ActiveActions`, filtered by
`a.Entered` (`FsmState.cs:626-635`). `FsmStateAction.Entered` is set true at `FsmState.cs:301` and
is **never reset anywhere in the PlayMaker assembly** (only two references exist: `:301` write,
`:629` read). Consequence: once an action has been entered even once, its `OnExit` runs on *every*
subsequent exit of that state — including exits where `ActivateActions` aborted before reaching
it (§2.3). Actions that register/unregister delegates (`Trigger2dEvent`,
`CheckCollisionSideEnter`) rely on this; a port that only exits `ActiveActions` will leak
subscriptions.

### 2.7 Editor-only paths that the port must hard-code as "off"

`HitBreakpoint`, `BreakpointsEnabled`, `StepToStateChange`, `StepFsm` are static
(`Fsm.cs:777,779,791,793`) and gate `Fsm.Update` (`:1903`), `UpdateStateChanges` (`:2316`),
`Fsm.Start` (`:1878`) and `SwitchState` (`:2356`). `PlayMakerGlobals.IsEditor` is
`Application.isEditor` (`analysis/decomp/PlayMaker/PlayMakerGlobals.cs:58-63`), false in the
shipped player, so `DoTransition`'s logging branch (`Fsm.cs:2335-2338`) and the
`Init`-time editor resets (`PlayMakerFSM.cs:171-175`) are dead. `FsmLog.LoggingEnabled` is forced
false in `PlayMakerFSM.Awake` (`:137-140`), killing every `if (FsmLog.LoggingEnabled)` branch.
`ManualUpdate` is dumped (`oracle/Oracle/FsmDumper.cs:179`) and **false on 962/962** FSMs of
`analysis/fsm/GG_Hornet_1.json`; so are `hasHost` (`:184`, false on 962/962), `subFsmCount`
(`:185`, 0 on 962/962) and `usedInTemplate` (`:186`, false on 962/962). The sub-FSM/host machinery
(`Fsm.Host` `:320`, `SubFsmList` `:352`) and the `HostFSM`/`SubFSMs` event targets (§3.6) are
therefore **unreachable in these scenes** and need no port. `exposedEvents` (`:183`) is non-zero on
154/962 (2 on 144 FSMs, 1 on 6, 3 on 2, 6 on 1, 10 on 1); it feeds only `InitData`'s canonicalisation
loop (`Fsm.cs:1636-1642`) and no dispatch decision.

---

## 3. Events and transitions

### 3.1 `FsmEvent` identity

`FsmEvent` is interned in a process-global `Dictionary<string, FsmEvent>`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmEvent.cs:10,27-38,311-321`).
`GetFsmEvent(string)` returns the interned instance when `PlayMakerGlobals.IsPlaying`, else a copy
(`:402-413`). **Matching in `ProcessEvent` and `DoTransition` is by reference**
(`Fsm.cs:2050`, `:2066`) — but because `InitData` canonicalises every transition's `FsmEvent`
through `GetFsmEvent` (`Fsm.cs:1659-1660`, `:1673`), reference equality is equivalent to
**exact, case-sensitive string equality** at runtime. The interpreter may key events by interned
string id.

55 system events are pre-registered — 55 `AddSystemEvent("…")` calls in `FsmEvent.cs:440-497`, notably `FINISHED` (`:442`),
`DISABLE` (`:443`), `LEVEL LOADED` (`:446`), and the 2D physics set `COLLISION ENTER 2D`,
`COLLISION EXIT 2D`, `COLLISION STAY 2D`, `TRIGGER ENTER 2D`, `TRIGGER EXIT 2D`,
`TRIGGER STAY 2D` (`:461-466`). Names are the literal upper-case strings, spaces included.
`FsmEvent.IsNullOrEmpty` treats a null event or an empty name as nothing (`:302-309`).

### 3.2 Dispatch order inside one FSM

`Fsm.ProcessEvent` (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:2023-2080`):
```
if (!Active || FsmEvent.IsNullOrEmpty(e)) return;      # :2025-2028
if (!Started) Start();                                 # :2029-2032   ← an event STARTS a stopped FSM
if (!Active) return;                                   # :2033-2036
if (eventData != null) copy SentBy* into Fsm.EventData # :2037-2040
PushFsm(this)                                          # :2041
if (ActiveState.OnEvent(e)) { pop; return; }           # :2042-2046   actions may consume
foreach t in globalTransitions:  if (t.FsmEvent == e) and DoTransition(t, isGlobal:true)  { pop; return; }   # :2047-2062
foreach t in ActiveState.Transitions: if (t.FsmEvent == e) and DoTransition(t, isGlobal:false) { pop; return; }  # :2063-2078
pop
```
So the precedence is **actions → global transitions → state transitions**, first match wins, and
`DoTransition` returning `false` (null `ToFsmState`, `:2329-2333`) lets the scan continue to the
next candidate. `Active` (`:546-556`) = `owner != null && owner.gameObject != null && !Finished &&
ActiveState != null`; it does **not** test `Component.enabled` or `activeInHierarchy` — a disabled
component is excluded only because `OnDisable` set `Finished = true` (§1.4).

Global transitions are a flat `FsmTransition[]` field on the FSM (`Fsm.cs:58-59`; `:518` is the
public accessor), dumped as `globalTransitions` (`oracle/Oracle/FsmDumper.cs:199`). Incidence: ~1133–1141 across all FSMs in
each boss scene (`analysis/fsm/*.json`); `Hornet Boss 1 / Control` has exactly one,
`STUN → Stun Start`.

### 3.3 `FINISHED` conventions

`FINISHED` is not special-cased anywhere in dispatch; it is an ordinary interned event
(`FsmEvent.cs:442`) that the runtime happens to raise from `CheckAllActionsFinished`
(`FsmState.cs:617`) when a state's `ActiveActions` empties. Corollaries the port must preserve:
- A state with **zero enabled actions** finishes during its own `OnEnter`: `ActivateActions`
  returns `true` over an empty/disabled list, `CheckAllActionsFinished` sees
  `ActiveActions.Count == 0` and raises `FINISHED` immediately (`FsmState.cs:280-283`, `:609-620`).
  This is what makes multi-hop chains within one frame routine (§6).
- The `finished` latch is per-entry and cleared in `OnEnter` (`:275`) and `OnExit` (`:625`); a
  state cannot raise `FINISHED` twice without an intervening exit/entry.
- A state with no `FINISHED` transition simply latches `finished` and stops ticking `OnUpdate`
  (`:344`) — it does **not** exit. `FsmState.HasFinishedTransition` (`:685-696`) exists but is not
  consulted by the runtime.
- `Fsm.EventTarget` (`Fsm.cs:542`) redirects the *next* self-`Event` call and is nulled by
  `EnterState` (`:2377`) and `UpdateStateChanges` (`:2324`); the only writer is the
  `SetEventTarget` action
  (`analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/SetEventTarget.cs:15`).

### 3.4 Transitions whose target state has no actions

Nothing special: `SwitchState → EnterState → OnEnter → ActivateActions(0)` over an empty array
returns `true`, `CheckAllActionsFinished` raises `FINISHED`, `DoTransition` queues the next hop,
and the `while` in `UpdateStateChanges` takes it — all inside the same `UpdateStateChanges` call.

### 3.5 Events to a not-started / stopped / disabled FSM

- Not started (`Started == false`): `ProcessEvent` calls `Start()` first (`Fsm.cs:2029-2032`), so
  the FSM enters `startState`, runs its `OnEnter` chain, and only then sees the event.
- Stopped (`Finished == true`, i.e. after `OnDisable`): `Active` is false → the event is dropped
  at `Fsm.cs:2025`.
- `ActiveState == null` (after `StopAndReset`): `Active` false → dropped.
- The event *causing* the disable is dispatched before the stop (`PlayMakerFSM.cs:394-397`).

### 3.6 Cross-FSM delivery — targets, immediacy, ordering

`Fsm.Event(FsmEventTarget, FsmEvent)`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:2126-2182`) is the single fan-out point.
`eventTarget == null` is coerced to a static self target (`:2129-2132`, `:244`).

| `FsmEventTarget.EventTarget` | delivery | cite |
|---|---|---|
| `Self` | `this.ProcessEvent(e)` | `Fsm.cs:2139-2141` |
| `GameObject` | `BroadcastEventToGameObject(go, e, data, sendToChildren, excludeSelf)` | `:2142-2147`, `:2242-2270` |
| `GameObjectFSM` | `SendEventToFsmOnGameObject(go, fsmName, e)` | `:2148-2153`, `:2280-2307` |
| `FSMComponent` | `eventTarget.fsmComponent.Fsm.ProcessEvent(e)` | `:2154-2159` |
| `BroadcastAll` | `BroadcastEvent(e, excludeSelf)` | `:2160-2162`, `:2222-2232` |
| `HostFSM` | `Host.ProcessEvent(e)` | `:2163-2168` |
| `SubFSMs` | each of `SubFsmList` (over a copy) | `:2169-2174` |

(enum: `analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmEventTarget.cs:8-17`.)

**The dump carries these targets in full** (this contradicts review gap G11, which read the
`"__unserialized": true` marker as data loss). `FsmDumper` tags `FsmEventTarget` as an unhandled
type but still reflects its members into `__fields`, and all six survive with usable values.
Measured over `analysis/fsm/GG_Hornet_1.json`: **2045** `FsmEventTarget` action fields, every one
carrying `target`, `excludeSelf`, `gameObject` (a full `FsmOwnerDefault` incl. `ownerOption` and
the resolved GameObject `path`), `fsmName`, `sendToChildren`, `fsmComponent`. Target census:
`GameObject` 1031, `Self` 851, `BroadcastAll` 122, `GameObjectFSM` 41 — and **no** `FSMComponent`,
`HostFSM` or `SubFSMs`, consistent with `hasHost`/`subFsmCount` being 0 on 962/962 (§2.7). The
interpreter can resolve every dispatch target statically at load.

**All delivery is synchronous.** There is no queue anywhere in `Fsm`; the only asynchrony is
`DelayedEvent` (§2.4) and `NextFrameEvent`'s reliance on the next `OnUpdate`.

Ordering:
- `BroadcastEvent` iterates a **snapshot copy** of `PlayMakerFSM.FsmList` (`Fsm.cs:2225`), so FSMs
  enabled *during* the broadcast are not visited. The list is append-on-`OnEnable` /
  remove-on-`OnDisable` (`analysis/decomp/PlayMaker/PlayMakerFSM.cs:365,398`), so iteration order is
  enable order **only if** Unity's `OnEnable` order is what the port assumes — that ordering is an
  ENGINE ASSUMPTION (Q-fsmrt-2), not a decomp fact, and it is observable broadcast-visible state.
  `PlayMakerFSM.BroadcastEvent` (static) does the same (`analysis/decomp/PlayMaker/PlayMakerFSM.cs:470-479`).
- `BroadcastEventToGameObject` first collects every `PlayMakerFSM` on the object by scanning the
  global `FsmList` (`Fsm.cs:2249-2255`), dispatches to all of them, and only then recurses into
  children in `transform.GetChild(i)` order (`:2263-2269`) — depth-first, parent before children.
- `SendEventToFsmOnGameObject` with an empty `fsmName` hits **every** FSM on the object; with a
  name it hits the **first** match and `break`s (`:2288-2306`).
- After dispatch, `Event` drains the *sender's* own pending switch when the sender is not the
  currently executing FSM (`:2176-2181`).

Receivers commit immediately (§2.2), because for any target other than `Self` the receiving FSM
sees `EventData.SentByFsm != this`.

**HK's own senders** (all outside any FSM execution stack unless called from an action):
- `PlayMakerFSM.SendEvent(string)` → `fsm.Event(name)` (`analysis/decomp/PlayMaker/PlayMakerFSM.cs:444-447`).
  Used by `HeroController` via `proxyFSM` — e.g. `proxyFSM.SendEvent("HeroCtrl-LeftGround")`
  (`analysis/decomp/Assembly-CSharp/HeroController.cs:3864`, also `:4951`, `:4993`;
  `proxyFSM = FSMUtility.LocateFSM(gameObject, "ProxyFSM")` at `:5015`).
- `FSMUtility.SendEventToGameObject(go, ev, isRecursive)` →
  `go.GetComponents<PlayMakerFSM>()` then `list[i].Fsm.Event(ev)` per component, then children
  (`analysis/decomp/Assembly-CSharp/FSMUtility.cs:155-183`). Note this uses the **`Fsm.Event`
  overload**, not `ProcessEvent`, so each receiver additionally runs the `:2176-2181` drain.
- `EventRegister.SendEvent(name)` → for each subscriber `ReceiveEvent()` →
  `FSMUtility.SendEventToGameObject(gameObject, subscribedEvent)`
  (`analysis/decomp/Assembly-CSharp/EventRegister.cs:26-31`, `:41-52`); subscriptions are a static
  `Dictionary<string, List<EventRegister>>` populated in `Awake` (`:8`, `:15-18`).

### 3.7 System events from Unity callbacks

Two independent paths reach an FSM from a Unity physics callback:

**(a) PlayMaker's own proxies.** `PlayMakerCollisionEnter2D.OnCollisionEnter2D` /
`PlayMakerTriggerEnter2D.OnTriggerEnter2D` iterate `TargetFSMs`, and for each with
`fsm.Active && fsm.Fsm.HandleCollisionEnter2D` (resp. `HandleTriggerEnter2D`) call
`fsm.Fsm.OnCollisionEnter2D(info)` / `OnTriggerEnter2D(other)`, then fire their own delegate list
(`analysis/decomp/PlayMaker/PlayMakerCollisionEnter2D.cs:6-17`,
`analysis/decomp/PlayMaker/PlayMakerTriggerEnter2D.cs:6-17`,
`analysis/decomp/PlayMaker/PlayMakerProxyBase.cs:126-132`, `:80-86`).
`Fsm.OnCollisionEnter2D` (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:2759-2775`):
```
PushFsm(this); Collision2DInfo = info; Collision2dName = info.gameObject.name;
if (ActiveState.OnCollisionEnter2D(info)) { UpdateStateChanges(); PopFsm(); }
else { Event(FsmEvent.CollisionEnter2D); UpdateStateChanges(); PopFsm(); }
```
`FsmState.OnCollisionEnter2D` (`FsmState.cs:476-486`) calls `DoCollisionEnter2D` on every
`ActiveActions` entry, runs `RemoveFinishedActions()` (note: unlike the Update path, removal
happens here directly), and returns `fsm.IsSwitchingState`. So an action that consumed the hit and
queued a transition suppresses the generic `COLLISION ENTER 2D` event. `Fsm.OnTriggerEnter2D`
(`:2813-2829`) has the same shape. **These handlers dereference `ActiveState` without a null
check** — a stopped FSM reached here would NRE; in practice the proxy's `fsm.Active` guard
prevents it.

**(b) HK's `PlayMakerUnity2DProxy`.** `OnCollisionEnter2D` / `OnTriggerEnter2D` on that component
optionally forward the system event by name via
`PlayMakerUnity2d.ForwardEventToGameObject(gameObject, "COLLISION ENTER 2D")` and then invoke a
delegate list (`analysis/decomp/Assembly-CSharp/PlayMakerUnity2DProxy.cs:143-154`, `:190-205`;
`analysis/decomp/Assembly-CSharp/PlayMakerUnity2d.cs:92-100`, event-name constants at `:27-37`).
Actions subscribe in `OnEnter` and unsubscribe in `OnExit`:
`Trigger2dEvent` (`analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/Trigger2dEvent.cs:41-60`,
handlers `:86-111`, 649 instances) and `CheckCollisionSideEnter`
(`analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/CheckCollisionSideEnter.cs:63-77`).
`CheckCollisionSideEnter.DoCollisionEnter2D` filters on layer name `"Terrain"` and then
raycasts 3 points per side over 0.08 world units, setting a bool variable and calling
`Fsm.Event(sideEvent)` for the first hit per side, in order top → right → bottom → left
(`:83-96`, `:98-158`; `RAYCAST_LENGTH = 0.08f` at `:49`).

**Timing.** Both paths are Unity `MonoBehaviour` messages; the decomp fixes *what* they do, not
*when* Unity raises them. That physics callbacks are raised inside the FixedUpdate phase is a
Unity-engine fact the decomp does not state → Q-fsmrt-2 (measurable: the recorder stamps a coarse
phase byte, `oracle/Oracle/TraceRecorder.cs:275-286`, `:713`).

---

## 4. Variables

### 4.1 Stores

`FsmVariables` holds 15 parallel typed arrays — `FsmFloat[]`, `FsmInt[]`, `FsmBool[]`,
`FsmString[]`, `FsmVector2[]`, `FsmVector3[]`, `FsmColor[]`, `FsmRect[]`, `FsmQuaternion[]`,
`FsmGameObject[]`, `FsmObject[]`, `FsmMaterial[]`, `FsmTexture[]`, `FsmArray[]`, `FsmEnum[]`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmVariables.cs:11-54`) — plus category metadata
(`:56-59`). The dump mirrors exactly these keys
(`analysis/fsm/GG_Hornet_1.json`, `variables`: `Float, Int, Bool, String, Vector2, Vector3, Rect,
Quaternion, Color, GameObject, Array, Enum, Object, Material, Texture`). Lookup is a linear name
scan per type, falling back to the global store, then a `LogMissingVariable` + fresh instance
(`FsmVariables.cs:1171-1197` for `GetFsmFloat`; the other 14 are the same shape).

### 4.2 `useVariable` / `Value` / `IsNone`

`NamedVariable` (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/NamedVariable.cs`) carries
`useVariable` (`:9-10`), `name` (`:12-13`) and a non-serialized `obj` exposed as `CastVariable`
(`:24-37`).
```
IsNone      => useVariable && string.IsNullOrEmpty(name)      # :132-142
UsesVariable=> useVariable && !string.IsNullOrEmpty(name)     # :144-154
```
`FsmFloat.Value` (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmFloat.cs:12-26`):
`CastVariable == null ? value : CastVariable.ToFloat()`. The same indirection exists on
`FsmInt/FsmBool/FsmString/FsmGameObject/FsmObject/FsmVector3` (`FsmBool.cs:16-20`,
`FsmInt.cs:16-20`, `FsmString.cs:16-20`, `FsmGameObject.cs:16-20`, `FsmObject.cs:56-60`,
`FsmVector3.cs:16-20`).

**Resolution happens at load, by aliasing, not copying.** `ActionData.GetFsmFloat`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/ActionData.cs:1181-1211`) returns the serialized
parameter unchanged when its `Name` is empty, and otherwise returns
**`fsm.GetFsmFloat(name)`** — the FSM's own variable instance
(`Fsm.cs:2532-2535` → `FsmVariables.cs:1171-1197`). So a named action parameter *is* the variable
object: writing `x.Value` from one action is immediately visible to every other action and to the
trace's per-FSM variable snapshot (`docs/trace-format.md`, FRAME → ENTITY → per-FSM `n_var`).
`CastVariable` is only ever assigned on the miss path
(`FsmVariables.cs:1195,1223,1251,1279,1307,1335,1363,1416,1519,1572`), i.e. for a *dangling* name.

**Interpreter model:** parameters are either (a) an immediate literal, or (b) a reference to a slot
in the owning FSM's typed store, resolved once at load by name. `IsNone` means "the field is
switched to variable mode with no variable selected" and is the ubiquitous "leave unchanged"
sentinel (e.g. `SetVelocity2d` skips an axis when `x.IsNone`,
`analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/SetVelocity2d.cs:70-77`; defaults
set in `Reset()` at `:30-37`).

### 4.3 Globals

`FsmVariables.GlobalVariables => PlayMakerGlobals.Instance.Variables`
(`FsmVariables.cs:64`), searched after the FSM-local arrays in every getter (`:1045-1055`,
`:1183-1191`). `PlayMakerGlobals` is a `ScriptableObject` loaded once from
`Resources.Load("PlayMakerGlobals")`; in a player build the loaded asset instance is used directly,
not copied (`analysis/decomp/PlayMaker/PlayMakerGlobals.cs:65-92`). It also owns the global event
name list (`:46-56`, `:99-121`), which `FsmEvent.AddGlobalEvents` interns at first use
(`FsmEvent.cs:508-514`). Global variables are **process-wide mutable shared state** across every
FSM — the interpreter needs one global store per world.
The global store **is** dumped: `FsmDumper.DumpGlobals` (`oracle/Oracle/FsmDumper.cs:116-130`,
called at `:112`) writes `analysis/dumps/<scene>/globals.json`. Measured for `GG_Hornet_1`
(`analysis/dumps/GG_Hornet_1/globals.json`): `present: true`, **31 variables** — 30 `GameObject`,
1 `Bool`, all 13 other buckets empty — and **51 global event names**. No `Float`/`Int`/`String`
global exists, so no numeric boss state hides in the global store: the interpreter's global store
is one bool, thirty object handles and the global-event name set.

### 4.4 `FsmVar` and arrays

`FsmVar` (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmVar.cs:7-48`) is the dynamically
typed box used by reflection-ish actions: `variableName` + `useVariable` + a `VariableType type`
plus one field per primitive (`floatValue`, `intValue`, `boolValue`, `stringValue`,
`vector4Value`, `objectReference`, `arrayValue`) and a lazily bound `NamedVar` (`:50-76`).
`ActionData` re-binds it at load: `fsmVar.NamedVar = fsm.Variables.GetVariable(fsmVar.variableName)`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/ActionData.cs:1415`), and the same for
`FsmVarOverride` inside `FsmTemplateControl` (`:1397-1403`).
`FsmArray` is a first-class variable type in the store (`FsmVariables.cs:50-51`,
accessor `:1523-1546`). The dumper serialises the `Array` bucket
(`analysis/fsm/GG_Hornet_1.json`, `variables.Array`).
Array/`FsmVar` **element semantics** were not read in depth here (`FsmArray.cs` unopened) →
Q-fsmrt-3.

### 4.5 Action deserialization (what the interpreter is loading)

`FsmState.Actions` lazily calls `actionData.LoadActions(this)`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmState.cs:161-175`, `:254-257`).
`ActionData.LoadActions` (`ActionData.cs:624-650`) builds one `FsmStateAction` per entry via
`CreateAction` (`:661-762`), which:
- resolves the type name via `ReflectionUtils.GetGlobalType` with a `TryFixActionName` fallback,
  substituting a `MissingAction` stub on failure (`:669-698`);
- compares a stable field-signature hash against the saved one and, on mismatch, calls
  `action.Reset()` + `TryRecoverAction` (`:706-728`) — a data-migration path that should be
  **dead** for shipped data and, if it fires, means the dump is not what the runtime executes;
- loads each public field in declaration order via `LoadActionField` (`:730-746`, `:764+`), with
  `FsmEvent` fields resolved through `FsmEvent.GetFsmEvent(stringParams[...])` when
  `DataVersion > 1` (`:772-779`);
- applies `customNames` and `actionEnabled[i] → action.Enabled` (`:747-759`).

The dump preserves exactly what the interpreter needs: per action `index`, `type`, `enabled`,
`fields[]` (`analysis/fsm/GG_Hornet_1.json`; `oracle/Oracle/FsmDumper.cs:230-271`, keys written at
`:263-264`). Census:
312 distinct action types and 23958 action instances in GG_Hornet_1 alone
(`analysis/fsm/GG_Hornet_1.json` `counts`), 200 types / 4389 instances on the boss objects
themselves (`analysis/specs/fsm-census.md`).

---

## 5. Interaction with the mod's regime

`FsmPauseGate.Install` (`oracle/Game/FsmPauseGate.cs:29-43`) MonoMod-hooks exactly three methods
and makes each a no-op while `Time.timeScale <= 0` (`:45-64`, gate at `:57`):
`PlayMakerFSM.Update`, `PlayMakerLateUpdate.LateUpdate`, `PlayMakerFixedUpdate.FixedUpdate`.
`TrainingEnv` sets `Time.timeScale = 1f` before `RaiseStepBegin`
(`oracle/Environment/TrainingEnv.cs:596-601`) and `Time.timeScale = 0` after the
`frames_per_wait` frame-skip loop (`:616-617`, `:627`).

### What the gate stops
Everything reached through `Fsm.Update` / `Fsm.FixedUpdate` / `Fsm.LateUpdate`: action
`OnUpdate`/`OnFixedUpdate`/`OnLateUpdate`, `StateTime` accumulation
(`FsmState.cs:346`), the `FINISHED` raised by `CheckAllActionsFinished` from those sweeps, and
**`DelayedEvent` ticking** — `UpdateDelayedEvents` is only called from `Fsm.Update`
(`Fsm.cs:1910`). `Continue()`'s deferred `EnterState` (`Fsm.cs:1906-1909`) is also stalled.

### What still happens during a frozen frame
The gate covers the *drivers*, not the *dispatch surface*. Still live, by construction:
1. **Any `Fsm.Event` / `ProcessEvent` from non-FSM code.** `PlayMakerFSM.SendEvent`
   (`analysis/decomp/PlayMaker/PlayMakerFSM.cs:444-447`), `FSMUtility.SendEventToGameObject`
   (`analysis/decomp/Assembly-CSharp/FSMUtility.cs:163-183`), `EventRegister.SendEvent`
   (`analysis/decomp/Assembly-CSharp/EventRegister.cs:41-52`), the static
   `PlayMakerFSM.BroadcastEvent` (`:462-479`). `HeroController.Update` is not gated, and it calls
   `proxyFSM.SendEvent(...)` (`analysis/decomp/Assembly-CSharp/HeroController.cs:3864,4951,4993`,
   and 16 more sites). Because the sender is outside the receiver's execution stack,
   `DoTransition` commits **immediately** (`Fsm.cs:2340-2343`), so `ExitState`, `EnterState`, the
   new state's `OnEnter` action list, and any chain they trigger all execute while the world is
   frozen. **Measured**: 589 `FSM_EVENT` and 146 `FSM_TRANSITION` records in
   `analysis/traces/p0/r2_rand1.a.hktrace` fall on frames with no `FRAME` record (= frames outside
   the timeScale-1 frame-skip window); 37 such frames lie strictly inside the step range, carrying
   75 transitions, including `Hornet Boss 1 / Control: Thrown → Throw Recover`,
   `Hornet Boss 1 / Stun Control: Stop → Reset Counter → Idle`, `Needle / Control: Return → Notify`
   and 70 `Knight / ProxyFSM: Idle ↔ Left Ground`.
2. **`OnEnable` / `OnDisable`.** Activating or deactivating a GameObject from ungated C# runs
   `Fsm.OnEnable` (restart in `startState`, `Fsm.cs:1839-1856`) or
   `PlayMakerFSM.OnDisable` → `fsm.Event(FsmEvent.Disable)` → `Stop()`
   (`analysis/decomp/PlayMaker/PlayMakerFSM.cs:392-403`). Record 4646 of
   `analysis/traces/p0/r2_rand1.a.hktrace` is exactly this: `Needle / Control  DISABLE` on frozen
   frame 25061.
3. **The other 20 proxy `MonoBehaviour`s** (`PlayMakerTriggerEnter2D`, `PlayMakerCollisionEnter2D`,
   `PlayMakerMouseEvents`, `PlayMakerApplicationEvents`, … — `PlayMakerFSM.cs:212-315`) are not
   hooked. With `timeScale = 0` Unity runs no physics step, so 2D collision/trigger callbacks
   should not fire, but that is an ENGINE ASSUMPTION and nothing in the mod enforces it →
   Q-fsmrt-2.
4. **Coroutines started by actions.** `FsmStateAction.StartCoroutine` runs
   `PlayMakerFSM.DoCoroutine` on the component
   (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmStateAction.cs:191-194`,
   `analysis/decomp/PlayMaker/PlayMakerFSM.cs:377-390`), which pushes/pops the execution stack
   around each `MoveNext`. Unity's coroutine scheduler is untouched by `FsmPauseGate`. Whether a
   given yield instruction resumes at `timeScale = 0` is a Unity fact, not a decomp fact →
   Q-fsmrt-2.

**Simulator consequence.** The interpreter must expose event dispatch as an entry point
independent of the per-frame tick, and the world driver must be able to run dispatch on a frame
where no tick runs. Modelling the gate as "FSMs are frozen" would be wrong.

### Port scope: the Knight's own FSMs (review gap G2)

Every FSM record on a frozen frame in §5 belongs to the Knight, and `analysis/specs/hero-motion.md`
defers the
Knight's PlayMaker layer to this spec, so it is scoped here explicitly. **These FSMs are part of the
FSM port, on the same interpreter as the boss FSMs** — nothing about them is special-cased; they are
listed so no worker assumes someone else owns them.

`analysis/fsm/GG_Hornet_1.json` carries **12** `PlayMakerFSM`s whose `gameObject` is `Knight`
(`path: "Knight"`), not 13; the same 12 names, and only those 12, appear as `owner: "Knight"` FSM
records across all four R2 traces (`analysis/traces/p0/r2_{idle,move,rand1,rand2}.a.hktrace`) — see
Q-fsmrt-5. Eight carry `handleFixedUpdate`, none carries `handleLateUpdate`, five have
`restartOnEnable: false`:

| fsmName | handleFixedUpdate | restartOnEnable | states | globalTransitions | startState | activeState at dump |
|---|---|---|---|---:|---|---|
| `Spell Control` | **true** | **false** | 98 | 4 | `Pause` | `Inactive` |
| `Dream Nail` | **true** | true | 47 | 3 | `Init` | `Inactive` |
| `Nail Arts` | **true** | **false** | 35 | 3 | `Init` | `Inactive` |
| `Dream Return` | **true** | true | 28 | 1 | `Init` | `Idle` |
| `Superdash` | **true** | **false** | 28 | 4 | `Init` | `Inactive` |
| `Map Control` | **true** | **false** | 25 | 4 | `Pause` | `Inactive` |
| `Surface Water` | **true** | **false** | 13 | 3 | `Init` | `Inactive` |
| `Roar Lock` | **true** | true | 11 | 3 | `Init` | `Detect` |
| `ProxyFSM` | false | true | 16 | 0 | `Init` | `Idle` |
| `Control Interpolation` | false | true | 5 | 0 | `Idle` | `Idle` |
| `Spore Cooldown` | false | true | 5 | 0 | `Init` | `Idle` |
| `Globalise` | false | true | 1 | 1 | `Init` | `Init` |

(all values from `analysis/fsm/GG_Hornet_1.json`, keys `fsmName` / `handleFixedUpdate` /
`handleLateUpdate` / `restartOnEnable` / `states` / `globalTransitions` / `startState` /
`activeState`; `handleLateUpdate` is false on all 12, matching the scene-wide count of exactly one
`handleLateUpdate` FSM, `Orbit Shield(Clone)/Control`.)

Port notes:
- `ProxyFSM` is the hero→PlayMaker bridge: `HeroController.proxyFSM =
  FSMUtility.LocateFSM(gameObject, "ProxyFSM")` (`analysis/decomp/Assembly-CSharp/HeroController.cs:5015`)
  and 19 `proxyFSM.SendEvent(...)` call sites (`:1921 … :5293`). It is also the busiest FSM in the
  traces (2700 of the 7973 `owner: "Knight"` FSM records across the four R2 traces) and the one that
  fires on frozen frames (§5). It has **no** `handleFixedUpdate`, so it ticks in `Update` only.
- The five `restartOnEnable: false` FSMs (`Map Control`, `Spell Control`, `Nail Arts`, `Superdash`,
  `Surface Water`) take the **resume-mid-state, no re-`OnEnter`** branch of §1.5. All five sit at
  `activeState: Inactive` at SceneReady, so the branch is reachable in a normal fight the moment
  one of them is disabled and re-enabled.
- The eight `handleFixedUpdate` FSMs are driven by the shared `PlayMakerFixedUpdate` proxy on the
  Knight GameObject (§2.5) — one component, eight targets — so they tick in FixedUpdate **and**
  Update, and can commit transitions in either.
- All 12 are inside the `FsmPauseGate` for ticking but outside it for dispatch (§5): a frozen frame
  runs `HeroController.Update`, which sends `HeroCtrl-*` events into `ProxyFSM`, which fans out to
  the rest through `FSMUtility.SendEventToGameObject`.

---

## 6. Trace validation

Corpus `analysis/traces/p0/r2_rand1.a.hktrace` (regime R2, `GG_Hornet_1`, seeded, 480 FRAMEs,
10092 records). Reader: `harness/hktrace.py`. Structure reference:
`analysis/fsm/GG_Hornet_1.json`, FSM `Boss Holder/Hornet Boss 1 : Control`
(77 states, 1 global transition `STUN → Stun Start`, `startState: Pause`,
`handleFixedUpdate: true`).

**Recorder semantics that must be read into the records** (`oracle/Oracle/TraceRecorder.cs:191-243`):
- `FSM_EVENT` hooks `Fsm.Event(FsmEvent)` (`Fsm.cs:2192`) and is emitted **pre-`orig`**. It
  therefore misses every `Fsm.Event(FsmEventTarget, FsmEvent)` call (`Fsm.cs:2126`) — which is
  what `SendEvent`, `SendEventByName` and `SetEventTarget`-style actions use
  (`analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/SendEvent.cs:39,62`,
  `SendEventByName.cs:35,58`) — and every direct `ProcessEvent` (broadcasts). It *does* catch
  `SendRandomEvent` (`SendRandomEvent.cs:33`), `BoolTest` (`BoolTest.cs:31,40`),
  `CheckCollisionSideEnter` (`CheckCollisionSideEnter.cs:126,136,146,156`),
  `PlayMakerFSM.SendEvent` and `FSMUtility.SendEventToGameObject`.
- `FSM_TRANSITION` hooks `Fsm.SwitchState` (`Fsm.cs:2347`) and is emitted **post-`orig`**, i.e.
  after `ExitState` + `EnterState` and everything they cascade. Records are therefore in
  **post-order**: a state's own entry-time events appear *before* the transition record that
  entered it, and an enclosing `SwitchState` is recorded after every transition nested inside it.
- `phase` is a sticky global set by the recorder behaviour's `Update`/`FixedUpdate`/`LateUpdate`
  and by the HeroController hooks (`oracle/Oracle/TraceRecorder.cs:35, 256-259, 275-286`). It is a
  weak witness of the Unity phase, not a proof.

### Case A — external event, single hop (records 442–443, frame 24748, phase 1)
```
442 FSM_EVENT      Hornet Boss 1 / Control   LAND
443 FSM_TRANSITION Hornet Boss 1 / Control   GG Fall -> GG Land
```
`GG Fall`'s transition list is exactly `[(LAND, GG Land)]` and its actions include
`CheckCollisionSideEnter` and `CheckCollisionSide`, both with `bottomHitEvent = LAND`
(`analysis/fsm/GG_Hornet_1.json`). `CheckCollisionSideEnter` fires from the
`PlayMakerUnity2DProxy` collision delegate
(`analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/CheckCollisionSideEnter.cs:71,83-96`),
so the event enters via `Fsm.Event(FsmEvent)` → `ProcessEvent` → state transition (no global
transition matches `LAND`). Matches §2.2 + §3.2 + §3.7. The `phase 1` stamp is *consistent with*
a physics-phase callback but does not prove it, and the trace cannot distinguish
"committed immediately inside `DoTransition`" from "queued and drained one call later", since both
land on the same frame.

### Case B — chain of 3, all inside one `UpdateStateChanges` (records 868–873, frame 24766, phase 0)
```
868 FSM_EVENT      FINISHED
869 FSM_EVENT      FINISHED
870 FSM_TRANSITION GG Land   -> GG Reset
871 FSM_EVENT      FINISHED
872 FSM_TRANSITION GG Reset  -> GG Music
873 FSM_TRANSITION GG Music  -> Flourish
```
Reading, with the post-order rule: 868 is `GG Land` completing under `FsmState.OnUpdate` →
`CheckAllActionsFinished` → `Fsm.Event(FINISHED)` (`FsmState.cs:353,617`); it queues
`GG Land → GG Reset` (that state's only transition). `UpdateState` then calls
`UpdateStateChanges` (`Fsm.cs:2410`) → `SwitchState(GG Reset)` → `EnterState` → `GG Reset`'s two
actions (`SetInvincible`, `SetDamageHeroAmount`) both finish in `OnEnter`, so `ActivateActions`
returns true and `CheckAllActionsFinished` raises **869** and queues `GG Reset → GG Music`; the
enclosing `SwitchState` then returns and writes **870**. Loop lap 2: `SwitchState(GG Music)` →
`GG Music`'s first two actions are `enabled: false` (skipped, `FsmState.cs:292-296`), its third is
`SendEventByName` with `eventTarget = BroadcastAll` — invisible to the recorder (`Fsm.Event(target, e)`),
which is why no record sits between 870 and 872 — then the state finishes, raising **871** and
queuing `GG Music → Flourish`; `SwitchState` returns → **872**. Lap 3: `SwitchState(Flourish)` →
`Tk2dPlayAnimationWithEvents` does not finish, no event → **873** with nothing before it.
All 6 records are contiguous global indices on one frame. Matches §2.3 and §3.3/§3.4 exactly.

### Case C — chain of 4 with mid-list abort (records 2049–2056, frame 24855, phase 0)
```
2049 FSM_EVENT      FINISHED
2050 FSM_EVENT      RUN
2051 FSM_TRANSITION Flourish   -> Idle
2052 FSM_EVENT      FINISHED
2053 FSM_TRANSITION Idle       -> Flip?
2054 FSM_EVENT      FINISHED
2055 FSM_TRANSITION Flip?      -> Run Away?
2056 FSM_TRANSITION Run Away?  -> Run Antic
```
2049 = `Flourish` finishing (queues `→ Idle`). 2050 is raised **inside `EnterState(Idle)`**:
`Idle`'s action 6 is `SendRandomEvent` with events `[null, RUN]` at weights `[0.5, 0.5]`, which
calls `Fsm.Event(events[i])` (`SendRandomEvent.cs:33`); `Idle` has `(RUN, Flip?)` as its first
transition, so `switchToState = Flip?` and `ActivateActions` returns false at `FsmState.cs:307-310`
— **actions 7 (`SendRandomEventV2`) and 8 (`WaitRandom`) never run this entry**, and `OnEnter`
skips `CheckAllActionsFinished`, which is why no `FINISHED` appears for `Idle`. 2051 is the
post-order record of `SwitchState(Idle)`. Same shape for `Flip?` (action 0 `SendRandomEvent`
`[null, FINISHED]` → 2052, then abort, → 2053) and `Run Away?` (2054, → 2055). 2056 has no
preceding event because `Run Antic`'s `Tk2dPlayAnimationWithEvents` does not finish on entry.
Matches §2.3 (both chaining mechanisms) and §2.2 (self-events deferred to the enclosing drain).

### Aggregate checks on the same trace
- Transitions per (frame, FSM): 690 frames with 2, 149 with 1, 87 with 4, 61 with 3, 7 with 8,
  7 with 6, 4 with 5, 2 with 7, **2 with 16** (`SD Crystal(Clone) / Control`, frames 25411 and
  25426). Multi-hop chaining is the norm, not an edge case, and 16 ≪ `MaxLoopCount` 1000.
- Phase histogram: `FSM_TRANSITION` 1577 phase-0 / 621 phase-1 / 26 phase-2;
  `FSM_EVENT` 2979 / 919 / 44. Non-zero phase-1 and phase-2 counts are consistent with
  `FixedUpdateState`/`LateUpdateState` committing transitions (§2.5) and with the 79 FSMs in this
  scene carrying `handleFixedUpdate`.

### Anomalies / things the trace does not confirm
1. **Frozen-frame transitions.** 75 transitions on timeScale-0 frames (§5). Record window 4645–4652
   (frame 25061) shows a nested cross-FSM cascade under one `SwitchState`:
   `Needle/Control FINISHED` → [`Needle/Control DISABLE`, `Hornet Stun Control FINISHED`+2
   transitions, `Hornet Control Thrown → Throw Recover`] → `Needle/Control Return → Notify`.
   The bracketing is exactly what post-order recording of one enclosing `SwitchState` produces, and
   the immediate-commit rule (§2.2) explains why the Hornet transitions happen inside the Needle's
   `EnterState`. The *originating* caller is not recorded (the `Fsm.Event(target, e)` overload and
   `ProcessEvent` are unhooked), so this reconstruction is consistent-with, not proven →
   `analysis/open-questions.md` Q19.
2. **Duplicate-looking `FINISHED` pairs** (868/869, 2049/…): not duplicates — each belongs to a
   different state, separated only by the post-order transition record. A naive reader that
   attributes an event to the *preceding* transition will mis-assign every one of them.
3. `DoTransition`'s immediate vs deferred branch is **not observable** in this trace format (both
   land on the same frame and in the same record order). Verifying it needs a hook on
   `Fsm.DoTransition` or `UpdateStateChanges` → `analysis/open-questions.md` Q19.

---

## 7. Interpreter ABI proposal (orchestrator owns the final contract; this is input)

### 7.1 Static, per FSM asset (loaded from `analysis/fsm/<scene>.json`, immutable, shared)
```
fsm_def   { name, start_state:u16, n_states, states[], n_global:u16, globals[],
            var_layout[15 typed spans], flags{ handle_fixed, handle_late,
                                               restart_on_enable, max_loop:u16 } }
state_def { name, is_sequence:u8, n_actions:u16, action0:u32,
            n_trans:u16,  trans[] }                      # FsmState.cs:44,161,181
trans     { event_id:u32, to_state:u16 }                 # FsmTransition.cs:24-40, resolved at load
action_def{ type_id:u16, enabled:u8, param_block:u32 }   # ActionData.cs:747-760
```
`event_id` is a world-global interned string id (§3.1). `to_state` = `0xFFFF` for an unresolvable
`ToState`, which `DoTransition` must treat as "no transition, keep scanning"
(`Fsm.cs:2329-2333`). Every flag is now a dump key, not a guess (`restartOnEnable`,
`handleFixedUpdate`, `handleLateUpdate`, `maxLoopCount` — `oracle/Oracle/FsmDumper.cs:176-182`).
`manual_update`, `keep_delayed_on_exit`, `is_breakpoint`, `host`/`sub_fsm` are omitted from the
struct deliberately: measured constant across 962/962 FSMs and 7539/7539 states (§2.3, §2.4,
§2.7), so the port hard-codes `ManualUpdate = false`, always-kill-delayed-events, no breakpoints
and no sub-FSMs, and must **trap** if a future dump contradicts any of them.

### 7.2 Mutable, per FSM instance
```
fsm_rt    { active_state:u16, switch_to:u16(NONE), prev_state:u16, last_transition:u32,
            started:u8, finished:u8, active_state_entered:u8, switched_state:u8,
            event_target:u8, var_store*, delayed[] }              # Fsm.cs:107,138,141,144,354,558,795
state_rt  { active:u8, finished:u8, state_time:f32, loop_count:u16,
            active_action_index:u16, active_actions:[u16], finished_actions:[u16] }  # FsmState.cs:10-16,53,56,58,62
action_rt { enabled:u8, active:u8, finished:u8, entered:u8, <per-type state> }        # FsmStateAction.cs:15,19,21,122
```
`active_actions` must be an ordered, index-stable list with removal deferred to
`RemoveFinishedActions` (§2.1). `entered` is sticky for the FSM's lifetime (§2.6). `loop_count`
lives on the *state*, is bumped in `ENTER`, and is zeroed for **all** states at the end of every
`DRAIN` (§2.3). There is **no event queue**: `switch_to` is the entire pending-transition state.

### 7.3 Per-frame entry points the world driver needs
```
fsm_update(f)        # PlayMakerFSM.Update — per-frame; iteration order = FsmList (Q-fsmrt-2)
fsm_fixed(f)         # PlayMakerFixedUpdate — only if f.handle_fixed && f.Active
fsm_late(f)          # PlayMakerLateUpdate  — only if f.handle_late  && f.Active
fsm_send(f, ev)                      # Fsm.Event(FsmEvent)          — Fsm.cs:2192
fsm_send_to(f, target, ev)           # Fsm.Event(FsmEventTarget,…)  — Fsm.cs:2126
fsm_process(f, ev, sent_by_fsm)      # Fsm.ProcessEvent             — Fsm.cs:2023
fsm_on_enable(f) / fsm_on_disable(f) # Fsm.cs:1839 / PlayMakerFSM.cs:392
fsm_physics_2d(f, kind, other)       # Fsm.OnCollisionEnter2D … — Fsm.cs:2634-2865
```
`fsm_process` must take the sender identity explicitly (not read it from a global) because the
immediate-vs-deferred commit hinges on `EventData.SentByFsm != this` (`Fsm.cs:2340`). The
execution stack (`FsmExecutionStack.cs:61-73`) can be a single per-world `u32` depth + top pointer.

### 7.4 Action dispatch
One C function per action type, five slots — `on_preprocess`, `awake`, `on_enter`, `on_update`,
`on_fixed_update`, `on_late_update`, `on_exit`, `on_event`, plus the 16 `Do*` physics hooks
(`FsmStateAction.cs:168-317`). Unimplemented types trap with the type name (PLAN §2.3). The census
(`analysis/specs/fsm-census.md`) is the implementation order; note that 5 of the top 20 by count
(`SendEventByName` 4383, `NextFrameEvent` 1408, `SendMessage` 1255, `SendEvent` 1000,
`Wait` 2497) are pure control-flow and must land with the interpreter itself.

---

## 8. Open questions

One `### Q-fsmrt-<n>` heading + one paragraph each, per the coordinator's format.
`analysis/open-questions.md` is consolidated by script and is not edited from this spec.

**Already consolidated — do not re-file.** The former FSM-Q1 / FSM-Q3 / FSM-Q5 (RestartOnEnable,
ManualUpdate, KeepDelayedEventsOnStateExit, MaxLoopCountOverride, IsBreakpoint, ExposedEvents,
Host/SubFsmList, PlayMakerGlobals) are `analysis/open-questions.md` **Q17**, CLOSED 2026-08-31 —
§1.5, §2.3, §2.4, §2.7 and §4.3 now quote the dumped values instead of assuming defaults. The
former FSM-Q7 (recorder blind spots) is **Q19**. The §5 frozen-frame dispatch finding is **Q18**,
RESOLVED 2026-08-31 by `analysis/specs/frame-order.md`.

### Q-fsmrt-1 — do templated-but-uninitialized FSMs dump the template's states or a stale copy?
68 of 962 FSMs in `analysis/fsm/GG_Hornet_1.json` are `template != null` with
`initializedBeforeDump: false`, so `PlayMakerFSM.Init` — and therefore `InitTemplate`
(`analysis/decomp/PlayMaker/PlayMakerFSM.cs:158-166`), which swaps `fsm` for a fresh copy of
`FsmTemplate.fsm` — never ran for them; the dumper forced only the data half, `Fsm.InitData`
(`oracle/Oracle/FsmDumper.cs:158-163`). Their dumped `states` are therefore the *serialized
component copy*, not the instantiated template, and if the two differ every pooled object that uses
a template will be ported wrong. Positive but weak evidence that they agree: the 4 templates that
appear in both the initialized and the uninitialized group have identical state-name lists, and all
68 have non-empty state lists (min 1, median 7, 1390 actions total). *Evidence needed:* dump the
`FsmTemplate.fsm` assets directly and diff them against the component copies.

### Q-fsmrt-2 — Unity-engine facts the decomp cannot supply, currently asserted as assumptions
Five statements in this spec are engine behaviour, not decompiled code, and are flagged inline as
ENGINE ASSUMPTION: (a) Unity does not call `Awake`/`OnEnable`/`Start`/`Update` on components of
inactive GameObjects (§1.4 — consistent with 116 `initializedBeforeDump: false` FSMs per scene, but
not proven by them); (b) `MonoBehaviour.Update` runs exactly once per rendered frame (§2.6, §7.3);
(c) `PlayMakerFSM.FsmList` iteration order equals component enable order, which is broadcast-visible
state (§3.6); (d) 2D collision/trigger callbacks are raised in the FixedUpdate physics sub-phase and
not at all while `timeScale = 0` (§3.7, §5); (e) which coroutine yield instructions resume at
`timeScale = 0` (§5). This overlaps `analysis/open-questions.md` Q4 and is partly answered by
`analysis/specs/frame-order.md`; the parts that are not are the ones the fsm port depends on.
*Evidence needed:* targeted oracle hooks (an ordering probe MonoBehaviour, a broadcast-order probe),
not more reading.

### Q-fsmrt-3 — `FsmArray` / `FsmVar` element semantics are unread
`FsmArray` is a first-class variable bucket (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmVariables.cs:50-51`,
accessor `:1523-1546`) and is dumped (`analysis/fsm/GG_Hornet_1.json`, `variables.Array`);
`FsmVar` is the dynamically typed box rebound at load by
`analysis/decomp/PlayMaker/HutongGames.PlayMaker/ActionData.cs:1415`. Neither
`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmArray.cs` nor `FsmVar.cs:78+` was read for this
spec, so element typing, growth, index bounds and the `FsmVar`→`NamedVariable` conversion table are
UNKNOWN. Nothing in the P4 Hornet backlog needs them yet, but every array-touching action
(`ArrayForEach`, `GetArrayItem`, …) is blocked until they are read. *Evidence needed:* read the two
files; no runtime experiment required.

### Q-fsmrt-4 — `SendMessage` is an escape hatch out of the FSM model
`HutongGames.PlayMaker.Actions.SendMessage` has 1255 instances across the four boss scenes
(`analysis/specs/fsm-census.md`), 20th by frequency. It calls arbitrary HK `MonoBehaviour` methods
by name through Unity reflection, so its effects are outside everything §2–§4 describes and cannot
be derived from the FSM dump alone. Its receiver set in the boss scenes has not been enumerated, so
the interpreter cannot know which of the 1255 instances are inert (audio, UI) and which mutate
simulated state. *Evidence needed:* enumerate `functionCall.FunctionName` + resolved target
component across `analysis/fsm/*.json`, then classify; the trap-on-unimplemented rule (PLAN §2.3)
must cover unknown receivers, not just unknown action types.

### Q-fsmrt-5 — Knight FSM count: 12 measured, 13 in the review brief
The coordinator's brief asks for "the Knight's 13 PlayMakerFSMs". Both independent sources say 12:
`analysis/fsm/GG_Hornet_1.json` has 12 entries with `gameObject: "Knight"` / `path: "Knight"`, and
exactly the same 12 `fsm` names appear as `owner: "Knight"` in FSM records across all four R2 traces
(`analysis/traces/p0/r2_{idle,move,rand1,rand2}.a.hktrace`). The §5 *Port scope* table therefore
lists 12. A 13th would have to be a component added after SceneReady that never emits an event, or a
count that includes something off the `Knight` root (`Knight/Charm Effects` also carries 12 FSMs, on
a child object). *Evidence needed:* whoever produced the 13 should name it; otherwise 12 stands.

---

## Review fixes

Applied 2026-08-31 in response to `analysis/specs/REVIEW-p1.md` (verdict PASS-WITH-FIXES). Every row
of that review naming `analysis/specs/fsm-runtime.md`, plus the two cross-spec rows and the port gap
that land here.

| id | class | change |
|---|---|---|
| D44 | STALE (5 riders) | Deleted every "not dumped / assume the default" statement. §1.5 now reads `restartOnEnable` from `analysis/fsm/GG_Hornet_1.json` (true 954 / **false 8**) and tables the 8, five of which are `Knight/*`; §2.3 quotes `maxLoopCountOverride` 0 and `maxLoopCount` 1000 on 962/962 plus `isBreakpoint` false on 0/7539; §2.4 quotes `keepDelayedEventsOnStateExit` false on 962/962; §2.7 quotes `manualUpdate` false, `hasHost` false, `subFsmCount` 0, `usedInTemplate` false on 962/962 (so `HostFSM`/`SubFSMs` targets need no port) and the `exposedEvents` histogram; §4.3 quotes `analysis/dumps/GG_Hornet_1/globals.json` (31 variables — 30 GameObject, 1 Bool — and 51 global events). Dumper cites re-pointed to `oracle/Oracle/FsmDumper.cs:178-186` and `:116-130`. Former FSM-Q1/Q3/Q5 removed from §8 and marked consolidated into `analysis/open-questions.md` Q17 (CLOSED). |
| D45 | OMISSION (port hazard) | §2.2 gained a **Port hazard** block: `FsmState.OnEvent`'s loop body is `flag = fsmStateAction.Event(fsmEvent);` (`FsmState.cs:326`), an assignment that overwrites each iteration, so the return `fsm.IsSwitchingState \|\| flag` (`:328`, `flag` initialised false at `:321`) reflects **only the last active action**. Stated that `flag \|= …` would be a wrong port because it suppresses transitions the real runtime takes. |
| D46 | MISREAD (count) | §3.1 "53 system events" → **55**, cited as 55 `AddSystemEvent("…")` calls in `FsmEvent.cs:440-497` (the 56th grep match is the method declaration at `:499`). |
| D47 | OMISSION | §1.2 now states that `Fsm.Awake` re-runs `CheckFsmEventsForEventHandlers()` when `!preprocessed` (`Fsm.cs:1598-1601`), and ties it to the `HandleFixedUpdate`-setter-clears-`preprocessed` hazard immediately below. |
| D48 | WRONG-LINE (stale) | `globalTransitions` re-pointed from `Fsm.cs:518` (the property) to the field at `Fsm.cs:58-59`, keeping `:518` labelled as the accessor. `FsmDumper` cites re-pointed against the current file: `comp.Fsm` `:130`→`:148`, forced `InitData` `:138-145`/`:140-144`→`:158-163`, `globalTransitions` `:171`→`:199`, action emit `:202-250`/`:234-235`→`:230-271`/`:263-264`. |
| D49 | UNCITED | Five engine facts are now explicitly labelled ENGINE ASSUMPTION and routed to Q-fsmrt-2: the inactive-GameObject callback rule (§1.4, with the 116-FSM dump evidence that is consistent with it but does not prove it), "once per rendered frame" (§2.6 pseudocode header and §7.3), `FsmList` order = enable order (§3.6, reworded so the cited part is the append/remove sites `PlayMakerFSM.cs:365,398` and the *ordering* is the assumption), physics-callback phase and `timeScale = 0` behaviour (§3.7, §5), coroutine resume rule (§5). |
| D38 / C3 | MISREAD (count) | §2.5 "50 action types set `HandleFixedUpdate`" → **51**: 50 in `analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/` plus `analysis/decomp/Assembly-CSharp/CheckTrackTriggerCount.cs:42`, which sits outside that directory. Now agrees with `analysis/specs/frame-order.md:170`. |
| G2 | port gap (unowned) | New §5 subsection **Port scope: the Knight's own FSMs**, claiming them for the FSM port and tabling all of them with `handleFixedUpdate` / `handleLateUpdate` / `restartOnEnable` / state and global-transition counts / `startState` / `activeState` from the dump, plus `ProxyFSM`'s role as the hero bridge and the eight-target shared `PlayMakerFixedUpdate` proxy. **Count is 12, not 13** — see Q-fsmrt-5. |
| G11 | gap **rejected** (new evidence) | §3.6 now records that the dump does *not* lose `FsmEventTarget` data: the `"__unserialized": true` marker is a type tag, and `__fields` carries all six members with usable values on **2045/2045** instances in `analysis/fsm/GG_Hornet_1.json` (targets: `GameObject` 1031, `Self` 851, `BroadcastAll` 122, `GameObjectFSM` 41; no `FSMComponent`/`HostFSM`/`SubFSMs`). Every dispatch target resolves statically at load; no dumper fix is needed. |
| C6 | cross-spec | No change needed — the review resolved C6 in this spec's favour, and §1.5 now cites the dumped `restartOnEnable` that carries the argument (`Needle / Control` is `true` ⇒ restarts in `startState` on every enable). |
| C11, C12 | cross-spec | Confirmed CONSISTENT by the reviewer; no change. |
| — | format | §8 reformatted to `### Q-fsmrt-<n> — <title>` + one paragraph, with a header listing what is already consolidated (Q17 closed, Q18 resolved, Q19 open). `analysis/open-questions.md` untouched. |

Not addressed here because they belong to other specs: D01-D09 (boss-hornet), D34-D43 (frame-order,
tk2d), D50-D56 (fsm-actions), C1/C2/C4/C5/C7-C10/C13/C14, G9-G15 other than G11.
