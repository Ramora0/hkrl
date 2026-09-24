# P1.2 — frame ordering under regime R2

Scope: which Unity callback runs which HK system, in what order, under R2
(`Time.captureDeltaTime = Time.fixedDeltaTime = 0.02`, `frames_per_wait = 2`,
`Time.timeScale` toggled 0/1 by the mod, `-batchmode` renderer alive).
Sources: `analysis/traces/p0/r2_move.a.hktrace` (movement_all, 300 steps, 12257 records)
and `analysis/traces/p0/r2_rand1.a.hktrace` (random_s1, 240 steps, 10092 records), both
`GG_Hornet_1`, `capture_dt=0.02 fixed_dt=0.02 frames_per_wait=2` (trace header `capture`);
decomp under `analysis/decomp/`; recorder `oracle/Oracle/TraceRecorder.cs`.
Method: records grouped by their `frame` field (= `Time.frameCount`, written by every
record kind — `oracle/Oracle/TraceRecorder.cs:414` (OBS), `:433` (FRAME), `:691` (FIXED/HC_*),
`:711` (EVENT)), stream order preserved,
grouping started at `EVENT SCENE_READY` (arming, `docs/trace-format.md` §Arming).

---

## 1. Observed per-frame record order

### 1.1 Record census (post-arming)

| kind | r2_move.a | r2_rand1.a |
|---|---|---|
| FRAME | 600 | 480 |
| FIXED | 606 | 486 |
| HC_FIXED_PRE / HC_FIXED_POST | 606 / 606 | 486 / 486 |
| HC_UPDATE_PRE / HC_UPDATE_POST | 908 / 908 | 728 / 728 |
| HC_LATE_PRE / HC_LATE_POST | **0 / 0** | **0 / 0** |
| OBS | 301 | 241 |
| EVENT STEP | 300 | 240 |
| EVENT FSM_EVENT / FSM_TRANSITION | 5255 / 2153 | 3942 / 2224 |
| EVENT HERO_DAMAGE | 7 | 44 |

`HC_LATE_*` is absent because `HeroController` has no `LateUpdate`: grepping
`analysis/decomp/Assembly-CSharp/HeroController.cs` for a `LateUpdate` declaration returns
nothing (the only Unity per-frame callbacks it declares are `Update` :904 and `FixedUpdate` :910),
so the recorder's `HookHcVoid("LateUpdate", …)` (`oracle/Oracle/TraceRecorder.cs:138`) takes the
`m == null` branch at `:253` and installs no hook. (The mod's runtime log confirms this, but that
log lives outside `analysis/` and is therefore not cited here — the decomp grep is sufficient.)

### 1.2 Two frame classes

Grouping by `Time.frameCount` yields 909 frames (r2_move) / 729 (r2_rand1), each with
**exactly 0 or 1** `FIXED` record and **exactly 1** `HC_UPDATE_PRE` (one exception: the
`SCENE_READY` frame, 0):

| | r2_move.a | r2_rand1.a |
|---|---|---|
| live frames (1 FIXED) | 606 | 486 |
| frozen frames (0 FIXED) | 303 | 243 |
| — of those, carrying an `HC_UPDATE_PRE/POST` pair | **302** | **242** |

The one frozen frame without an `HC_UPDATE` pair is the `SCENE_READY` frame in each trace
(909 − 1 = 908 `HC_UPDATE_PRE`, 729 − 1 = 728). Quote 302/242 when the quantity wanted is
"rendered frames on which `HeroController.Update` ran with no FixedUpdate".

`FIXED` never appears after `HC_FIXED_PRE` in the same frame (0 violations in both traces)
⇒ `RecorderBehaviour.FixedUpdate` (`oracle/Oracle/TraceRecorder.cs:277`) is dispatched before
`HeroController.FixedUpdate` in the FixedUpdate list.

### 1.3 Canonical patterns (FSM_EVENT/FSM_TRANSITION runs collapsed to `fsm`, adjacent duplicates collapsed)

r2_move.a — 22 distinct signatures, top 6 = 854/909 frames:

```
 231  HC_UPDATE_PRE HC_UPDATE_POST STEP                                              (frozen)
  68  HC_UPDATE_PRE fsm HC_UPDATE_POST STEP                                          (frozen)
 211  FIXED HC_FIXED_PRE HC_FIXED_POST fsm HC_UPDATE_PRE HC_UPDATE_POST FRAME        (live, 1st of step)
 200  FIXED HC_FIXED_PRE HC_FIXED_POST fsm HC_UPDATE_PRE HC_UPDATE_POST FRAME OBS    (live, last of step)
  79  FIXED HC_FIXED_PRE HC_FIXED_POST fsm HC_UPDATE_PRE fsm HC_UPDATE_POST FRAME OBS
  65  FIXED HC_FIXED_PRE HC_FIXED_POST fsm HC_UPDATE_PRE fsm HC_UPDATE_POST FRAME
```

r2_rand1.a — 28 distinct signatures, top 6 = 641/729 frames: `203 / 35 / 179 / 121 / 74 / 29`
for the same six shapes. Remaining signatures differ only by an interleaved
`ev` (HERO_DAMAGE) or by an `fsm` run after `HC_UPDATE_POST` / after `FRAME`.

Full-fidelity live frame (uncollapsed, example r2_move.a records 1527–1543, frame 24945):

```
FIXED  HC_FIXED_PRE  HC_FIXED_POST  [fsm×k]  HC_UPDATE_PRE  [fsm×m]  HC_UPDATE_POST  [fsm×n]  FRAME  [OBS]  [fsm×p]
```

Frozen frame (example r2_move.a records 3179–3198, frame 25135):

```
[fsm×q]  HC_UPDATE_PRE  [fsm×m]  HC_UPDATE_POST  EVENT STEP
```

### 1.4 Where FSM records cluster, and what raises them

FSM_EVENT is emitted PRE-`orig` of `Fsm.Event(FsmEvent)`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:2192`, hook at
`oracle/Oracle/TraceRecorder.cs:230-245`); FSM_TRANSITION is emitted POST-`orig` of
`Fsm.SwitchState` (`Fsm.cs:2347`, hook `TraceRecorder.cs:202-220`). So a nested cascade
prints as: outer event, inner events, inner transitions, outer transition.

Counts by position, r2_move.a / r2_rand1.a (EVENT + TRANSITION combined):

| slot | move | rand1 | dominant owners |
|---|---|---|---|
| before `FIXED` | 3 | 10 | HeroLight, Needle, Hornet Boss 1 (frozen frames only — §3.2) |
| between `HC_FIXED_PRE` and `HC_FIXED_POST` (inside `HeroController.FixedUpdate`) | 51 | 0 | Knight/`ProxyFSM` `HeroCtrl-DashEnd`, `DASH END` broadcast |
| between `HC_FIXED_POST` and `HC_UPDATE_PRE` | 3472 | 3897 | Evade Check/FSM, Floor Check/FSM, Hornet Boss 1/Control, SD Crystal(Clone), Knight |
| between `HC_UPDATE_PRE` and `HC_UPDATE_POST` (inside `HeroController.Update`) | 3706 | 1968 | Knight/`ProxyFSM` `HeroCtrl-LeftGround` + `LEFT GROUND` broadcast |
| between `HC_UPDATE_POST` and `FRAME` | 46 | 137 | Run Effects(Clone), SD Crystal(Clone), Floor Check |
| after `FRAME` | 130 | 154 | Hornet Boss 1, G Dash Effect, Flash Effect, Charm Effects, Hit L/R |

**Boss/scene FSMs vs HeroController-raised events — the discriminator.**

* **Inside `HC_UPDATE_PRE..POST`** every record is raised from inside `HeroController.Update`.
  Owner breakdown — r2_move: Knight 3621, Run Effects(Clone) 62, Shadow Recharge 11,
  Charm Effects 5, Shadow Ring(Clone) 3, White Flash DJ 2, Dash Burst 2 (3706 total);
  r2_rand1: Knight 1785, Run Effects(Clone) 164, Charm Effects 10, CameraParent 8,
  White Flash NA 1 (1968 total). Event breakdown — `LEFT GROUND` 2556/1260,
  `FINISHED` 443/248, `HeroCtrl-LeftGround` 213/105, `RUN STOP` 13/31.
  The dominant burst is verbatim (r2_move.a records 3180–3196, frame 25135; 15 FSM_EVENT +
  2 FSM_TRANSITION):
  `ProxyFSM HeroCtrl-LeftGround` → `LEFT GROUND` to `ProxyFSM, Spell Control, Map Control,
  Roar Lock, Superdash, Nail Arts, Surface Water, Dream Nail, Dream Return, Spore Cooldown,
  Globalise, Control Interpolation` → `ProxyFSM FINISHED` → `Idle → Left Ground` →
  `FINISHED` → `Left Ground → Idle`.
  Sources: `HeroController.FallCheck()` calls `proxyFSM.SendEvent("HeroCtrl-LeftGround")`
  at `analysis/decomp/Assembly-CSharp/HeroController.cs:3864`, and `FallCheck()` is called
  from `orig_Update()` at `HeroController.cs:5113` (`Update` :904 → `orig_Update` :5106);
  `RUN STOP` is `runEffect.GetComponent<PlayMakerFSM>().SendEvent("RUN STOP")` at
  `HeroController.cs:5129` and `:5146`, also inside `orig_Update`.
* **Inside `HC_FIXED_PRE..POST`** the owner is `Knight` again:
  `proxyFSM.SendEvent("HeroCtrl-DashEnd")` at `HeroController.cs:4235` inside
  `FinishedDashing()` (:4230), reached from `Dash()` (:1508) called at `HeroController.cs:1029`
  inside `FixedUpdate()` (:910).
* **Between `HC_FIXED_POST` and `HC_UPDATE_PRE`** the owners are dominated by boss/scene FSMs
  (Evade Check, Floor Check, Hornet Boss 1/Control, Text/Dialogue Page Control, Area Title,
  Godseeker Crowd, SD Crystal(Clone)), with `Knight` and the hero's attack colliders also
  present. The slot can be split further by the recorder's `phase` byte, written into every
  EVENT at `TraceRecorder.cs:713` — but **the byte is a latch, not a phase probe**: `_phase` is assigned at `TraceRecorder.cs:279` (`RecorderBehaviour.FixedUpdate` → 1),
  `:285` (`RecorderBehaviour.Update` → 0), `:286` (`RecorderBehaviour.LateUpdate` → 2),
  `:256` and `:259` (both HeroController hooks, pre- and post-`orig`, to that callback's own
  code — 1 for FixedUpdate, 0 for Update), `:357` (`SceneReady` → 0) and `:429` (`OnFrame` → 0);
  its initial value is 3 (`:35`). So a record's `phase` says only *which of those writers ran
  most recently*, never which Unity callback list is currently executing:
  * `phase == 0` (2694 move / 2357 rand1): the Update phase has demonstrably begun
    (`RecorderBehaviour.Update` already ran this frame) yet `HeroController.Update` has not.
    Owners: Evade Check 2362/1526, Hornet Boss 1 52/48, Floor Check 0/162, Text 54,
    Area Title 33, SD Crystal(Clone) 0/144.
    ⇒ **`PlayMakerFSM.Update` (`analysis/decomp/PlayMaker/PlayMakerFSM.cs:369-374` →
    `Fsm.Update()` `Fsm.cs:1895`) runs BEFORE `HeroController.Update` for these FSM
    instances.**
  * `phase == 1` (778 move / 1540 rand1). Full owner census — r2_move: Knight 421,
    DownSlash 72, **Evade Check 62**, Refight Range 36, A Sphere Range 36, Slash 24,
    Run Away Check 16, Charm Effects 13, Damage Effect 13, Hornet Boss 1 8, Sphere Range 8,
    A Dash Range 8, CameraParent 7, Health 6, Evade Range 4, SD Crystal Gen G1/G2/W1/W2 4 each,
    AudioManager 4; r2_rand1: Knight 468, **Evade Check 418**, **Floor Check 164**,
    Hit R 104, Hit L 62, Refight Range 36, Charm Effects 26, Damage Effect 26, Slash 24,
    CameraParent 19, A Sphere Range 16, Run Away Check 16, Sphere Range 16, Evade Range 16,
    Health 12, SD Burst 10, SD Crystal Gen G1/G2/W1/W2 8 each.
    **This is a bound, not an attribution.** All the byte establishes is: emitted at or after
    `RecorderBehaviour.FixedUpdate` and before `RecorderBehaviour.Update` of the same frame.
    It is tempting to read the slot as Unity's physics-callback sub-phase — `Knight`,
    `Slash`/`DownSlash`/`Hit L`/`Hit R` are the hero's attack colliders and
    `HeroController.OnCollisionStay2D` (`HeroController.cs:4906`, `→ HeroCtrl-LeftGround` :4951)
    / `OnCollisionExit2D` (:4961, :4993) are trigger/collision callbacks — but the census also
    contains plain boss/scene FSMs (`Evade Check`, `Floor Check`, `Hornet Boss 1`) that have no
    collider callback, and those same FSMs dominate the `phase == 0` half of the slot. The
    trace cannot separate "late in the FixedUpdate/physics block" from "early in the Update
    block, before `RecorderBehaviour.Update`". **UNKNOWN — Q-frame-1**; the phase markers of
    blind spot #1 settle it.
* **After `HC_UPDATE_POST`** (46/137) and **after `FRAME`** (130/154) there are further
  FSM records, mostly from objects spawned during the episode (`Run Effects(Clone)`,
  `SD Crystal(Clone)`, `G Dash Effect`, `Flash Effect`, `Throw Effect`) plus
  `Hornet Boss 1` (74 move / 53 rand1 at `phase==0`). ⇒ **`PlayMakerFSM.Update` is not a
  single contiguous block relative to `HeroController.Update`**: the large majority of FSM
  instances tick before it, a minority after. The per-instance ordering is not recoverable
  from the current trace (§5).
* **`phase == 2` records** (24 move / 70 rand1) exist only *after* the frame's `FRAME`
  record — **0 phase-2 records precede a `FRAME` in the same frame, in both traces**.
  Owners: G Dash Effect 11, Flash Effect 11, Charm Effects 24, Throw Effect 11,
  A Dash Effect 11, Hit L 9, Cyclone Slash 7, Hit R 7, Area Title 1.
  ⇒ the mod's coroutine resume (`FRAME`) precedes **every LateUpdate that runs at or after
  `RecorderBehaviour.LateUpdate`** (`TraceRecorder.cs:286`). It does **not** prove that `FRAME`
  precedes *all* LateUpdates: a `LateUpdate` dispatched earlier in the list than
  `RecorderBehaviour`'s would emit its records carrying the stale `phase == 0`, and such a
  record is indistinguishable from an Update-phase one. The 130/154 post-`FRAME` records that
  carry `phase == 0` (owners `Hornet Boss 1` 74/53, `CameraParent` 5/6, `Shadow Recharge` 8,
  …) are exactly the ambiguous population: other coroutines resuming after ours, later
  `Update()`s, or early `LateUpdate()`s — not separable. **Partially UNKNOWN — Q-frame-2**;
  blind spots #1 and #2 settle it.

---

## 2. Unity callback → HK system map

Proven by the traces / decomp. "not readable" = script execution order is not exposed at
runtime and no dump captures it.

### FixedUpdate (0 or 1 per rendered frame under R2, §3.2)
| system | evidence |
|---|---|
| `HKOracle.TraceRecorder.RecorderBehaviour.FixedUpdate` (emits `FIXED`, `FixedCount++`) | `oracle/Oracle/TraceRecorder.cs:277-283`; always first in the frame (0 inversions) |
| `HeroController.FixedUpdate` — recoil steps, `Move(move_input)` (:976 → :1249), `Dash()` (:1029), gravity/jump/dash state machine, `positionHistory`, `cState.wasOnGround` (:1207-1209) | `analysis/decomp/Assembly-CSharp/HeroController.cs:910-1210`; `HC_FIXED_PRE/POST` bracket it |
| `PlayMakerFixedUpdate.FixedUpdate` → `Fsm.FixedUpdate()` → `FixedUpdateState` → `state.OnFixedUpdate()` + `UpdateStateChanges()` | `analysis/decomp/PlayMaker/PlayMakerFixedUpdate.cs:6-16`; `Fsm.cs:1950-1957`, `:2399-2404` |
| — which FSMs: those with `Fsm.HandleFixedUpdate`, set by **51 classes** calling `base.Fsm.HandleFixedUpdate = true` — **50** under `analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/` (e.g. `AddForce2d.cs:63`, `ChaseObject.cs:51`) **plus `analysis/decomp/Assembly-CSharp/CheckTrackTriggerCount.cs:42`**, which is outside that directory (this is why `fsm-runtime` L323 says 50 — both are right, different scopes); the proxy component is added in `PlayMakerFSM.Preprocess` (`PlayMakerFSM.cs:286-289`) | dumps: `analysis/fsm/GG_Hornet_1.json` — **79 PlayMakerFSM *instances*** of 962 have `handleFixedUpdate:true` (56 distinct `(gameObject, fsmName)` pairs), incl. `Hornet Boss 1/Control`, `Evade Check/FSM`, `Floor Check/FSM`, `Needle/Control`, and **8** `Knight/*` FSMs (`Dream Nail, Dream Return, Map Control, Nail Arts, Roar Lock, Spell Control, Superdash, Surface Water`). Other scenes (instances / distinct pairs / `Knight/*`): 88 / 57 / 8 (FK), 77 / 54 / 8 (Gruz), 80 / 57 / 8 (MMC) |
| Unity physics step + `OnCollision*2D` / `OnTrigger*2D` — `HeroController.OnCollisionStay2D` (:4906), `OnCollisionExit2D` (:4961), hero attack-collider FSMs (`Slash`, `DownSlash`, `Hit L/R`) | `phase==1` FSM records between `HC_FIXED_POST` and `HC_UPDATE_PRE` (778 move / 1540 rand1) |

### Update (1 per rendered frame, live **and** frozen)
| system | evidence |
|---|---|
| InControl device sampling → `InputDeviceShim.Update(updateTick, deltaTime)` — **listed here provisionally; the trace does not place it in Update** | `oracle/Game/ProxyController.cs:96-113`. InControl is **not decompiled** (`analysis/decomp/Assembly-CSharp-firstpass/` contains only Steamworks + UnityStandardAssets), so the callback that drives it is unknown. §4 bounds it only to the open interval **after `ApplyAction` in frame *F*'s coroutine phase and before `HeroController.Update` in frame *F+1***; nothing excludes frame *F+1*'s FixedUpdate or physics sub-phase. **UNKNOWN — Q-frame-3** |
| `InputHandler.Update` → `PlayingInput()` | `analysis/decomp/Assembly-CSharp/InputHandler.cs:263-270` |
| `PlayMakerFSM.Update` → `Fsm.Update()` (delayed events + `UpdateState` + `UpdateStateChanges`) | `PlayMakerFSM.cs:369-374`; `Fsm.cs:1895-1921`, `:2406-2411`. Majority of instances before `HeroController.Update`, a minority after (§1.4) |
| `HeroController.Update` → `ModHooks.OnHeroUpdate()` then `orig_Update()`: `Update10()` every 10th `Time.frameCount` (:5108), `current_velocity = rb2d.velocity` (:5112), `FallCheck()` (:5113), `FailSafeChecks()`, run-effect FSM sends, `attack_time/recoilTimer/... += Time.deltaTime`, `LookForInput()` (:5194 → `move_input` at :3262), `LookForQueueInput()` (:5279 → `HeroJump`/`HeroDash`/`DoAttack` at :3344/:3369/:3381) | `HeroController.cs:904-908`, `:5106-…`; bracketed by `HC_UPDATE_PRE/POST` |
| `HeroAnimationController.Update` → `UpdateAnimation()` (chooses the clip) | `HeroAnimationController.cs:71-84` |
| `CameraTarget.Update` | `CameraTarget.cs:146` |
| `GameManager.Update` — `currentLoadDuration += Time.unscaledDeltaTime`, `IncreaseGameTimer`, `UpdateEngagement()` | `GameManager.cs:365-377` |
| `HKOracle.RecorderBehaviour.Update` (sets `_phase = 0` only) | `oracle/Oracle/TraceRecorder.cs:285` |

`TimeController` has **no** Unity callback — it is a static (`TimeController.cs:3`) whose
`GenericTimeScale` setter writes `Time.timeScale` (:79).

### Coroutine resume after `yield return null`
| system | evidence |
|---|---|
| `TrainingEnv.Step` frame-skip loop: `yield return null` then `Oracle.Hooks.RaiseFrame()` → `FRAME` record | `oracle/Environment/TrainingEnv.cs:614-624` (`RaiseFrame` at :617); `docs/trace-format.md` §0x01 |
| Position: **after** all `Update()`s recorded this frame (`FRAME` always follows `HC_UPDATE_POST` in its frame group) and **before** any LateUpdate-phase record (0 `phase==2` records precede `FRAME`) | §1.4 |
| After the last loop iteration, still in the same frame: `Time.timeScale = 0` (:627), reward accounting, obs build (`GetSplitFeatures` :748, `GetGlobalState` :749-750, `SnapshotFsms` :759, i.e. :748-759), `RaiseObs` (:762) → `OBS` record, `SendMessage` | `TrainingEnv.cs:614-762`; trace shows `FRAME OBS` adjacent, same `frame` (e.g. r2_move.a records 1542,1543) |

### LateUpdate
| system | evidence |
|---|---|
| `tk2dSpriteAnimator.LateUpdate()` → `UpdateAnimation(Time.deltaTime)` — **this is what advances animation** | `analysis/decomp/Assembly-CSharp/tk2dSpriteAnimator.cs:586-589` (`UpdateAnimation` :433) |
| `PlayMakerLateUpdate.LateUpdate` → `Fsm.LateUpdate()` → `LateUpdateState` → `state.OnLateUpdate()` + `UpdateStateChanges()` | `PlayMakerLateUpdate.cs:6-16`; `Fsm.cs:1960-1967`, `:2413-2418` |
| — which FSMs: `Fsm.HandleLateUpdate`, set by 13 action classes: `GetAngleToTarget`, `LookAt`, `QuaternionBaseAction`, `Rotate`, `SetPosition`, `SetRotation`, `SetScale`, `SmoothFollowAction`, `SmoothLookAt`, `SmoothLookAt2d`, `SmoothLookAtDirection`, `Translate`, `TranslateV2` (all in `analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/`, `base.Fsm.HandleLateUpdate = true`; several gated on a `lateUpdate`/`perSecond` field) | dumps: **exactly 1** FSM per scene has `handleLateUpdate:true` — `Orbit Shield(Clone)/Control` — in all four of `analysis/fsm/GG_{Hornet_1,False_Knight,Gruz_Mother,Mega_Moss_Charger}.json` |
| `CameraController.LateUpdate` — camera follow, reads `hero_ctrl.cState` / `hero_ctrl.transform.position` | `CameraController.cs:188-…` |
| `HKOracle.RecorderBehaviour.LateUpdate` (sets `_phase = 2`) | `oracle/Oracle/TraceRecorder.cs:286` |
| `HeroController` — **none** | §1.1 |

**Not readable / not proven.** Unity's script execution order table is not exposed at
runtime and is not in any dump. What the traces prove is only the *observed* relative order
of the hooked points, per frame, in this build+scene: `FIXED` before `HeroController.FixedUpdate`;
most `PlayMakerFSM.Update` before `HeroController.Update`, some after; the mod coroutine
after all `Update`s and before any LateUpdate-phase FSM record. Nothing in the traces
constrains the order of `InputHandler.Update`, `CameraTarget.Update`,
`HeroAnimationController.Update`, or `GameManager.Update` against each other or against
`HeroController.Update`.

---

## 3. Time semantics under R2

### 3.1 Per-FRAME scalars (all 600 / 480 FRAME records)

| field | r2_move.a | r2_rand1.a |
|---|---|---|
| `dt` (`Time.deltaTime`) | `0.02` ×600 (single value) | `0.02` ×480 |
| `time_scale` | `1.0` ×600 | `1.0` ×480 |
| Δ`frame` between consecutive FRAMEs | `1` ×300, `2` ×299 | `1` ×240, `2` ×239 |
| Δ`fixed_count` between consecutive FRAMEs | **`1` ×599** | **`1` ×479** |
| Δ`Time.time` | `0.02` ×461, `0.019999` ×133, `0.019997` ×5 (f32 accumulation) | `0.02` ×364, `0.019999` ×115 |
| Δ`Time.fixedTime` | identical to Δ`Time.time` | identical |
| `Time.time − Time.fixedTime` | `0.018398`/`0.018396`/`0.018394` | `0.003468`/`0.003466` |
| `unscaled_dt` | wall clock, varies (0.000347…0.011139 in the first 6 frames) | idem |

**Exactly one FixedUpdate per rendered live frame** (Δ`fixed_count` = 1 for every consecutive
FRAME pair, including the Δ`frame`=2 pairs that straddle a frozen frame) — this is the
property R2 was chosen for (`docs/frame-order.md` §Regime R2, `STATE.md`).
`Time.time − Time.fixedTime` is a per-run constant (the accumulator residual fixed at boot),
not a per-frame varying quantity: 3 distinct f32 values across 600 frames, all within 4e-6.
`Time.unscaledDeltaTime` is **not** pinned by `captureDeltaTime` — it tracks wall time; any
HK/PlayMaker code reading it (or `Time.realtimeSinceStartup`, or `FsmTime.RealtimeSinceStartup`,
`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmTime.cs:16-30`) is a nondeterminism source
(cf. `ShakePositionV2`, `docs/frame-order.md`).

### 3.2 The frozen frame

`Time.timeScale = 0` is written by `TrainingEnv.Step` after the frame-skip loop
(`oracle/Environment/TrainingEnv.cs:627`) and restored to `1f` at the top of the next
`Step()` (:596), before `ActionDecoder.ApplyAction` (:598) — the ordering comment at :591-595
states why. Measured consequences on the intervening frame:

* **The frozen frame contributes nothing to `Time.time`.** Between the last FRAME of step *N*
  (frame *F*) and the first FRAME of step *N+1* (frame *F+2*), Δ`Time.time` = 0.02, while the
  FRAME at *F+2* itself records `dt = 0.02`. On the accumulation identity
  `Time.time(n) = Time.time(n−1) + dt(n)` — which the Δ`frame`=1 pairs support directly
  (Δ`Time.time` = the next FRAME's own `dt`, 599/599 move, 479/479 rand1) — this gives
  `dt(F+1) = 0` for the frozen frame.
  **What this does NOT establish**: that a mid-frame `Time.timeScale` write is invisible to
  the current frame's `deltaTime`. An earlier draft argued that from "frame *F* still reported
  `dt = 0.02` after the `timeScale = 0` write at `:627`" — that is circular: `FRAME.dt` for
  frame *F* is sampled at `RaiseFrame` (`TrainingEnv.cs:617`), which runs **before** the write
  at `:627`, so it cannot witness a same-frame effect. No record samples `deltaTime` after a
  mid-frame `timeScale` write. **UNKNOWN — Q-frame-4**; blind spot #3 (a `FRAME_LITE` from
  `RecorderBehaviour.Update`, plus a `dt` field on a LateUpdate record) settles it.
* **No FixedUpdate.** 303 / 243 frames carry 0 `FIXED` records, and Δ`fixed_count` across a
  Δ`frame`=2 FRAME pair is 1, not 2.
* **`HeroController.Update` still runs** — 1 `HC_UPDATE_PRE`/`POST` pair on every frozen frame.
  With `dt = 0`: every `+= Time.deltaTime` timer in `orig_Update` (`attack_time` :5202,
  `recoilTimer` :5174, `hardLandingTimer` :5161, `dashLandingTimer` :5153, `fallTimer` :3875) is frozen,
  but the **dt-free** parts still execute: `Update10()` on `Time.frameCount % 10 == 0` (:5108),
  `current_velocity = rb2d.velocity` (:5112), `FallCheck()`'s `rb2d.velocity.y <= -1e-06`
  branch → `cState.falling/onGround/wallJumping` writes + `proxyFSM.SendEvent("HeroCtrl-LeftGround")`
  (:3855-3864), `FailSafeChecks()`, `LookForInput()` (`move_input` latch), and
  `LookForQueueInput()` (`WasPressed` edge consumption → `HeroJump`/`HeroDash`/`DoAttack`).
  This is exactly why `Time.timeScale = 1` must precede `ApplyAction` (`TrainingEnv.cs:591-596`).
  Observed cost: the `HeroCtrl-LeftGround` cascade (§1.4) fires on **71/303** (move) and
  **35/243** (rand1) frozen frames. (68 was the count of frozen frames matching the single
  collapsed signature `HC_UPDATE_PRE fsm HC_UPDATE_POST STEP`; counting frozen frames that
  contain a `HeroCtrl-LeftGround` `FSM_EVENT` gives 71.) These are the frozen frames on which
  `FallCheck`'s guard `rb2d.velocity.y <= -1E-06f` (`HeroController.cs:3855`) held and
  `CheckTouchingGround()` was false — i.e. descending and off the ground, which is narrower
  than "airborne" (a frozen frame at the apex of a jump, `v.y > 0`, does not fire it).
* **PlayMaker is gated off**: `FsmPauseGate` returns early from `PlayMakerFSM.Update`,
  `PlayMakerLateUpdate.LateUpdate`, `PlayMakerFixedUpdate.FixedUpdate` while
  `Time.timeScale <= 0` (`oracle/Game/FsmPauseGate.cs:33-59`). Consistent with the data:
  of 1210 (move) / 605 (rand1) FSM records on frozen frames, 1207 / 595 have owner `Knight`
  and sit inside `HC_UPDATE_PRE..POST` (the `HeroCtrl-LeftGround` cascade).
* **The gate is not airtight** — 3 (move) / 10 (rand1) frozen-frame FSM records escape it,
  all *before* `HC_UPDATE_PRE`. r2_rand1.a records 4645–4652, frame 25061:
  `Needle/Control FINISHED` → `Needle/Control DISABLE` → `Hornet Boss 1/Stun Control FINISHED`
  → `Stop→Reset Counter` → `FINISHED` → `Reset Counter→Idle` → `Hornet Boss 1/Control
  Thrown→Throw Recover` → `Needle/Control Return→Notify`. A boss FSM changed state on a
  frozen frame. `DISABLE` is `FsmEvent.Disable`
  (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmEvent.cs:443`), raised from
  `PlayMakerFSM.OnDisable` (`PlayMakerFSM.cs:392-396`) — un-gated. The other un-gated path is
  `PlayMakerFSM.DoCoroutine` (`PlayMakerFSM.cs:377-388`): a Unity coroutine started by an
  action keeps resuming at `timeScale = 0`. Which one raised the initial
  `Needle/Control FINISHED` is **not determined by the trace** — Q-frame-5.
* **tk2d advances exactly one `dt` per agent-step *frame pair*, never more.** Animator clip
  time between consecutive FRAMEs with the same clip and `playing`: `0.02` for Δ`frame`=1
  (278 move / 161 rand1) *and* `0.02` for Δ`frame`=2, i.e. across the frozen frame
  (275 move / 230 rand1); the only other values are negative loop wraps (−0.18 ×2, −0.1 ×3).
  Net: `frames_per_wait × dt` of animation per agent step, matching the live-frame count.
  **Which LateUpdate contributed the second 0.02 is not separable.** `FRAME` is sampled before
  `RecorderBehaviour.LateUpdate` (§1.4) and there is no `dt` sample on a frozen frame, so the
  Δ`frame`=2 total of 0.02 is equally consistent with (a) `LateUpdate(F)` running at
  `dt = 0.02` and `LateUpdate(F+1)` at `dt = 0`, or (b) `LateUpdate(F)` at `dt = 0` (the
  `timeScale = 0` write at `TrainingEnv.cs:627` precedes it) and `LateUpdate(F+1)` at
  `dt = 0.02` (the `timeScale = 1` write at `:596` precedes it). This is the same ambiguity as
  Q-frame-4. What *is* measured, and is what the sim must reproduce, is the per-step total.
  Mechanism cited: `tk2dSpriteAnimator.LateUpdate` passes `Time.deltaTime`
  (`analysis/decomp/Assembly-CSharp/tk2dSpriteAnimator.cs:586-589`).

### 3.3 The agent step, measured

Per-step signature = the records of one segment `[EVENT STEP_i, EVENT STEP_{i+1})`, each
token rendered as `f+<frame−frame(STEP_i)>:<kind>`, adjacent duplicates collapsed. The final
step is **excluded** (its segment runs to EOF and picks up the 8 post-loop frames), leaving
**299 segments in r2_move.a and 239 in r2_rand1.a**. Distinct signatures, by how FSM records
are treated:

| FSM record treatment | r2_move.a | r2_rand1.a |
|---|---|---|
| stripped (dropped entirely) | **3** | **3** |
| each collapsed to a `fsm` token | 24 | 28 |
| verbatim | 78 | 89 |

Stripped, the three signatures are the same shape in both traces and differ **only** by where
`EVENT HERO_DAMAGE` falls: r2_move 295 clean / 3 with damage on both live frames / 1 with
damage on `f+1` only; r2_rand1 218 clean / 20 on both / 1 on `f+2` only. (An earlier draft
reported "5 / 6" from a per-frame *kind-set* signature; that rule is superseded by the one
stated here.) The invariant:

```
f+0  frozen : [fsm*] HC_UPDATE_PRE [fsm*] HC_UPDATE_POST  EVENT STEP
f+1  live   : FIXED HC_FIXED_PRE HC_FIXED_POST [fsm*] HC_UPDATE_PRE [fsm*] HC_UPDATE_POST [fsm*] FRAME [fsm*]
f+2  live   : FIXED HC_FIXED_PRE HC_FIXED_POST [fsm*] HC_UPDATE_PRE [fsm*] HC_UPDATE_POST [fsm*] FRAME OBS [fsm*]
```

i.e. **1 frozen frame + `frames_per_wait` live frames per agent step**; 40 ms of game time
and exactly 2 FixedUpdates per step. Frame accounting: r2_move 909 = 1 pre-loop frame
(the `SCENE_READY` frame) + 300×3 loop frames + 8 post-loop frames;
r2_rand1 729 = 1 + 240×3 + 8.

### 3.4 What the sim's step loop must be

```
per agent step:
  # frozen frame  (Time.deltaTime = 0, no FixedUpdate)
  Update phase with dt = 0:
      HeroController.Update   (dt-free branches only: Update10 on frameCount%10,
                               FallCheck -> cState + LEFT GROUND broadcast, FailSafeChecks,
                               LookForInput -> move_input latch, LookForQueueInput)
      PlayMakerFSM.Update     -> SKIPPED (FsmPauseGate)
  coroutine:  timeScale <- 1 ; ApplyAction(action)      # writes the raw key bitset
  LateUpdate phase: tk2d advance attributed to this frame OR to the previous one -- Q-frame-4;
                    either way the per-step total is frames_per_wait * dt
  frameCount += 1

  repeat frames_per_wait (=2) times:              # dt = 0.02, one FixedUpdate each
      FixedUpdate block:
          RecorderBehaviour.FixedUpdate            (sim: none)
          HeroController.FixedUpdate               (Move(move_input), Dash(), gravity, jump)
          PlayMakerFixedUpdate proxies             (79 instances in GG_Hornet_1)
          physics step -> OnCollision*/OnTrigger* callbacks
                                                   # that these run inside the fixed block,
                                                   # not early in Update, is Q-frame-1
      Update block:
          InControl device sample                  # input written last frame becomes visible by
                                                   # HeroController.Update; exact slot Q-frame-3
          PlayMakerFSM.Update  (most instances)
          HeroController.Update
          PlayMakerFSM.Update  (spawned-this-episode instances)
          HeroAnimationController.Update, CameraTarget.Update, GameManager.Update
      coroutine resume:  >>> OBSERVATION SAMPLED HERE <<<  (last iteration only)
          on the last iteration: timeScale <- 0, then build obs (hitboxes, global state,
          FSM snapshot), then emit
      LateUpdate block:
          PlayMakerLateUpdate proxies (1 FSM/scene)
          tk2dSpriteAnimator.LateUpdate(dt)        # animation advances HERE; the dt on the
                                                   # step's LAST live frame is Q-frame-4
          CameraController.LateUpdate
      frameCount += 1
```

The observation phase matters: the obs is taken **after** the last live frame's `Update`
and before every `LateUpdate` that runs at or after `RecorderBehaviour.LateUpdate`
(§1.4; whether any `LateUpdate` precedes it is Q-frame-2). So the animator state in the
observation is `clip_time` as of an earlier LateUpdate than the one belonging to the frame
the obs is stamped with — stale by one frame's advance, with the exact attribution
Q-frame-4. (`TrainingEnv.cs:614-762`.)

---

## 4. Input latency

`ActionDecoder.ApplyAction` writes the shim's key booleans at `TrainingEnv.cs:598`, in the
coroutine phase of the frozen frame *F* (the frame that carries `EVENT STEP`). Measured over
`r2_move.a` against `analysis/traces/p0/r2_move.corpus.json` (corpus index *i* ⇒ `EVENT STEP`
`step == i+1`, since `_stepCount++` precedes `RaiseStepBegin`, `TrainingEnv.cs:524,601`):

Selection and probe are stated per row so the counts are re-derivable; `n` is
`matched / selected`.

| transition | selection over `r2_move.corpus.json` | probe | n | key bit in `FRAME.input` | `cState` / clip in `FRAME` | `rb2d.velocity` at |
|---|---|---|---|---|---|---|
| walk start + L↔R turns | `action[0]` changes and lands in {0,1} (**no** filter on `action[2]`/`[3]`) | first `HC_FIXED_POST` with `rb_vel_x · dir > 1.0` within 6 frames | **32/32** | **F+1** | — | **F+2**, in `HC_FIXED_POST` |
| jump, executed | `action[3]` 1→0 | first `HC_FIXED_POST` with `rb_vel_y > 5.0` within 8 frames | **5/15** | F+1 | `cState.jumping` at F+1 | **F+2** `HC_FIXED_POST` |
| dash, executed | `action[2]` becomes 4 | first `HC_FIXED_POST` with `abs(rb_vel_x) > 19.0` within 6 frames | **3/6** | F+1 | `cState.dashing` at F+1 | **F+2** `HC_FIXED_POST` |
| attack, fresh | `action[2]`=0 and `cState.attacking` false in the FRAME before *F* | `cState.attacking` rising edge | **8/8** | F+1 | `cState.attacking` + `SlashAlt`/`Slash`/`UpSlash`/`DownSlash` at F+1 | n/a (attack sets no velocity) |

All 32 walk/turn transitions land at F+2 — no exceptions — and all 32 also move `rb_pos_x`
between the FRAME at F+1 and the FRAME at F+2.

The 15 jump presses (`action[3]` 1→0) resolve as: **5** at F+2 (the row above); **2**
(corpus 258, 282) at F+5 via the jump queue (`jumpQueueSteps++` in `FixedUpdate`
`HeroController.cs:1110`, drained at `:3391` `if (jumpQueueSteps <= JUMP_QUEUE_STEPS &&
CanJump() && jumpQueuing)` → `HeroJump()` :3393); and **8 with no attributable effect** —
7 that never reach `rb_vel_y > 5` within 8 frames (airborne, no double jump available), plus
corpus 144, whose `HC_FIXED_POST` at **F+1** already reads `vel=(+15.000,+7.500)`.

**Corpus 144 is damage recoil, not a jump.** `EVENT STEP` step=145 is at f25195
(r2_move.a rec 4553); the two frames before it carry `EVENT HERO_DAMAGE`
(`source="Hornet Boss 1"`, `amount=1`, `hazard_type=1`, `hp_after=8`) at f25193 (rec 4468)
and f25194 (rec 4536), and the FRAME at f25194 (rec 4549) reads
`cState.recoiling=1`, `cState.invulnerable=1`, `hero_state=7` = `ActorStates.no_input`
(`analysis/decomp/Assembly-CSharp/GlobalEnums/ActorStates.cs`, index 7). The F+1 velocity is
the recoil vector, not a jump: `StartRecoil` (`HeroController.cs:3782`) sets
`recoilVector = new Vector2(RECOIL_VELOCITY, RECOIL_VELOCITY * 0.5f)` for
`CollisionSide.left` (`:3794`; `:3801` is the mirrored right case) and `FixedUpdate` assigns
`rb2d.velocity = recoilVector` at `:955`. With `RECOIL_VELOCITY = 15.0`
(`analysis/dumps/GG_Hornet_1/hero.json#heroController.RECOIL_VELOCITY`) that is exactly
`(+15.0, +7.5)`, the measured value, and `recoilTimer` starts advancing on the same frame
(0.02 at f25196, 0.04 at f25197). No wall jump is involved anywhere in the window:
`wallLocked`, `wallLockSteps`, `wallJumpedL`, `wallJumpedR`, `touchingWallL`, `touchingWallR`
are all 0 in every FRAME from f25191 to f25197, and `cState.wallJumping` is never set —
consistent with `hero-motion` Q-hero-2 (0 wall-jump frames in this corpus). An earlier draft
called it "a wall-jump ramp already in flight"; that attribution is withdrawn.

The press is still correctly classified as no-effect, for a stronger reason than the timing
argument: `CanJump()` (`HeroController.cs:4717`) short-circuits on its first conjunct
`hero_state != ActorStates.no_input`, which is false throughout the recoil, so
`LookForQueueInput` cannot call `HeroJump()` at all. (The timing argument holds too — a
velocity present at F+1's `HC_FIXED_POST` cannot have been caused by the press at F, because
that FixedUpdate precedes the first `Update` that could have sampled the new key state.)
3 of the 6 dash presses likewise produced nothing (dash cooldown).

Worked example (r2_move.a records 1514–1543, corpus step 60 = `[0,2,7,1]` = hold left):

```
rec 1514  f24943  HC_UPDATE_PRE   vel=( 0.0000, 0.0000)
rec 1515  f24943  HC_UPDATE_POST  vel=( 0.0000, 0.0000)
rec 1516  f24943  EVENT STEP step=61 action=[0,2,7,1]      <- keys written here (coroutine)
rec 1517  f24944  FIXED           vel=( 0.0000, 0.0000)
rec 1519  f24944  HC_FIXED_POST   vel=( 0.0000, 0.0000)    <- FixedUpdate does NOT see it
rec 1526  f24944  FRAME  input=[left]  cst=[onGround]  vel=( 0.0000, 0.0000)
rec 1529  f24945  HC_FIXED_POST   vel=(-8.3000, 0.0000)    <- first effect
rec 1542  f24945  FRAME  input=[left]  vel=(-8.3000,0.0000)  anim=Turn#0
rec 1543  f24945  OBS  step=61
```

Jump (records 3025–3069, corpus 120 = `[1,2,7,0]`): `EVENT STEP` at f25123;
f25124 `FRAME input=[right,jump] cst=[onGround,jumping] vel=(-8.3, 0)`;
f25125 `HC_FIXED_POST vel=(+8.3, +16.65)`.
Dash (records 6446–6502, corpus 180 = `[1,2,4,1]`): `EVENT STEP` at f25303 (rec 6446);
f25304 `FRAME input=[right,dash] cst=[dashing,falling]  anim=Shadow Dash#0 vel=(-8.3,-1.896)`;
f25305 `HC_FIXED_POST vel=(+20.0, 0.0)`.

**Why exactly one FixedUpdate of delay.** `move_input` is latched only in
`HeroController.LookForInput()` — `move_input = inputHandler.inputActions.moveVector.Vector.x`
(`HeroController.cs:3262`), called from `orig_Update` (:5194) — and consumed only in
`HeroController.FixedUpdate` via `Move(move_input)` (:976 → :1249-1275). Likewise
`HeroJump()` (:3425-3435, sets `cState.jumping`, no velocity) and `HeroDash()` (:3511) are called
from `LookForQueueInput()` (:3330-3421), itself called from `orig_Update` (:5279); the
velocities they imply are applied by `FixedUpdate`. Because `HeroController.FixedUpdate` is
measured to precede `HeroController.Update` in every rendered frame of both traces
(§1.2/§1.3, 0 inversions), the FixedUpdate of frame *F+1* consumes
the `move_input`/`cState` latched by frame *F*'s Update — which ran **before** `ApplyAction`
(the coroutine resume is after all Updates, §1.4). Hence:

```
F   Update (pre-input)  ->  coroutine: keys written
F+1 FixedUpdate (stale) ->  Update: keys are visible by HeroController.Update at the latest
                            (exact sample slot Q-frame-3); move_input latched / HeroJump fires
F+2 FixedUpdate         ->  applies velocity
```

Net: **input applied in step *N* first moves the rigid body on the second live frame of step
*N*, and is therefore already reflected in step *N*'s own observation** (the obs is taken at
the end of frame F+2, §3.4). The state flags and the animation clip flip one frame earlier
(F+1) and are visible in the intermediate FRAME record.

**What is and is not established about the InControl sample.** The measurement bounds it to
the open interval *(ApplyAction in frame F's coroutine phase, `HeroController.Update` in frame
F+1)*: the lower bound because the key bits do not exist before `ApplyAction`
(`TrainingEnv.cs:598`), the upper because `cState.jumping` is already set in the FRAME of F+1
and only `LookForQueueInput` (from `orig_Update`) sets it. **Nothing narrows it further** — in
particular nothing excludes frame *F+1*'s FixedUpdate or physics sub-phase, since
`HeroController.FixedUpdate` reads no `WasPressed` edge that would have witnessed it (it
consumes `move_input`/`cState`, both Update-latched). So "InControl samples in the Update
phase" is **not** established; only the interval is. InControl is absent from
`analysis/decomp/`, so the callback that drives `InputDeviceShim.Update`
(`oracle/Game/ProxyController.cs:96`) is unverified. **UNKNOWN — Q-frame-3.**

The delay is nonetheless *one FixedUpdate* regardless of where in that interval the sample
lands, because both consumers (`Move(move_input)` and the `cState.jumping`/`dashing` branches)
read state that only `HeroController.Update` writes, and `HeroController.Update` at F+1 runs
after `HeroController.FixedUpdate` at F+1 (§1.2, 0 inversions).

---

## 5. Recorder blind spots, and the capture points that would close them

1. **No Unity-phase boundary markers.** The `phase` byte is "last phase seen by
   `RecorderBehaviour`" (`TraceRecorder.cs:279,285,286,713`), not the true phase; a record
   emitted before `RecorderBehaviour.Update` in the Update phase is indistinguishable from one
   emitted in the physics-callback sub-phase (this is exactly the 778/1540 `phase==1` records
   in §1.4). **Add**: a `PHASE` record (`u8 which`, `u32 frame`) from a MonoBehaviour with a
   pinned extreme script execution order (`[DefaultExecutionOrder(-32000)]` and a second at
   `+32000`) emitting `FIXED_BEGIN/FIXED_END/UPDATE_BEGIN/UPDATE_END/LATE_BEGIN/LATE_END`.
   That converts every ordering claim here from "observed relative to HeroController" to
   "observed relative to the phase".
2. **No LateUpdate sample of any kind.** `HC_LATE_*` never fires (HeroController has no
   `LateUpdate`), and `FRAME` is emitted before LateUpdate. Consequences: the animator
   state, the camera, and every `PlayMakerLateUpdate` FSM are sampled one frame stale; the
   sim cannot be diffed at end-of-frame. **Add**: a `FRAME_LATE` record (hero pose + ANIM +
   camera transform) from `RecorderBehaviour.LateUpdate`, and a `CAMERA` block
   (`CameraController.transform.position`, `mode`) — currently the camera is not recorded at all,
   although `CameraController.LateUpdate` reads `hero_ctrl.cState` and can move the hero's
   visual frame.
3. **Frozen frames record almost nothing.** No `FRAME` (so no `cstate`, no hero/PlayerData
   fields, no `Time.*`, no `rng`, no entity block, no input bitset), only `HC_UPDATE_PRE/POST`
   (pose only). `Time.timeScale` on a frozen frame is never recorded. So "what advances during
   the pause" is inferred, not measured. **Add**: emit a `FRAME` (or a reduced `FRAME_LITE`)
   from `RecorderBehaviour.Update` on *every* rendered frame, not only from the mod coroutine.
4. **Per-instance `PlayMakerFSM.Update` order is invisible.** Only `Fsm.Event`/`Fsm.SwitchState`
   are hooked, so an FSM that ticks without raising an event or transition leaves no trace, and
   an FSM's *position* in the Update list is only inferable when it happens to fire.
   **Add**: PRE/POST records around `PlayMakerFSM.Update`, `PlayMakerFixedUpdate.FixedUpdate`,
   `PlayMakerLateUpdate.LateUpdate` carrying `(instanceID, fsmName, activeState)` — gate them
   behind an env var, the volume is large.
5. **`Fsm.Event(FsmEventTarget, FsmEvent)` (`Fsm.cs:2126`) called directly is not recorded** —
   only the `Fsm.Event(FsmEvent)` funnel (:2192) is hooked (`TraceRecorder.cs:222-245`), as the
   hook comment states. `ProcessEvent` (:2023) reached any other way, and every `DelayedEvent`
   drained in `Fsm.UpdateDelayedEvents` (:1924), are invisible. **Add**: hook `Fsm.ProcessEvent`
   instead of / in addition to `Fsm.Event`.
6. **No FSM-action-level record.** `FsmState.OnEnter/OnUpdate/OnFixedUpdate/OnLateUpdate`
   (`FsmState.cs:267,331,342,357`) are the unit P4 must reimplement; nothing in the trace says which
   action ran when. **Add**: an `ACTION` record (fsm instanceID, state, action index, type name)
   behind an env var.
7. **`Time.timeScale`, `Time.captureDeltaTime`, `Time.fixedDeltaTime` are recorded only on
   `FRAME`** — i.e. only on live frames, only at the coroutine phase. A mid-frame write (e.g.
   `TimeController.GenericTimeScale`, `TimeController.cs:79`) is invisible. **Add**: a hook on
   the `Time.timeScale` setter, or record it in the proposed `PHASE` records.
8. **`Physics2D` step boundaries and contact events are not recorded.** Collision/trigger
   callbacks are only visible when they happen to raise an FSM event.
   **Add**: a `CONTACT` record from `HeroController.OnCollisionEnter2D/Stay2D/Exit2D` hooks
   (`HeroController.cs:4906,4961`) with the other collider's name and layer.
9. **`hero.f.*` and `pd.*` are sampled once per FRAME**, so any field mutated in FixedUpdate and
   again in Update within a frame shows only the post-Update value. `HC_FIXED_*`/`HC_UPDATE_*`
   carry pose only (`TraceRecorder.cs:684-701`). **Add**: `cstate` (u64) + a small selected-field
   set to the `Pose` payload — cheap and it makes the hero solver diffable per-callback.

---

## 6. Open questions

### Q-frame-1 — Are the `phase == 1` records between `HC_FIXED_POST` and `HC_UPDATE_PRE` the physics-callback sub-phase, or early Update?

778 (r2_move) / 1540 (r2_rand1) FSM records sit after `HeroController.FixedUpdate` and before
`HeroController.Update` carrying `phase == 1`, i.e. emitted after `RecorderBehaviour.FixedUpdate`
(`oracle/Oracle/TraceRecorder.cs:279`) and before `RecorderBehaviour.Update` (`:285`). Their owners
mix hero collider FSMs (`Knight` 421/468, `DownSlash` 72, `Slash` 24, `Hit R` 104, `Hit L` 62 —
consistent with `HeroController.OnCollisionStay2D` `analysis/decomp/Assembly-CSharp/HeroController.cs:4906`
and `OnCollisionExit2D` :4961, which send `HeroCtrl-LeftGround` at :4951/:4993) with plain boss/scene
FSMs that own no collider callback (`Evade Check` 62/418, `Floor Check` 0/164, `Hornet Boss 1` 8/0) —
and those same FSMs dominate the `phase == 0` half of the same slot. The trace therefore cannot
separate "late in the FixedUpdate/physics block" from "early in the Update block, before
`RecorderBehaviour.Update`", and the sim's decision of whether trigger/collision callbacks run inside
the fixed step depends on the answer. Settled by blind spot #1 (`PHASE` records from
`[DefaultExecutionOrder(±32000)]` behaviours bracketing each callback list).

### Q-frame-2 — Does the mod coroutine's `FRAME` sample really precede *all* LateUpdates?

0 records carrying `phase == 2` precede a `FRAME` in the same frame, in both traces — but
`phase == 2` is only stamped by `RecorderBehaviour.LateUpdate` (`TraceRecorder.cs:286`), so a
`LateUpdate()` dispatched earlier in the LateUpdate list than `RecorderBehaviour`'s would emit
records still carrying the stale `phase == 0` and be indistinguishable from Update-phase records.
The 130 (move) / 154 (rand1) post-`FRAME` records with `phase == 0` (owners `Hornet Boss 1` 74/53,
`CameraParent` 5/6, `Shadow Recharge` 8/0) are exactly that ambiguous population. This decides
whether the observation the trainer receives is taken before or after some of the frame's LateUpdate
work — notably `CameraController.LateUpdate` and `PlayMakerLateUpdate`. Settled by blind spots #1
and #2.

### Q-frame-3 — Which Unity callback samples the InControl device, and where in the frame?

`InputDeviceShim.Update(updateTick, deltaTime)` (`oracle/Game/ProxyController.cs:96-113`) is driven by
InControl's own manager, which is **not** in `analysis/decomp/` (`Assembly-CSharp-firstpass/` holds only
Steamworks and UnityStandardAssets). §4 bounds the sample to the open interval *(ApplyAction in frame
F's coroutine phase, `HeroController.Update` in frame F+1)* and nothing narrows it further — in
particular nothing excludes frame F+1's FixedUpdate or physics sub-phase, because no HeroController
FixedUpdate code path reads a `WasPressed` edge that would have witnessed an earlier sample. The
measured one-FixedUpdate latency (§4) holds for every position in that interval, so this does not
block P3; it blocks any sim that wants to place the sample precisely (e.g. to model an input arriving
mid-frame). Settled by blind spot #1 plus a PRE/POST hook on `InputDeviceShim.Update`.

### Q-frame-4 — When does a mid-frame `Time.timeScale` write change `Time.deltaTime`?

`TrainingEnv.Step` writes `Time.timeScale = 0` at `oracle/Environment/TrainingEnv.cs:627` and `= 1f`
at `:596`, both in the coroutine phase, i.e. after that frame's Update block. No record samples
`deltaTime` after either write: `FRAME.dt` is read at `RaiseFrame` (`:617`), which precedes `:627`,
and frozen frames emit no `FRAME` at all. Consequently two hypotheses fit every measurement equally —
(a) `deltaTime` is fixed for the whole frame, so the last live frame's LateUpdate still runs at 0.02
and the frozen frame's at 0; or (b) the write takes effect immediately, so the last live frame's
LateUpdate runs at 0 and the frozen frame's (after the `= 1f` write) at 0.02. Both predict the
measured Δ`Time.time` = 0.02 across the frozen frame and the measured tk2d advance of
`frames_per_wait × dt` per agent step. An earlier draft asserted (a) as a corollary of "frame F still
reported dt = 0.02"; that was circular and is withdrawn. The per-step totals the sim must reproduce
are unaffected; the per-frame attribution of animation and camera advance is not. Settled by blind
spot #3 (a `FRAME_LITE` from `RecorderBehaviour.Update` on every frame) plus a `dt` field on the
`FRAME_LATE` record of blind spot #2.

### Q-frame-5 — What raises the FSM cascade that escapes `FsmPauseGate` on frozen frames?

`FsmPauseGate` gates `PlayMakerFSM.Update`, `PlayMakerLateUpdate.LateUpdate` and
`PlayMakerFixedUpdate.FixedUpdate` on `Time.timeScale > 0` (`oracle/Game/FsmPauseGate.cs:33-59`), yet
3 (move) / 10 (rand1) FSM records land on frozen frames from non-`Knight` owners, including a real boss
state change (`Hornet Boss 1/Control  Thrown→Throw Recover`, r2_rand1.a rec 4651, frame 25061).
Candidate un-gated paths: `PlayMakerFSM.OnDisable` → `fsm.Event(FsmEvent.Disable)`
(`analysis/decomp/PlayMaker/PlayMakerFSM.cs:392-396`, event name "DISABLE" per
`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmEvent.cs:443`), `PlayMakerFSM.OnEnable` (:363-367),
and Unity coroutines started through `PlayMakerFSM.DoCoroutine` (:377-388), which keep resuming at
`timeScale = 0`. Which one raised the initiating `Needle/Control FINISHED` is not in the trace. This
decides whether the sim must model the frozen frame as an FSM-active frame at all. Settled by blind
spot #4 plus a hook on `Fsm.ProcessEvent` recording the calling phase (blind spots #1, #5).

### Q-frame-6 — Which `PlayMakerFSM` instances tick after `HeroController.Update`, and is that set stable across runs?

46 (move) / 137 (rand1) FSM records fall between `HC_UPDATE_POST` and `FRAME`, and a further 130 / 154
after `FRAME`; the owners are dominated by objects instantiated during the episode
(`Run Effects(Clone)`, `SD Crystal(Clone)`, `G Dash Effect`, `Flash Effect`, `Throw Effect`) plus
`Hornet Boss 1`. So `PlayMakerFSM.Update` is not one contiguous block relative to
`HeroController.Update`. If the split is instantiation-order dependent it is a divergence risk for P4,
because a boss FSM reading hero state would see it pre- or post-`HeroController.Update` depending on
when its GameObject was created. Settled by blind spot #4 (PRE/POST records around
`PlayMakerFSM.Update` carrying the instance id).

### Q-frame-7 — Is `Hornet Boss 1/Control` driven by `PlayMakerFSM.Update`, by its `PlayMakerFixedUpdate` proxy, or both, per frame?

`analysis/fsm/GG_Hornet_1.json` gives `Hornet Boss 1/Control` `handleFixedUpdate: true` (and
`preprocessed: false` at dump time, so the flag's provenance wants re-checking), which per
`analysis/decomp/PlayMaker/PlayMakerFSM.cs:286-289` adds a `PlayMakerFixedUpdate` proxy in addition to
the unconditional `PlayMakerFSM.Update` (`:369-374`). Its records appear in three different slots
(`phase==0` pre-Update 52/48, `phase==1` 8/0, post-`FRAME` 74/53). The sim's boss tick rate — one
`Fsm.Update` plus one `Fsm.FixedUpdate` per frame under R2, or something else — depends on this.
Settled by blind spot #4.

### Q-frame-8 — What sets the absolute `Time.frameCount` phase at scene load?

`orig_Update` runs `Update10()` only when `Time.frameCount % 10 == 0`
(`analysis/decomp/Assembly-CSharp/HeroController.cs:5108`); `Update10` (:1212) clamps `transform` scale
and z and runs `OutOfBoundsCheck`. Behaviour is therefore coupled to the *absolute* frame counter, not
to frame deltas, so a sim that starts its counter at 0 will run `Update10` on a different set of frames
than the trace. The traces begin at `Time.frameCount` 24764 (move) / 24705 (rand1) — boot- and
scene-load-dependent values that nothing in `analysis/` explains or pins. Settled by recording the frame
counter at `SceneReady` (already in `EVENT SCENE_READY`'s `frame` field) **and** deciding a
normalisation rule for the sim, which is an orchestrator contract question.

### Q-frame-9 — Which FSM actions read wall-clock time?

§3.1 measures `Time.unscaledDeltaTime` as wall clock even under `captureDeltaTime`
(0.000347…0.011139 s over the first six frames, against a pinned `dt` of 0.02).
`FsmTime.RealtimeSinceStartup` (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmTime.cs:16-30`)
exposes `Time.realtimeSinceStartup` to any action. A census of which actions in `analysis/fsm/*.json`
read either — the `ShakePositionV2` class of bug that R2 had to pin (`docs/frame-order.md`
§Regime R2) — has not been done, and any un-pinned reader makes the RNG stream wall-clock dependent,
which the P4 seeded-RNG gate cannot tolerate. Not settled by any recorder change; needs a dump-side
sweep.

Scope of the shake surface, recounted from `analysis/fsm/GG_Hornet_1.json` (an action's `type`
is fully qualified there, e.g. `HutongGames.PlayMaker.Actions.SendEventByName`): **69**
`SendEvent*` action instances carry a `*Shake*` event string, of which **68 have
`enabled: true`**. Quote **69 instances / 68 enabled**, and say which — the single difference
is `Hornet Boss 1 / Control / Throw` action index 3 (`EnemyKillShake`), `enabled: false`.
Events sent: `EnemyKillShake` 18, `AverageShake` 16, `AverageShake;CameraShake` 19,
`CameraShake;EnemyKillShake` 8, `BigShake` 5, `BigShake;CameraShake` 2, `SuperDashShake` 1;
19 distinct owner GameObjects. Separately, **15** `ShakePositionV2` action instances exist in
the same dump — those are the actual `Random.Range` draw sites.

### Q-frame-10 — Is any `HeroController` field written in both `FixedUpdate` and `Update` of the same frame?

`FRAME` samples `hero.f.*` and `pd.*` once per frame, at the coroutine phase; `HC_FIXED_*` and
`HC_UPDATE_*` carry pose only (`oracle/Oracle/TraceRecorder.cs:684-701`). A field mutated in both
callbacks of one frame is therefore observed only in its post-Update state, and the sim cannot tell
whether the two blocks may be fused into a single per-frame step or must stay ordered. Settled by
blind spot #9 (adding `cstate` and a selected-field set to the `Pose` payload).

---

## Review fixes

Applied against `analysis/specs/REVIEW-p1.md` (2026-08-31). Every count below was re-measured from
the traces before editing.

| id | class | change |
|---|---|---|
| D30 | MISREAD/UNCITED | §1.4 `phase == 1` bullet rewritten: full owner census printed for both traces (r2_move Knight 421, DownSlash 72, **Evade Check 62**…; r2_rand1 Knight 468, **Evade Check 418**, **Floor Check 164**…), the "hero attack/range colliders" reading demoted to a hypothesis contradicted by the boss/scene FSMs in the same slot, and the "physics-callback sub-phase … after the FixedUpdate list and before the Update list" engine claim replaced by the bound the byte actually gives. Raised as **Q-frame-1**. |
| D31 | UNCITED (over-claim) | §1.4 `phase == 2` bullet: "`FRAME` precedes all LateUpdate-phase FSM activity" → "precedes every LateUpdate that runs at or after `RecorderBehaviour.LateUpdate` (`TraceRecorder.cs:286`)", with the post-`FRAME` `phase == 0` population (130/154) named as the ambiguous remainder. Raised as **Q-frame-2**. |
| D32 | UNCITED (over-claim) | §2 callback-map row, §3.4 loop comment and §4's closing paragraph all rewritten: the InControl sample is stated only as the open interval *(ApplyAction at F, `HeroController.Update` at F+1)*, explicitly not excluding F+1's FixedUpdate/physics sub-phase; the row is marked provisional. Added the reason the one-FixedUpdate latency holds regardless. Raised as **Q-frame-3** (was §6 Q2). |
| D33 | UNSUPPORTED (circular) | §3.2 first bullet: the corollary "a `Time.timeScale` write takes effect at the next frame's `deltaTime`" is **withdrawn** — `FRAME.dt` is sampled at `RaiseFrame` (`TrainingEnv.cs:617`) before the `:627` write, so it cannot witness a same-frame effect. Retained only `dt(frozen) = 0`, re-derived from Δ`Time.time` on the accumulation identity, with the identity's own empirical support (599/599, 479/479) stated. Knock-on: the tk2d bullet no longer attributes the advance to a particular frame's LateUpdate — both hypotheses fit — and §3.4's loop comments now flag it. Raised as **Q-frame-4** (absorbs the old §6 Q3). |
| D34 | MISREAD (count) | §3.2: frozen frames carrying the `HeroCtrl-LeftGround` cascade **68 → 71** (move; 35 rand1 unchanged), with the reason the old number was wrong (68 counted one collapsed signature, not frames containing the event). "every frozen frame spent airborne" replaced by the actual guard `rb2d.velocity.y <= -1E-06f` + `!CheckTouchingGround()` (`HeroController.cs:3855`), which excludes the rising half of a jump. |
| D35 | UNSUPPORTED/MISREAD (counts) | §3.3: per-step signature counts **5/6 → 3/3** with FSM records stripped, plus 24/28 collapsed and 78/89 verbatim, and the segment rule now stated (`[STEP_i, STEP_{i+1})`, frame-relative tokens, adjacent duplicates collapsed, final truncated step excluded → 299/239 segments). §4: walk/turn transitions **29 → 32** (32/32 at F+2, all 32 also move `rb_pos_x`) with the selection filter widened to match the reviewer's (no filter on `action[2]`/`[3]`); jump presses now itemised as 5 executed / 2 queued at F+5 / **8 with no attributable effect** (was 7), the 8th being corpus 144, whose F+1 velocity the F press cannot have caused (mechanism corrected in R2-2 below: it is damage recoil). Each row now carries its selection and probe. |
| D36 | WRONG-LINE | Method note now cites `TraceRecorder.cs:414` (OBS `frame`) alongside `:433/:691/:711`; §2 coroutine row obs-build range **:748-756 → :748-759** (`GetSplitFeatures` :748, `GetGlobalState` :749-750, `SnapshotFsms` :759). |
| D37 | OUT-OF-REPO | §1.1: the `%USERPROFILE%/…/HKOracle_oracle_g1.log` citation is removed. The claim now rests on the decomp grep plus `TraceRecorder.cs:138` taking the `m == null` branch at `:253`. |
| D38 | MISREAD (counts) | §2 FixedUpdate row: `HandleFixedUpdate = true` classes stated as **50 under `HutongGames.PlayMaker.Actions/` + `CheckTrackTriggerCount.cs:42` = 51**, reconciling C3 with `fsm-runtime` L323 (different scopes); `Knight/*` FSMs with the flag **7 → 8** (the 8 names were already listed); the 79 relabelled as *instances* with the distinct-pair count (56) and per-scene instances/pairs/`Knight/*` given. |
| D39 | OMISSION | §1.4: the `phase` byte's semantics completed — it is a latch written at `TraceRecorder.cs:256`, `:259` (HC hooks), `:279`, `:285`, `:286`, `:357`, `:429`, initial value 3 (`:35`) — and explicitly described as "which writer ran most recently", not a phase probe. |
| C2 | contradiction | §1.2: added the row "of those, carrying an `HC_UPDATE_PRE/POST` pair — **302** / **242**" and named the `SCENE_READY` frame as the single exception, so 303 (frames with no `FIXED`) and 302 (frames where `HeroController.Update` ran without a FixedUpdate) can no longer be confused. |
| C3 | contradiction | Resolved in D38: 51 = 50 + 1, both scopes named. |
| — | (unflagged, found while fixing D35) | §4's "Because under R2 the FixedUpdate block precedes the Update block within a rendered frame" now cites the measurement (§1.2/§1.3, 0 inversions) instead of asserting the engine rule. |
| R2-2 | MISREAD (introduced by D35) | §4: the corpus-144 no-effect jump press is re-attributed from "a wall-jump ramp already in flight" to **damage recoil**. Evidence added: `EVENT HERO_DAMAGE` (`Hornet Boss 1`, amount 1, hazard 1) at f25193 (rec 4468) / f25194 (rec 4536); FRAME f25194 (rec 4549) `cState.recoiling=1`, `invulnerable=1`, `hero_state=7` = `ActorStates.no_input`; `(+15.0,+7.5)` = `recoilVector` from `StartRecoil` `HeroController.cs:3782`/`:3794` with `RECOIL_VELOCITY = 15.0` (`analysis/dumps/GG_Hornet_1/hero.json`), assigned at `:955` in `FixedUpdate`; `wallLocked/wallLockSteps/wallJumpedL/wallJumpedR/touchingWallL/touchingWallR` all 0 across f25191-f25197 (so no contradiction with `hero-motion` Q-hero-2). The no-effect conclusion stands and is now given the stronger reason: `CanJump()` (`:4717`) fails on its first conjunct `hero_state != no_input`. |
| R2-2 nit | count scope | Q-frame-9: shake-send sites recounted — **69** `SendEvent*` instances in `analysis/fsm/GG_Hornet_1.json` carry a `*Shake*` event string, **68** of them `enabled: true`; the one difference is named (`Hornet Boss 1 / Control / Throw` idx 3, `EnemyKillShake`, `enabled: false`). Both numbers plus the per-event breakdown and the 15 `ShakePositionV2` draw sites are now stated, so "68" and "69" can no longer be quoted without a scope. (Neither number appeared in this spec before; they are added to Q-frame-9, where the RNG census belongs.) |

Open questions renumbered to `Q-frame-1..10`: the four new/downgraded ones (Q-frame-1..4) come from
D30–D33; the previous §6 Q1, Q4–Q8 became Q-frame-5, Q-frame-6–Q-frame-10; the previous Q2 and Q3
were absorbed into Q-frame-3 and Q-frame-4.

Not changed: §3.4 (the step loop — 1 frozen frame + `frames_per_wait` live frames per agent step),
confirmed by the review; its LateUpdate comments now carry the Q-frame-4 caveat but the loop is
unaltered. §5's blind-spot list is unchanged and is referenced by every Q above.

---

## 8. Where the FRAME record sits relative to the update-coroutine phase (P1.2 UNKNOWN — CLOSED)

`docs/frame-order.md` has carried this as **UNKNOWN — measured, not assumed** since P1.2. It is
closed here by a direct read of the record stream, which is the evidence the header names (every
record kind is emitted from a hook at its own point, so the order of records within one frame IS the
answer).

Source: `analysis/polbat_GG_Hornet_2/h2_ep02.a.hktrace`, **game** side, live frame 26157, records in
stream order, FSM_EVENT suppressed:

```
FIXED
HC_FIXED_PRE
HC_FIXED_POST
EVENT ph=0  FSM_TRANSITION  Evade Check/FSM        Detect -> Exit
EVENT ph=0  FSM_TRANSITION  Evade Check/FSM        Exit -> Detect
HC_UPDATE_PRE
HC_UPDATE_POST
FRAME                      (Hornet Boss 2 anim.clip = "Sphere Antic A Q")
EVENT ph=0  FSM_TRANSITION  CameraParent/CameraShake  Normal -> To Kill Shake
EVENT ph=0  FSM_TRANSITION  CameraParent/CameraShake  To Kill Shake -> ShakingKill
EVENT ph=0  FSM_TRANSITION  Hornet Boss 2/Control     Sphere Antic A -> Sphere A
EVENT ph=2  FSM_TRANSITION  Sphere Ball/Grow          Grow -> Grow
EVENT ph=2  FSM_TRANSITION  Flash Effect/FSM          Init -> Init
...
```

and the following live frame 26158 ends `... HC_UPDATE_POST  FRAME (clip="Sphere Attack")  OBS`.

### Two things this establishes

1. **`FRAME` PRECEDES the trailing update-coroutine (`ph=0`) event block.** So a `FRAME` record that
   shows the OLD animation clip on the same frame as a `ph=0` transition which changes that clip is
   **consistent**, not a contradiction: frame 26157's FRAME reads `Sphere Antic A Q`, the
   `Sphere Antic A -> Sphere A` transition is emitted after it, and the new clip first appears in
   26158's FRAME. Anyone reconciling a FRAME snapshot against same-frame events must read FRAME as a
   state taken BEFORE that frame's trailing coroutine block, not after it.

2. **`ph` is a label, not a contiguous region of the frame.** `ph=0` events occur in *two* separate
   blocks within one frame — one between `HC_FIXED_POST` and `HC_UPDATE_PRE`, and one after `FRAME`.
   Ordering conclusions must therefore be drawn from **stream position**, not from the `ph` value.
   (`docs/trace-format.md:105`: `phase (0 update-coroutine, 1 fixed, 2 late, 3 other/hook)`.)

### Why it was being asked

The simulator dispatches a tk2d animation-completion synchronously inside phase 2, so a transition it
raises lands at `ph=2`; the game's equivalent lands at `ph=0` of the same frame. Because an object
activated in phase 2 has already missed that frame's earlier phases, its FSM first ticks the FOLLOWING
frame — which is the one frame `Sphere Ball`'s `iTweenScaleTo` growth curve runs behind on
GG_Hornet_2. See `findings/boss-GG_Hornet_2.md` for that chain. The ordering above removes the
apparent contradiction in that evidence but does NOT by itself say which side's dispatch is right.

Recorded by d2, 2026-09-02.

---

## FRAME's position within a live frame — CLOSED 2026-09-02

`docs/frame-order.md` carried this as **UNKNOWN** since P1.2. It is now measured, by a direct read
of the record stream rather than by inference.

Source: `analysis/polbat_GG_Hornet_2/h2_ep02.a.hktrace`, **game** side, live frame **26157**, in
stream order:

    FIXED
    HC_FIXED_PRE
    HC_FIXED_POST
    [ph=0 EVENTs]
    HC_UPDATE_PRE
    HC_UPDATE_POST
    FRAME
    [ph=0 EVENTs]
    [ph=2 EVENTs]
    ... and on the next obs frame: FRAME then OBS

**So `FRAME` PRECEDES the trailing update-coroutine EVENT block.**

### What this resolves

A `FRAME` record showing the **old** clip alongside a `ph=0` transition on the **same** frame is
therefore **consistent**, and there is no contradiction to resolve. Frame 26157's `FRAME` reads
`clip = 'Sphere Antic A Q'`, the `Control` transition to `Sphere A` is emitted *after* it, and the
new clip appears in 26158's `FRAME`.

This was the blocker on diagnosing the frame-phase defect (439 of 6732 FSM transitions firing in a
different phase game-vs-sim). Without it, a dispatch fix could as easily have landed the transition
a frame *later* as earlier.

### A second fact worth stating explicitly, because it is easy to assume otherwise

**`ph` is a label, not a contiguous region.** `ph=0` EVENTs occur in **two separate blocks** within
one frame — one between `HC_FIXED_POST` and `HC_UPDATE_PRE`, and one after `FRAME`. Any analysis
that treats a phase as a single span of the stream, or that infers ordering between two records from
their phase labels alone, is unsound.

Measured by the `d2` agent during the 2026-09-02 marathon; committed from `integration` because
tracked files under `analysis/` cannot be published from a worker branch (the directory is a
junction to the integrator's worktree).
