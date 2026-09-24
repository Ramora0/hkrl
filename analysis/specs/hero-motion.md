# Hero motion — P1 spec (HeroController, regime R2)

Owner constraint: every rule below is a **port of the decompiled code path**, with constants taken
from the runtime dump. The r2 traces are used only to *verify* a ported rule reproduces the recorded
numbers, never to fit one. Anything the code does not settle is UNKNOWN + a Q entry in §6.

## Citation keys

| key | file |
|---|---|
| `HC:<n>` | `analysis/decomp/Assembly-CSharp/HeroController.cs:<n>` |
| `HCS:<n>` | `analysis/decomp/Assembly-CSharp/HeroControllerStates.cs:<n>` |
| `IH:<n>` | `analysis/decomp/Assembly-CSharp/InputHandler.cs:<n>` |
| `HA:<n>` | `analysis/decomp/Assembly-CSharp/HeroActions.cs:<n>` |
| `OAIC:<n>` | `analysis/decomp/Assembly-CSharp/InControl/OneAxisInputControl.cs:<n>` |
| `ICM:<n>` | `analysis/decomp/Assembly-CSharp/InControl/InControlManager.cs:<n>` |
| `scene.json#…` | `analysis/dumps/GG_Hornet_1/scene.json` → `colliders[…]` |
| `AS:<n>` | `analysis/decomp/Assembly-CSharp/GlobalEnums/ActorStates.cs:<n>` |
| `PL:<n>` | `analysis/decomp/Assembly-CSharp/GlobalEnums/PhysLayers.cs:<n>` |
| `PC:<n>` | `oracle/Game/ProxyController.cs:<n>` |
| `TE:<n>` | `oracle/Environment/TrainingEnv.cs:<n>` |
| `hero.json#X` | `analysis/dumps/GG_Hornet_1/hero.json` → `heroController[name=X].value` |
| `phys.json#P` | `analysis/dumps/GG_Hornet_1/physics.json` → JSON path `P` |
| `pd.json#X` | `analysis/dumps/GG_Hornet_1/playerdata.json` → `fields[name=X].value` |
| `save#X` | `oracle/Resource/save_file.json` → `playerData.X` |
| trace | `analysis/traces/p0/r2_{idle,move,rand1,rand2}.a.hktrace` (R2, 1800 FRAME records total) |

**324 of 347** HeroController fields are inspector-serialized with **no initializer** in the decomp.
The 23 that do have one: `HC:183-219` (19 fields), `HC:233`, `HC:713`, `HC:723`, and the `const` at
`HC:719`. Every other constant below cites the dump, which is the only source. [D27]

Dump provenance: `analysis/dumps/GG_Hornet_1/meta.json` — `timestampUtc 2026-08-31T02:49:29Z`,
`frameCountAtDump 29672`, `timeScaleAtDump 0.0`; taken at `SceneReady` under regime R2
(`phys.json#Time.captureDeltaTime = 0.02 = phys.json#Time.fixedDeltaTime`). Fields that are only
assigned while an action is in flight therefore read their idle value in the dump — see
`attackDuration` in §3.3. [C4]

Engine-behaviour convention: Unity/Box2D/InControl semantics that no decomp line, dump key or trace
measurement establishes are marked **[engine-assumption → Q-hero-10]** rather than asserted
(`analysis/open-questions.md` Q16: Physics2D is native, has no decomp, and must not be assumed from
memory). [D28]

---

## 1. Motion model (PLAN §5 P1 Q1)

### 1.1 What the code does

**HeroController is purely velocity-driven.** In 5549 lines there are:

| pattern | count | citation |
|---|---|---|
| `rb2d.velocity = …` | 69 | grep over `HeroController.cs` |
| `AddForce` / `AddRelativeForce` | **0** | grep |
| `rb2d.MovePosition` / `rb2d.position = …` | **0** | grep |
| `transform.position` writes | 15, none in a locomotion path — scene entry/respawn `HC:2377,2435,2463,2476,2518,2573,2714,2807`, z-clamp `HC:892,1229`, off-screen park `HC:853,888`, death-effect spawn `HC:3765,3772`, `TileMapTest` `HC:4326,4330,4336` | grep |

`TileMapTest` (`HC:4308-4342`, the only per-frame transform teleport) **has no call site in this
build** (grep: the identifier appears only at its declaration `HC:4308`) — treat as dead code.

Every motion state is one velocity assignment per FixedUpdate:

| state | assignment | citation |
|---|---|---|
| ground/air run | `v = (move_input·RUN_SPEED, v.y)` | `HC:1275` (branches `HC:1259,1263,1267,1271`) |
| jump hold | `v = (v.x, JUMP_SPEED)` | `HC:1290` |
| double jump (after step 3) | `v = (v.x, JUMP_SPEED·1.1)` | `HC:1308` |
| jump release | `v = (v.x, 0)` | `HC:4212,4218` (in **Update**) |
| dash | `v = OrigDashVector()` | `HC:1517-1519`, vector `HC:5428-5433` |
| damage recoil | `v = recoilVector` | `HC:955`, vector `HC:3794,3801` |
| horizontal recoil | `v.x ∓= RECOIL_HOR_VELOCITY[_LONG]` | `HC:995,999,1007,1011` |
| wall jump lock | `v = (±currentWalljumpSpeed, v.y)` | `HC:1058,1062` |
| wall slide | `v.y` ramped toward `WALLSLIDE_SPEED` by `WALLSLIDE_DECEL` | `HC:1124-1142` |
| cast / cast-recoil | `v = 0` / `v = (∓CAST_RECOIL_VELOCITY, 0)` | `HC:1046` / `HC:1037,1041` |
| bounce | `v = (v.x, BOUNCE_VELOCITY)` | `HC:1051` |
| bump (running/dashing into a step) | `v.y = BUMP_VELOCITY` / `BUMP_VELOCITY_DASH` | `HC:966,971` / `HC:5453,5462` |
| fall clamp | `if v.y < -MAX_FALL_VELOCITY: v.y = -MAX_FALL_VELOCITY` | `HC:1104-1107` |
| dead | `v = (0,0)` | `HC:925` |
| super dash | **not in HeroController** — `SuperDash()` is an empty body | `HC:1531-1533` |

`AffectedByGravity(bool)` (`HC:3241-3254`) saves `rb2d.gravityScale` into `prevGravityScale` and sets
it to 0, or restores it. Called with `false` for dash (`HC:1510`), damage recoil (`HC:954,3790`),
scene exit (`HC:937`); with `true` on dash end (`HC:4034,4233`), recoil end (`HC:4100,5198`), hard
landing (`HC:3496`), dash landing (`HC:4894`).
It is **not** the only writer of `rb2d.gravityScale`: `EnterAcid` (`HC:4289`, `= UNDERWATER_GRAVITY`)
and `ExitAcid` (`HC:4297`, `= DEFAULT_GRAVITY`) assign it directly. Both are `[Obsolete]`
(`HC:4286`, `HC:4294`) and have no call site in `HeroController.cs` (grep), so under the boss-arena
regime `AffectedByGravity` is the only *reachable* path — which is what the sim ports. [D26]

### 1.2 What Box2D does to that velocity — measured

Experiment: `HC_FIXED_POST[frame N]` (the velocity HeroController just assigned, pre-solve) vs
`HC_UPDATE_PRE[frame N]` (post-solve, before HeroController.Update runs). The physics step is the
only thing between them (§2). 1707 frame pairs across the four r2 traces.

The integrator is **not** read from any source (Physics2D is native, no decomp — `open-questions.md`
Q16). It is a **hypothesis fitted to the trace and then tested**, stated here as the measurement it
is [D28]:

> H1 (semi-implicit / symplectic Euler): `v.y += gravityScale · Physics2D.gravity.y · dt`, **then**
> `p += v_new · dt`
> H2 (explicit Euler): `p += v_old · dt`, then `v.y += …`

with `Physics2D.gravity.y = -60` (`phys.json#Physics2D.gravity.y`), `dt = 0.02`
(`phys.json#Time.fixedDeltaTime`), `gravityScale = 0.79` (`phys.json#rb2d.gravityScale`).
⇒ per step `Δv.y = -0.948`; H1 and H2 differ in position by `g·dt² = -0.018960`, which is 5 orders of
magnitude above the f32 noise floor at these coordinates, so the trace discriminates them cleanly.
**H1 wins on 404/404 airborne frames; H2 is off by exactly −0.018960 on every one of them.**

| bucket | n | result |
|---|---|---|
| airborne, no wall contact | 404 | prediction exact to **float rounding** (max residual: `Δv.y` 8.8e-7, `Δp` 9.6e-7) |
| `onGround`, resting | 1289 | `Δv.y = 0` exactly (contact cancels the whole gravity increment, residual `+0.948`); `Δp.y = 0` exactly; `Δv.x = 0` in 98% |
| `onGround`, free (no real contact that step) | 79 | prediction exact |
| any, touching wall | 21 | normal component zeroed |
| air, contact | 7 | landing/ceiling frames |

`Δv.x = 0.000000` in **100%** of the 404 airborne frames — there is no drag, no air friction, and the
solver never touches the horizontal velocity in free flight (`phys.json#rb2d.drag = 0`;
the body collider's material is `FrictionlessSurface`, `phys.json#heroColliders[0].sharedMaterial.name`).

**Landing** (13 frames with `v.y ≤ -12` hitting ground): in a *single* fixed step the body is placed
at the resting contact position and `v.y` set to 0 — never overshoot, never a multi-step settle:

| trace | frame | v.y pre | v.y post | p.y pre | p.y post | free-flight prediction |
|---|---|---|---|---|---|---|
| r2_move | 25184 | −16.116 | 0 | 28.43271 | **28.40809** | 28.09143 |
| r2_move | 25349 | −15.168 | 0 | 28.69803 | **28.40809** | 28.37571 |
| r2_rand1 | 24787 | −20.000 | 0 | 28.59474 | **28.40809** | 28.17578 |
| r2_rand1 | 25335 | −20.000 | 0 | 28.49423 | **28.40809** | 28.07527 |

(all 13: `p.y_post ∈ {28.40635, 28.40809}`). The steady resting value is `28.40812` (1281 of 1305
resting frames; the rest within 3e-5): the landing frame lands 3e-5 low and the next 1–2 frames close
that gap. Attributing the residual to Baumgarte positional correction
(`phys.json#Physics2D.baumgarteScale = 0.2`, `maxLinearCorrection = 0.2`) is an
**[engine-assumption → Q-hero-10]**; the *measurement* is the two values and the ≤2-frame settle. The
body is configured `phys.json#rb2d.collisionDetectionMode = Continuous`; that a swept (rather than
discrete) test is *why* one step suffices at `v.y = −20` (a 0.4-unit step vs a 1.28-unit-tall
collider, so discrete would also have caught it) is likewise **[engine-assumption → Q-hero-10]** — the
measurement is only that no frame overshoots. [D28]

**Walls**: identical shape on the x axis. Blocked frames advance 0 and zero `v.x`:

| trace | frame | v.x pre | v.x post | Δp.x vs `v·dt` | state |
|---|---|---|---|---|---|
| r2_move | 25379 | +20.0 | 0 | −0.16546 of −0.4 (partial: first contact) | dashing |
| r2_move | 25380-25383 | +20.0 | 0 | −0.40002 (full block) | dashing |
| r2_move | 25386-25394 | +8.3 | 0 | −0.16600 (full block) | running |
| r2_rand1 | 25237 | −15.0 | 0 | +0.06545 of +0.3 (partial) | recoil |

### 1.3 Answer to Q1

**Velocity-driven, not solver-driven, and not state-dependent in mechanism** (only in which velocity
is written). The sim integrates `v.y += gravityScale·(−60)·0.02; p += v·0.02` after HeroController
writes `v`, and that is *exact to float rounding* whenever the hero is not in contact (404/404 air
frames). **A contact solver is needed, but only a normal-clamp one**, for exactly three cases:

1. **Resting on ground** (1289/1707 frames): `v.y ← 0` (the gravity increment is annihilated),
   `p.y ← unchanged`. Not an impulse — an exact clamp.
2. **Landing / ceiling** (13 + 7 frames): sweep along `v·dt`, place at the contact surface, zero the
   normal velocity, in one step. **Measured**: `v.y_post = 0` and `p.y_post ∈ {28.40635, 28.40809}`
   on all 13 landings, at impact speeds from −12.3 to −20 — i.e. no rebound and no residual normal
   velocity. Explaining that by `phys.json#Physics2D.velocityThreshold = 1.0` and the hero material
   `FrictionlessSurface` (`phys.json#heroColliders[0].sharedMaterial.name`) is an
   **[engine-assumption → Q-hero-10]**; the dump gives the settings, the trace gives the outcome, and
   nothing in `analysis/` connects them.
3. **Wall** (21 frames): same on x.

Nothing else the solver does is observable in the corpus: no friction (`Δv.x = 0` on ground in 98% of
frames), no restitution, no rotation (`phys.json#rb2d.freezeRotation = true`,
`constraints = FreezeRotation`), never sleeps (`phys.json#rb2d.sleepMode = NeverSleep`).
Of the 25 frames where `v.x` changed across the `HC_FIXED_POST → HC_UPDATE_PRE` segment, 18 are wall
contacts and 7 are the damage path (`StartRecoil → ResetMotion`, `HC:3789`, e.g. r2_move f25193
`v.x: 8.3 → 0` with `Δp.x = 0`). The trace bounds *when* that damage path ran — after
`HeroController.FixedUpdate`, before `HeroController.Update` of the same frame — but the recorder has
no marker for the solver step itself, so "it runs from a trigger callback inside the physics step" is
an **[engine-assumption → Q-hero-10]**, not a measurement. `analysis/specs/frame-order.md` §1.4
("This is a bound, not an attribution", currently `:156`) makes the same caveat about `phase==1`
and raises it as its Q-frame-1. [D28, R2-6]

**However the solver's contacts are not optional**, because part of HeroController's state machine is
driven by Unity collision callbacks: `OnCollisionEnter2D` (`HC:4841-4904`), `OnCollisionStay2D`
(`HC:4906-4959`), `OnCollisionExit2D` (`HC:4961-4999`). Writers per flag (grep, exhaustive) [D26]:

| flag | callback writers | non-callback writers |
|---|---|---|
| `cState.touchingWall`, `touchingWallL/R` | `HC:4921-4935`, `HC:4965-4978` | `DoWallJump` `HC:3465-3467`, `CancelWallsliding` `HC:4053-4054`, `SetStartingMotionState` `HC:4265`, `RelinquishControl*` `HC:2883-2884,2898-2899` |
| `cState.touchingNonSlider` | `HC:4918`, `HC:4957`, `HC:4968` | none |
| `ledgeBufferSteps = LEDGE_BUFFER_STEPS` | `HC:4997` | none (only decrement `HC:1195-1198`, clear `HC:1294` / `HC:4727`) |
| hard-landing entry (`DoHardLanding`) | `HC:4886`, `HC:4941` | none |
| `cState.onGround = true` | via `BackOnGround()` `HC:4195` from `HC:4890`, `HC:4945` | **also** `BackOnGround()` from `orig_Update` timers `HC:5156` (dash landing), `HC:5165` (hard landing) and `FailSafeChecks` `HC:3954`, `HC:3990`; and direct writes at `SetBackOnGround` `HC:1564`, scene-entry/respawn `HC:2471,2513,2567,2697,2797`, `SetStartingMotionState` `HC:4268` |
| `cState.onGround = false` | `HC:4950`, `HC:4992` | `FallCheck` `HC:3862`, recoil exit `HC:5181`, `SetStartingMotionState` `HC:4280` |

So the sim needs contact **events** (enter/stay/exit, with a contact normal for
`FindCollisionDirection`, `HC:4692-4715`) even though it needs almost none of the contact *solving* —
but "collision callbacks are the only source of `onGround`" would be wrong: the `hard_landing` /
`dash_landing` exits and the failsafes re-ground the hero from `Update` with no contact event at all.

### 1.4 What the hero collides with, and its own raycasts

| | |
|---|---|
| body collider | `BoxCollider2D` on `Knight`, layer 9 `Player`, `isTrigger=false`, offset `(0, −0.75)`, size `(0.5, 1.28125)`, `edgeRadius = 0.0025`, material `FrictionlessSurface` — `phys.json#heroColliders[0]` |
| hurtbox | `Knight/HeroBox`, layer 20 `Hero Box`, `isTrigger=true`, offset `(0.00557327271, −0.6942673)`, size `(0.455413818, 1.16978645)` — `phys.json#heroColliders[1]` |
| body collides with layers | 0 Default, 1 TransparentFX, 3, 6, 7, **8 Terrain**, 10 TransitionGates, 13 Hero Detector, 21 Grass, 25 Soft Terrain — `phys.json#layerCollisionMatrix.ignoreLayerCollision[9]` × `phys.json#layerNames` |
| solver settings | `velocityIterations 8`, `positionIterations 3`, `defaultContactOffset 0.01`, `baumgarteScale 0.2`, `maxLinearCorrection 0.2`, `velocityThreshold 1.0`, `queriesHitTriggers true`, `queriesStartInColliders false`, `autoSyncTransforms true`, `simulationMode FixedUpdate` — `phys.json#Physics2D` |
| body `interpolation` | `Interpolate` **at dump time** — `phys.json#rb2d.interpolation` | see below |

**`rb2d.interpolation` — dump and regime disagree, and the spec ports the regime.**
`phys.json#rb2d.interpolation` reads `Interpolate`, but R2 sets it to `None` on the hero and on every
`HealthManager` body, at `sceneLoaded` and again at `SceneReady`
(`oracle/Oracle/RegimeTweaks.cs:61-75`; armed only when `HK_ORACLE_NOINTERP=1`, `:30-36`). The dump
run did not set that variable; the r2 traces did (`STATE.md`, regime R2 definition). **The sim ports
`None`**, i.e. `transform.position == rb2d.position` on every frame with no accumulator-residual
offset, because that is the regime the traces and the trainer run under. Consequence: nothing in §1
depends on the interpolated transform. See Q-hero-11 and `analysis/open-questions.md` Q13.
[D29, C10, G12]

HeroController's own raycasts (all mask `256` = `1 << 8` = layer 8 `Terrain`, `PL:8`), all origins in
**world space from `col2d.bounds`** (the body BoxCollider2D, `hero.json#col2d`):

| method | origins | direction, distance | rejects | cite |
|---|---|---|---|---|
| `CheckTouchingGround()` | bounds `(min.x, center.y)`, `center`, `(max.x, center.y)` | down, `bounds.extents.y + 0.16` | **nothing** — the code inspects only `hit.collider != null` (`HC:4614`), unlike `CheckStillTouchingWall` which rejects `isTrigger` (`HC:4467`). Whether a trigger collider on layer 8 is therefore returned by the raycast depends on `phys.json#Physics2D.queriesHitTriggers = true` — **[engine-assumption → Q-hero-10]** | `HC:4602-4619` |
| `CheckStillTouchingWall(side, checkTop=false)` | side edge at `max.y` (only if `checkTop`), `center.y`, `min.y` | left/right, `0.1` | `isTrigger`, `SteepSlope`, `NonSlider` | `HC:4427-4525` |
| `CheckForBump(side)` | `(min.x+0.2, min.y+0.2)` & `(min.x+0.2, min.y−0.025)` (mirrored for right) | left/right, `0.32+0.2 = 0.52`; then 2 probe rays down `1.5` from `hit.point + (±0.1, 1)` | — | `HC:4527-4579` |
| `CheckNearRoof()` | `(min.x, max.y)` and `bounds.max` diagonally, plus `center.x ± size.x/4` at `max.y` | `(−0.5,1)`/`(0.5,1)` len 2; `up` len 1 | — | `HC:4581-4600` |
| `FindGroundPoint(p, ext)` | `p` | down, `FIND_GROUND_POINT_DISTANCE[_EXT]` | — | `HC:5067-5080` |
| `CheckForTerrainThunk` (nail→terrain) | box-cast `0.45×0.45` from bounds centre/top/bottom | attack dir, `2·range` (normal) or `1.5·range`; `range = 1 + 0.2·charm18 + 0.3·charm13`; mask `33554688` = layers 8 ∪ 25 | `isTrigger`, active `NonThunker` | `HC:4358-4425` |

`CheckTouching` (`HC:4621-4648`) and `CheckTouchingAdvanced` (`HC:4650-4690`) exist but have no call
site in this build (grep).

Every one of these rays starts **inside or on the hero's own body collider**
(`col2d.bounds.center`, `.min`, `.max`). `phys.json#Physics2D.queriesStartInColliders = false`; what
that setting does to a ray originating inside a collider — and whether the hero's own layer-9
collider could ever be returned by a layer-8-masked ray anyway — is not established by any source
here: **[engine-assumption → Q-hero-10]**. The layer mask makes the question moot for the hero's own
body but not for overlapping terrain. [D28, G7]

---

## 2. Per-frame control flow

### 2.1 Measured order (the trace *is* the measurement)

`analysis/traces/p0/r2_move.a.hktrace`, record stream, one agent step (`frames_per_wait = 2`):

```
[game frame N  ]  FIXED  HC_FIXED_PRE  HC_FIXED_POST  «physics solve»  (FSM events)  HC_UPDATE_PRE  HC_UPDATE_POST  FRAME
[game frame N+1]  FIXED  HC_FIXED_PRE  HC_FIXED_POST  «physics solve»  (FSM events)  HC_UPDATE_PRE  HC_UPDATE_POST  FRAME  OBS
[pause frame   ]                                                                     HC_UPDATE_PRE  HC_UPDATE_POST  EVENT:STEP
```

Evidence:
* `FIXED` (recorder MonoBehaviour) and `HC_FIXED_PRE` are **bit-identical in all 606 fixed steps**
  ⇒ the recorder's FixedUpdate runs first and nothing touches the body between them.
* `HeroController.FixedUpdate` changed `rb2d.velocity` in 129/600 frames and `rb2d.position` in
  **0**/600. `HeroController.Update` changed velocity in 26/600 and position in **0**/600.
* The solve segment (`HC_FIXED_POST → HC_UPDATE_PRE`) changed position in 466/600 and velocity in
  223/600 — it is the only segment that moves the body.
* Record kind counts for r2_move: 606 `FIXED`, 606 `HC_FIXED_PRE/POST`, 908 `HC_UPDATE_PRE/POST`,
  600 `FRAME`, **0 `HC_LATE_PRE/POST`** ⇒ HeroController has no `LateUpdate` (grep confirms).
* 908 `HC_UPDATE_PRE` − 606 `FIXED` = **302** Updates with no FixedUpdate = the `timeScale = 0`
  frozen frames. (Grouping the whole stream by `Time.frameCount` gives 909 frames, 606 live and
  **303** frozen — the extra frozen frame is the `SCENE_READY` frame, which has no `HC_UPDATE` pair:
  `analysis/specs/frame-order.md` §1.2 "Two frame classes" (currently `:40-54`), which tabulates
  606 live / 303 frozen / **302** frozen-carrying-an-`HC_UPDATE`-pair for r2_move. An earlier draft
  said 308 by subtracting `FRAME` from `HC_UPDATE`, which mixes two different counts.) [D24, C2, R2-6]

### 2.2 The pause frame — load-bearing

`TE:596-601`: `Time.timeScale = 1f` → `ActionDecoder.ApplyAction(...)` → `RaiseStepBegin`, then
`TE:614-618` `for i < frames_per_wait: yield return null; RaiseFrame()`, then `TE:627`
`Time.timeScale = 0`.

Two facts hold this together, both **measured, not assumed** [D28]:

1. **The mod coroutine resumes after every `Update` of its frame and before any LateUpdate-phase
   record** — `FRAME` always follows `HC_UPDATE_POST` within its frame group, and 0 `phase==2`
   records precede a `FRAME` in either trace — `analysis/specs/frame-order.md` §1.4 (currently
   `:174-181`) and §2 "Coroutine resume after `yield return null`" (currently `:223`). Only the first
   half is load-bearing here; whether *all* LateUpdates follow is that spec's Q-frame-2.
   Hence on the frozen frame **`HeroController.Update` runs before the new action is applied**, with
   the *previous* step's key state.
2. **`Time.deltaTime = 0` on the frozen frame** — `dt(F+1) = 0`, derived from the measured
   accumulation identity `Time.time(n) = Time.time(n−1) + dt(n)` (599/599 move, 479/479 rand1) plus
   Δ`Time.time` = 0.02 across the frozen frame: `analysis/specs/frame-order.md` §3.2 "The frozen
   frame" (currently `:279-285`). Deriving it instead from "timeScale 0 × captureDeltaTime 0.02"
   would be an engine rule, and deriving it from frame *F* still reporting `dt = 0.02` after the
   `timeScale = 0` write is circular — that spec's Q-frame-4.

Corroborated locally: `attack_time` / `dash_timer` / `recoilTimer` advance by exactly `0.02` per
`FRAME` record (2 per step), not 3 — the frozen-frame Update contributes zero dt (trace, r2_move
idx 481-495). But that Update *does* run `LookForInput` / `LookForQueueInput`
(`analysis/specs/frame-order.md` §3.2, "`HeroController.Update` still runs", currently `:294-301`,
enumerates the dt-free work): the two `Move` mispredictions
in §2.4 are exactly frames where `hero_state` left `no_input` on the last frame of a step, so the
frozen frame carried the first `LookForInput` to run (r2_rand1 f25012→f25014).

⇒ per agent step the sim must run: **3 × Update (dt = 0.02, 0.02, 0.00) and 2 × FixedUpdate**, with
the action applied between the 3rd Update and the next frame's FixedUpdate.

### 2.3 Method order inside each callback

`Update` (`HC:904-908`) = `ModHooks.OnHeroUpdate()` then `orig_Update()` (`HC:5106-5426`), in order:

| # | what | dt source | cite |
|---|---|---|---|
| 1 | `if (Time.frameCount % 10 == 0) Update10()` — OOB check, clamp `|scale.x| ≤ 1`, `z = 0.004` | frame counter | `HC:5108-5111`, `HC:1212-1231` |
| 2 | `current_velocity = rb2d.velocity` | — | `HC:5112` |
| 3 | `FallCheck()` | `Time.deltaTime` (`fallTimer`) | `HC:5113`, `HC:3853-3910` |
| 4 | `FailSafeChecks()` | `Time.deltaTime` | `HC:5114`, `HC:3946` |
| 5 | run/footstep audio + run effect | — | `HC:5115-5150` |
| 6 | `dash_landing`: `dashLandingTimer += dt`, `> DOWN_DASH_TIME → BackOnGround()` | `Time.deltaTime` | `HC:5151-5158` |
| 7 | `hard_landing`: `hardLandingTimer += dt`, `> HARD_LANDING_TIME → grounded + BackOnGround()` | `Time.deltaTime` | `HC:5159-5167` |
| 8 | `no_input` + `recoiling`: `recoilTimer += dt` until `RECOIL_DURATION[_STAL]`, then `CancelDamageRecoil()` | `Time.deltaTime` | `HC:5168-5191` |
| 9 | otherwise: **`LookForInput()`**; clear `recoiling`; `attack_time += dt` → `ResetAttacks` at `attackDuration`; bounce timer; `shroomBouncing` off when `current_velocity.y ≤ 0`; look up/down (`lookDelayTimer`) | `Time.deltaTime` | `HC:5192-5278` |
| 10 | **`LookForQueueInput()`** (jump/dash/attack edges + queues) | — | `HC:5279` |
| 11 | MP drain | `Time.deltaTime` | `HC:5280-5296` |
| 12 | wall-slide upkeep + cancel checks | `Time.deltaTime` | `HC:5297-5352` |
| 13 | `attack_cooldown -= dt`; `dashCooldownTimer -= dt`; `shadowDashTimer -= dt`; `preventCastByDialogueEndTimer -= dt` | `Time.deltaTime` | `HC:5357-5373` |
| 14 | nail-charge: `nailChargeTimer += dt` while `attack.IsPressed && CanNailCharge()` | `Time.deltaTime` | `HC:5376-5417` |
| 15 | `swimming` cancel; `parryInvulnTimer -= dt` | `Time.deltaTime` | `HC:5418-5425` |

Ordering is observable: `HeroDash()` sets `dashCooldownTimer = DASH_COOLDOWN = 0.6` at step 10, and
step 13 immediately subtracts one `dt` — the first FRAME after a dash press reads **0.58**
(trace r2_move idx 360). Ported rule reproduces it exactly.

`FixedUpdate` (`HC:910-1210`), in order — **no `Time.deltaTime` anywhere except the two timers
below**, everything else counts fixed steps:

| # | what | cite |
|---|---|---|
| 1 | `recoilingLeft/Right`: `recoilSteps++` while `≤ RECOIL_HOR_STEPS`, else `CancelRecoilHorizontal()` | `HC:912-922` |
| 2 | `dead → v = 0` | `HC:923-926` |
| 3 | `hard_landing`(¬onConveyor) or `dash_landing` → `ResetMotion()` | `HC:927-930` |
| 4 | else `no_input` → transition velocities, or `recoiling` → gravity off + `v = recoilVector` | `HC:931-957` |
| 5 | else: bump check (`running`), `Move(move_input)` unless dashing/backdashing, sprite flip, horizontal recoil | `HC:958-1014` |
| 6 | `ResetLook()` if looking and `|move_input| > 0.6` | `HC:1015-1018` |
| 7 | `if jumping → Jump()`; `if doubleJumping → DoubleJump()`; **`if dashing → Dash()`** | `HC:1019-1030` |
| 8 | casting / bouncing / wall-lock / wall-slide-unstick | `HC:1031-1102` |
| 9 | `MAX_FALL_VELOCITY` clamp | `HC:1104-1107` |
| 10 | `jumpQueueSteps++`, `doubleJumpQueueSteps++`, `dashQueueSteps++`, `attackQueueSteps++` | `HC:1108-1123` |
| 11 | wall-slide `v.y` ramp; cyclone; swim; superDashOnWall; conveyors; `slidingLeft` | `HC:1124-1190` |
| 12 | decrement `landingBufferSteps`, `ledgeBufferSteps`, `headBumpSteps`, `jumpReleaseQueueSteps` | `HC:1191-1206` |
| 13 | `positionHistory` shift; `cState.wasOnGround = cState.onGround` | `HC:1207-1209` |

`Dash()` (`HC:1508-1521`) does `dash_timer += Time.deltaTime` **inside FixedUpdate** (called from
`HC:1029`) — the one place in HeroController where a fixed-step path reads `Time.deltaTime`. Under R2
`phys.json#Time.captureDeltaTime = phys.json#Time.fixedDeltaTime = 0.02`, so whichever value the
property returns in that context, it is `0.02`. **Measured**: `dash_timer` runs 0.02…0.26 in steps of
exactly 0.02, one per FixedUpdate (trace r2_move idx 361-373), so the increment is `0.02` and the
increments-per-frame count is 1, not 2 — nothing here requires an engine rule about what
`Time.deltaTime` returns inside `FixedUpdate`. Under a regime where the two differ this would need
re-measuring: Q-hero-9. [D28, D25]

### 2.4 Input pipeline

`InControlManager.Update` calls `InputManager.UpdateInternal()` when
`updateMode == InControlUpdateMode.Default`, or when `updateMode == FixedUpdate && Time.timeScale`
is zero (`ICM:86-92`); `InControlManager.FixedUpdate` calls it when `updateMode == FixedUpdate`
(`ICM:94-100`). `updateMode` is a public serialized field with **no initializer** (`ICM:19`) and it is
in no dump, so **which branch runs is UNKNOWN → Q-hero-12**. The trace does **not** narrow it either:
`analysis/specs/frame-order.md` §2 (the `InputDeviceShim.Update` row, currently `:207`) and §4 "What
is and is not established about the InControl sample" (currently `:498-509`) bound the sample only to
the open interval *(`ApplyAction` in frame F's coroutine phase, `HeroController.Update` in frame
F+1)* — nothing excludes frame F+1's FixedUpdate or physics sub-phase, and that spec explicitly
states "InControl samples in the Update phase" is **not** established (its Q-frame-3). An earlier
draft of this spec asserted the Update phase; that was wrong and is corrected here. What survives is
the consequence, which is regime-independent: both consumers read state only
`HeroController.Update` writes, so the delay is one FixedUpdate wherever in that interval the sample
lands. `UpdateInternal` updates every
device (`InputDeviceShim.Update`, `PC:96-114`) then commits (`InputManager.cs:398-425`).
`InputHandler.Update` (`IH:263`) is UI/pause only; HeroController reads `inputHandler.inputActions`
directly. [D28]

`OneAxisInputControl` edge semantics (`OAIC:142-175`): `IsPressed = thisState.State`,
`WasPressed = thisState && !lastState`, `WasReleased = !thisState && lastState`. `lastState` is
rotated in `PrepareForUpdate` (`OAIC:264-280`) and `SetValue` (`OAIC:334-347`) when
`updateTick > pendingTick`, i.e. once per `InputManager` tick
(`InputManager.CurrentTick`, `analysis/decomp/Assembly-CSharp/InControl/InputManager.cs:128`;
devices are updated at `:398-407` and committed at `:411-425` with that tick) — how many of those occur
per rendered frame follows from `updateMode`, hence Q-hero-12, not from any rule asserted here. [D28]

**`move_input` can be ±0.7071, and the reason is not the one the previous draft gave.** [D28]
`HeroActions` sets `left/right.StateThreshold = 0.3`, `up/down = 0.5` (`HA:66-73`) and then
`moveVector.LowerDeadZone = 0.15; moveVector.UpperDeadZone = 0.95` (`HA:75-77`) — but those two
setters are `[Obsolete]` **no-ops** on `PlayerTwoAxisAction`:

> `[Obsolete("Please set this property on device controls directly. It does nothing here.")]`
> `public new float LowerDeadZone { get { return 0f; } set { } }`
> — `analysis/decomp/Assembly-CSharp/InControl/PlayerTwoAxisAction.cs:23-33` (same for
> `UpperDeadZone`, `:35-45`)

and the constructor sets `Raw = true` (`PlayerTwoAxisAction.cs:57`), so
`TwoAxisInputControl.UpdateWithAxes` takes the raw branch and skips `DeadZoneFunc` entirely
(`analysis/decomp/Assembly-CSharp/InControl/TwoAxisInputControl.cs:181`). `moveVector` is a
pass-through of `Utility.ValueFromSides(left, right, false)`
(`PlayerTwoAxisAction.cs:66-68`; `Utility.cs:360-373`).

The normalisation happens one level down, in the **device**: `InputDevice.Commit`
(`InputDevice.cs:708-714`) → `ProcessDPad` (`:681-706`). The shim drives its DPad controls with
`UpdateWithState` (`PC:104-107`), which never sets `Raw` (`OAIC:286-292`), so `ProcessDPad` takes the
deadzone branch (`InputDevice.cs:690-695`) with `DPad.DeadZoneFunc = DeadZone.Separate`
(`InputDevice.cs:414`), and `DeadZone.Separate` divides by the vector length
(`analysis/decomp/Assembly-CSharp/InControl/DeadZone.cs:16-26`, `num4 = sqrt(num2²+num3²)` at `:21`, `return new Vector2(num2 / num4, num3 / num4)` at `:26`). `ProcessDPad` then writes those
normalised components **back onto the digital controls**:
`DPadLeft.SetValue(DPad.Left.Value, updateTick)` and the three siblings (`InputDevice.cs:702-705`).
`ProcessDPad` is reached because `InputDevice.IsKnown => true` (`InputDevice.cs:141`) and
`InputDeviceShim` does not override it (`PC:7-114`).

⇒ with digital shim keys, one axis pressed gives `±1`; **two perpendicular keys give `±0.7071` on
each**. Confirmed in the trace: `move_input = 0.7071` at r2_rand1 f25008 with `left`+`up`+`jump`
held. Behaviourally this is absorbed by `FilterInput` (`0.7071 > 0.3 → 1`) and by the
`StateThreshold`s (`0.7071 > 0.5`), so no motion rule changes — but the raw `move_input` the
observation carries does, and so does the stale value described next.

`LookForInput` (`HC:3256-3328`): early-returns unless `acceptingInput && !gm.isPaused &&
isGameplayScene`, then `move_input = moveVector.Vector.x`, `vertical_input = moveVector.Vector.y`,
`FilterInput()` (`HC:5039-5065`: `>0.3 → 1`, `<−0.3 → −1`, else 0 for x; `>0.5 / <−0.5` for y).
⇒ **`move_input` keeps a raw unfiltered value (`±0.7071`) whenever `LookForInput` is skipped** —
in particular for the whole `hero_state == no_input` recoil window (`HC:5192`).

**Input latency (ported, then verified).** That the FixedUpdate block precedes the Update block within
a rendered frame is **measured, not an engine rule assumed here**: in both r2 traces `FIXED` never
appears after `HC_FIXED_PRE` in the same frame and is always first in the frame — 0 inversions
(`analysis/specs/frame-order.md` §1.2, currently `:55-58`, and the `RecorderBehaviour.FixedUpdate`
row of §2, currently `:198`), and §4 "Why exactly one FixedUpdate of delay" (currently `:474-496`)
derives the same one-FixedUpdate delay independently. Input is read only in Update (`LookForInput` from
`orig_Update` `HC:5194`) and consumed only in FixedUpdate (`Move(move_input)` `HC:976`) ⇒ a key change
applied at the end of frozen frame P is first *seen* by Update of frame P+1 and first *acted on by
FixedUpdate* at frame P+2. Verification: predicting [D28]
`post-FixedUpdate v.x` from the **previous** FRAME's raw left/right bits through
`moveVector → FilterInput → Move` gives **1463 / 1463 exact matches** over all four r2 traces
(all frames where none of dash/backdash/wallslide/cast/recoil/wall-lock/conveyor/wall-contact is
active). Using the *same* frame's `move_input` instead fails on 300/1490 — the lag is real.

---

## 3. State machine

### 3.1 `hero_state` (`ActorStates`, `AS:5-13`)

`grounded=0, idle=1, running=2, airborne=3, wall_sliding=4, hard_landing=5, dash_landing=6,
no_input=7, previous=8`. Set only via `SetState` (`HC:3616-3633`), which resolves `grounded` to
`running` iff `|move_input| > Mathf.Epsilon` else `idle` (`HC:3621`), resolves `previous` to
`prev_hero_state` (`HC:3624`), and on a real change stores `prev_hero_state` and calls
`animCtrl.UpdateState`.

| → state | trigger | cite |
|---|---|---|
| `grounded` (→idle/running) | `Move()` while `cState.onGround`; `BackOnGround()`; `hard_landing` timeout; `SetStartingMotionState` on ground | `HC:1253`, `HC:4194`, `HC:5164`, `HC:4269` |
| `airborne` | `FallCheck` when falling and not `no_input`; `OnCollisionStay2D` leaving ground; `OnCollisionExit2D`; `SetStartingMotionState` off ground; `CanSwim` path | `HC:3867`, `HC:4952`, `HC:4994`, `HC:4281`, `HC:3410` |
| `hard_landing` | `DoHardLanding()` from `OnCollisionEnter2D`/`Stay2D` bottom contact when `ShouldHardLand` | `HC:3498`, `HC:4886`, `HC:4941`, `HC:4832-4839` |
| `dash_landing` | `OnCollisionEnter2D` bottom while `cState.dashing && dashingDown` | `HC:4892-4897` |
| `no_input` | `StartRecoil` (damage); `RelinquishControl*`; `SceneInit` in a non-gameplay scene | `HC:3811`, `HC:2878`, `HC:887` |
| `wall_sliding` | **never assigned** in HeroController (grep: `ActorStates.wall_sliding` has no write site); wall slide is tracked only by `cState.wallSliding` | — |

`ActorStates` is never observed as 0, 4, 5, 6 in the r2 corpus (histogram: idle 719, running 548,
airborne 421, no_input 112) — see Q-hero-3.

### 3.2 `cState` flags (`HeroControllerStates`, `HCS:7-115`; 55 public bool fields, bit *i* of `FRAME.cstate` = field *i*)

| flag | set by | cleared by |
|---|---|---|
| `facingRight` | `FaceRight` `HC:1537`, `FlipSprite` `HC:1787` | `FaceLeft` `HC:1545`, `FlipSprite` |
| `onGround` | `BackOnGround` `HC:4195`, `SetStartingMotionState` `HC:4268` | `FallCheck` `HC:3862`, `OnCollisionStay2D` `HC:4950`, `OnCollisionExit2D` `HC:4992`, recoil end `HC:5181` |
| `jumping` | `HeroJump` `HC:3431`, `DoWallJump` `HC:3473`, infinite-air-jump `HC:3355`, `HeroJumpNoEffect` `HC:3441` | `CancelJump` `HC:4015` (from `Jump()` step overrun `HC:1298`, `JumpReleased` `HC:4213/4219`, roof bump `HC:4864`, `ResetMotion`) |
| `doubleJumping` | `DoDoubleJump` `HC:3489` | `CancelDoubleJump` `HC:4022` (step overrun / on ground `HC:1314,1318`, roof bump `HC:4865`) |
| `falling` | `FallCheck` `HC:3861` | `FallCheck` `HC:3895`, `BackOnGround` `HC:4183` |
| `dashing` | `HeroDash` `HC:3536` | `CancelDash` `HC:4032` (via `FinishedDashing` `HC:4232` when `dash_timer > DASH_TIME`, or `ResetMotion`) |
| `shadowDashing` | `HeroDash` when `hasShadowDash && shadowDashTimer ≤ 0` `HC:3562` | `CancelDash` `HC:4030` |
| `wallSliding` | `LookForInput` on wall+direction `HC:3272,3285`; `FinishedDashing` `HC:4240`; `RegainControl` `HC:2919` | `CancelWallsliding` `HC:4050` (unstick steps `HC:1087`, lost wall `HC:1094,1100`, down pressed `HC:3296`, on ground / `!touchingWall` / `!CanWallSlide` `HC:5310,5315,5319`) |
| `touchingWall`, `touchingWallL/R` | `OnCollisionStay2D` `HC:4921-4929` | `OnCollisionStay2D` `HC:4933-4935`, `OnCollisionExit2D` `HC:4965-4978`, `DoWallJump` `HC:3465-3467`, `CancelWallsliding` `HC:4053`, `SetStartingMotionState` `HC:4265` |
| `touchingNonSlider` | `OnCollisionStay2D` when the collider has `NonSlider` `HC:4957` | `HC:4918`, `OnCollisionExit2D` `HC:4968` |
| `attacking`, `upAttacking`, `downAttacking`, `altAttack` | `Attack()` `HC:1329,1354,1408,1437` | `ResetAttacks` `HC:4115-4117`, `ResetAttacksDash` `HC:4123-4125`, `CancelAttack` `HC:4076` |
| `recoiling` | `StartRecoil` `HC:3831` (after the freeze) | `CancelDamageRecoil` `HC:4097`, `Invulnerable` `HC:3843`, `Update` `HC:5197`, `HeroJump` `HC:3430`, `HeroDash` `HC:3523`, `orig_DoAttack` `HC:5516` |
| `recoilFrozen` | `StartRecoil` `HC:3812` | `StartRecoil` after `gm.FreezeMoment` `HC:3830` |
| `recoilingLeft/Right` | `RecoilLeft` `HC:2266`, `RecoilRight` `HC:2279`, `RecoilRightLong` `HC:2293`, `RecoilLeftLong` `HC:2308` | `CancelRecoilHorizontal` `HC:4090-4091` (after `RECOIL_HOR_STEPS`, `HC:920`) |
| `invulnerable` | `Invulnerable` coroutine `HC:3837` | `HC:3842` after `INVUL_TIME[_STAL]` |
| `willHardLand` | `FallCheck` when `fallTimer > BIG_FALL_TIME` `HC:3881`; `ForceHardLanding` `HC:2327` | `FallCheck` `HC:3899`, `BackOnGround` `HC:4186`, wall-slide start `HC:3273,3286`, `ResetHardLandingTimer` `HC:2860` |
| `bouncing`, `shroomBouncing` | `Bounce` `HC:2235`, `BounceHigh` `HC:2245`, `ShroomBounce` `HC:2256` | `CancelBounce` `HC:4083-4084` (timer `HC:5217`, roof bump `HC:4869,4874`) |
| `nailCharging` | `Update` while `attack.IsPressed && CanNailCharge()` `HC:5378`, and on charge-complete `HC:5405` | `HC:5384`, `ResetAttacks` `HC:4113`, `TakeDamage` `HC:1987`, paused w/o attack `HC:5415` |
| `wasOnGround` | end of every FixedUpdate = `cState.onGround` | — `HC:1209` |
| `casting`, `castRecoiling`, `focusing`, `superDashing`, `superDashOnWall`, `spellQuake`, `freezeCharge`, `swimming` (write side), `preventDash`, `dashCooldown`, `slidingLeft/Right`, `onConveyor*`, `inWalkZone`, `nearBench`, `isPaused` | **set from PlayMaker via `HeroController.SetCState(name, value)` `HC:2853-2856`** — not from HeroController itself | — |

`hazardDeath`, `hazardRespawning`, `dead`, `transitioning`, `backDashing`, `wallJumping`,
`lookingUp/Down[Anim]`, `inAcid`, `preventBackDash`, `backDashCooldown` — read by the predicates in §4
but out of scope for boss-arena motion (`backDashing` has no setter reachable here: `BackDash()` is an
empty body `HC:1523-1525`).

### 3.2b FSM-driven input lockout (`acceptingInput`)

`acceptingInput` is not written by any motion path in `HeroController`; it is toggled by
`IgnoreInput` (`HC:3137-3144`), `IgnoreInputWithoutReset` (`HC:3146-3152`), `AcceptInput`
(`HC:3154-3157`), `PauseInput`/`UnPauseInput` (`HC:3204-3234`) and `RelinquishControl`/`RegainControl`
(`HC:2888-2966`) — all called from PlayMaker.

Consequences for the port: while `acceptingInput == false`, `Move()` writes nothing (`HC:1255`) so
the hero **holds its current velocity**, and both `LookForInput` (`HC:3258`) and `LookForQueueInput`
(`HC:3332`) early-return, so no key edge is consumed.

Trace evidence (`r2_rand2`, 139 of 480 FRAMEs have `acceptingInput = false`, none of them recoiling):
the windows begin exactly when a hold action is committed — `super_dash` held at idx 212
(`cState.freezeCharge = 1`, `hero_state` still idle), `dream_nail` held at idx 308 (72 frames,
`hero_state = running`, `v.x` pinned at 0 the whole time and only returning to 8.3 on the frame
`acceptingInput` comes back). `r2_rand1` shows the same at 65/480 frames. So super-dash charge and
dream-nail charge lock the hero through this path, driven by the Knight's own FSMs, not by
HeroController. See §3.2c and Q-hero-8.

### 3.2c Hand-offs from HeroController to the Knight's own PlayMakerFSMs

`analysis/fsm/GG_Hornet_1.json` carries **12 FSMs whose `path` is exactly `Knight`** (the hero root)
plus ~148 more on its children. **These are owned by the FSM port, not by this spec** — what follows
is only the boundary: which HeroController code path hands off to which FSM, so the two ports meet at
a named edge. [G2]

HeroController's four cached hero-root FSM handles, and where they are bound:

| field | FSM — `analysis/fsm/GG_Hornet_1.json` `fsms[path=Knight]` | bound at | states | `restartOnEnable` |
|---|---|---|---|---|
| `proxyFSM` (`HC:729`) | `ProxyFSM` | `SetupGameRefs` `HC:5015` (`FSMUtility.LocateFSM(gameObject, "ProxyFSM")`) | 16 | true |
| `superDash` (`HC:691`) | `Superdash` | `Start` `HC:815` | 28 | false |
| `spellControl` (`HC:695`) | `Spell Control` | `Start` `HC:830` | 98 | false |
| `fsm_thornCounter` (`HC:693`) | `Thorn Counter` (on `Knight/Charm Effects`) | `Start` `HC:820` | 7 | true |

Every `SendEvent` HeroController makes to them (grep, exhaustive), with the motion path that raises it:

| motion path | target FSM | event | cite |
|---|---|---|---|
| `FallCheck` leaves ground; `OnCollisionStay2D`; `OnCollisionExit2D` | `ProxyFSM` | `HeroCtrl-LeftGround` | `HC:3864`, `HC:4951`, `HC:4993` |
| `OnCollisionEnter2D` bottom + `CheckTouchingGround()` | `ProxyFSM` | `HeroCtrl-Landed` | `HC:4849` |
| `FinishedDashing` | `ProxyFSM` | `HeroCtrl-DashEnd` | `HC:4235` |
| MP drain reaching `focusMP_amount` (focus heal) | `ProxyFSM` | `HeroCtrl-FocusCompleted` | `HC:5293` |
| `TakeDamage` / `AddHealth` / `TakeHealth` / `MaxHealth` | `ProxyFSM` | `HeroCtrl-HeroDamaged`, `HeroCtrl-Healed`, `HeroCtrl-MaxHealth`, `HeroCtrl-TookBlockerHit` | `HC:1921,1928,2032,2044,2174,2180,2185,2194` |
| scene entry into a super-dash / quake exit; respawn | `ProxyFSM` | `HeroCtrl-EnterSuperDash`, `HeroCtrl-EnterQuake`, `HeroCtrl-Respawned` | `HC:2499,2541,2400,2749,2787` |
| conveyor-zone velocity block; `CancelSuperDash()` | `Superdash` | `SLOPE CANCEL` | `HC:1185`, `HC:2868` |
| `OnCollisionEnter2D` / `OnCollisionStay2D` while `cState.superDashing` and still touching a wall | `Superdash` | `HIT WALL` | `HC:4845`, `HC:4910` |
| damage-recoil timeout (`CancelDamageRecoil` path) | `Thorn Counter` | `THORN COUNTER` | `HC:5188` |
| `Attack()` with charm 38 equipped (not equipped here, §5) | `fsm_orbitShield` — an inspector-assigned field (`HC:699`) with no `LocateFSM` binding, so which FSM it points to is not in the decomp; the candidates in `analysis/fsm/GG_Hornet_1.json` are `Knight/Charm Effects` / `Spawn Orbit Shield` and `_GameManager/GlobalPool/Orbit Shield(Clone)` / `Control` (Q-hero-13) | `SLASH` | `HC:1503` |
| `DoWallJump` / `HeroDash` | `dashBurst` (`Knight/Effects/Dash Burst`, `Effect Control`) | `CANCEL`, `PLAY` | `HC:3472`, `HC:3598` |
| `StartRecoil` | `damageEffectFSM` (`Knight/Effects/Damage Effect`, `Knight Damage`) | `DAMAGE` | `HC:3815` |
| `OnCollisionExit2D` leaving ground, not jumping | `fsm_fallTrail` (`Knight/Effects/Fall Trail`, `Trail Control`) | `PLAY` | `HC:4988` |

The reverse direction — FSM → hero — goes through three entry points, all of which the FSM port
drives and this spec only consumes:
`HeroController.SetCState(name, value)` (`HC:2853-2856`) for the flags listed in §3.2;
`IgnoreInput` / `AcceptInput` / `RelinquishControl` / `RegainControl` (`HC:3137-3157`, `HC:2888-2966`)
for the `acceptingInput` lockout of §3.2b; and the public velocity pokes `Bounce`/`ShroomBounce`/
`RecoilLeft`/`RecoilRight[Long]`/`RecoilDown`/`ForceHardLanding` (`HC:2229-2329`).
Two further hero-root FSMs are motion-relevant but never messaged by HeroController:
`Nail Arts` (35 states, `restartOnEnable=false`) and `Dream Nail` (47 states) — they read
`cState.nailCharging` / the dream-nail key and take over the body themselves.
`superDashing`/`casting`/`focusing` velocity is theirs, not HeroController's: `SuperDash()`
(`HC:1531-1533`) is an empty body. Also on the hero root: `Map Control`, `Roar Lock`,
`Surface Water`, `Spore Cooldown`, `Dream Return`, `Globalise`, `Control Interpolation`.

### 3.3 Transitions with their constants

All constants: `analysis/dumps/GG_Hornet_1/hero.json`, path `heroController[name=…].value`.

**Walk / run** — `Move(move_input)` (`HC:1249-1278`), called every FixedUpdate unless
`backDashing || dashing` (`HC:974`). Guards `acceptingInput && !cState.wallSliding` (`HC:1255`).
Speed selection (`HC:1257-1276`), in order: `inWalkZone → WALK_SPEED`; `inAcid → UNDERWATER_SPEED`;
`charm37 && onGround && charm31 → RUN_SPEED_CH_COMBO`; `charm37 && onGround → RUN_SPEED_CH`;
else `RUN_SPEED`. First statement: `if (cState.onGround) SetState(grounded)` (`HC:1251-1254`).

| const | value | path |
|---|---|---|
| `RUN_SPEED` | 8.3 | `hero.json#RUN_SPEED` |
| `RUN_SPEED_CH` | 10.0 | `hero.json#RUN_SPEED_CH` |
| `RUN_SPEED_CH_COMBO` | 11.5 | `hero.json#RUN_SPEED_CH_COMBO` |
| `WALK_SPEED` | 6.0 | `hero.json#WALK_SPEED` |
| `UNDERWATER_SPEED` | 6.0 | `hero.json#UNDERWATER_SPEED` |

Sprite flip (`HC:977-989`) is blocked while `cState.attacking && attack_time < ATTACK_RECOVERY_TIME`,
while `wallSliding`, and while `wallLocked`; a flip also calls `CancelAttack()`.

**Jump** — press handled in `LookForQueueInput` (`HC:3336-3364`): priority
`CanWallJump → DoWallJump`, `CanJump → HeroJump`, `CanDoubleJump → DoDoubleJump`,
`CanInfiniteAirJump → …`, else set `jumpQueuing = true, jumpQueueSteps = 0` (and the double-jump
queue). While held (`HC:3389-3405`): re-fire `HeroJump` if `jumpQueueSteps ≤ JUMP_QUEUE_STEPS &&
CanJump() && jumpQueuing`; else `DoDoubleJump`/`HeroJump` if `doubleJumpQueueSteps ≤
DOUBLE_JUMP_QUEUE_STEPS && CanDoubleJump() && doubleJumpQueuing`.
`HeroJump` (`HC:3425-3435`) sets `cState.jumping = true`, `jumpQueueSteps = 0`, `jumped_steps = 0`,
`doubleJumpQueuing = false`, `ResetLook()`, `cState.recoiling = false`.
`Jump()` in FixedUpdate (`HC:1280-1300`): while `jump_steps ≤ JUMP_STEPS` set
`v.y = JUMP_SPEED` (`JUMP_SPEED_UNDERWATER` in acid), `jump_steps++`, `jumped_steps++`,
`ledgeBufferSteps = 0`; else `CancelJump()`.

**Jump release / min jump** — `LookForInput`: `if (!jump.IsPressed) JumpReleased()` (`HC:3312-3315`).
`JumpReleased` (`HC:4204-4228`): if `v.y > 0 && jumped_steps ≥ JUMP_STEPS_MIN && !inAcid &&
!shroomBouncing` then (with `jumpReleaseQueueingEnabled == false`, `hero.json#jumpReleaseQueueingEnabled`)
`v.y = 0; CancelJump()`. Always clears `jumpQueuing`/`doubleJumpQueuing`.
`MIN_JUMP_SPEED` (3.0) is **not** part of the jump-release path despite the name: its only read site
is `LeaveScene`, `transition_vel = (0, MIN_JUMP_SPEED)` (`HC:2631`; declaration `HC:35`).
The short-hop is produced entirely by `JumpReleased` zeroing `v.y`.

| const | value | path |
|---|---|---|
| `JUMP_SPEED` | 16.65 | `hero.json#JUMP_SPEED` |
| `JUMP_SPEED_UNDERWATER` | 14.0 | `hero.json#JUMP_SPEED_UNDERWATER` |
| `MIN_JUMP_SPEED` | 3.0 (unused) | `hero.json#MIN_JUMP_SPEED` |
| `JUMP_STEPS` | 9 | `hero.json#JUMP_STEPS` |
| `JUMP_STEPS_MIN` | 4 | `hero.json#JUMP_STEPS_MIN` |
| `JUMP_TIME` | 0 (unused) | `hero.json#JUMP_TIME` |
| `JUMP_QUEUE_STEPS` | 2 | `hero.json#JUMP_QUEUE_STEPS` (= `HC:183`) |
| `JUMP_RELEASE_QUEUE_STEPS` | 2 | `hero.json#JUMP_RELEASE_QUEUE_STEPS` (= `HC:185`) |
| `DOUBLE_JUMP_QUEUE_STEPS` | 10 | `hero.json#DOUBLE_JUMP_QUEUE_STEPS` (= `HC:187`) |
| `LEDGE_BUFFER_STEPS` | 2 | `hero.json#LEDGE_BUFFER_STEPS` (= `HC:211`) |
| `LANDING_BUFFER_STEPS` | 5 | `hero.json#LANDING_BUFFER_STEPS` (= `HC:209`) |
| `HEAD_BUMP_STEPS` | 3 | `hero.json#HEAD_BUMP_STEPS` (= `HC:213`) |

*Verified*: 59/59 frames with `cState.jumping && 0 < jump_steps ≤ 9` have post-FixedUpdate
`v.y = 16.65` exactly; the FRAME record (post-solve) reads `15.702 = 16.65 − 0.948`.

**Double jump** — `DoDoubleJump` (`HC:3480-3492`): `cState.jumping = false`,
`cState.doubleJumping = true`, `doubleJump_steps = 0`, `doubleJumped = true`.
`DoubleJump()` (`HC:1302-1320`): while `doubleJump_steps ≤ DOUBLE_JUMP_STEPS`, **and only once
`doubleJump_steps > 3`**, set `v.y = JUMP_SPEED · 1.1`; `doubleJump_steps++`; else
`CancelDoubleJump()`; also cancel if `cState.onGround`.
`DOUBLE_JUMP_STEPS = 9` (`hero.json#DOUBLE_JUMP_STEPS`).
*Verified*: 24/24 qualifying frames give `v.y = 18.315 = 16.65·1.1` exactly; the 4 frames with
`doubleJump_steps ∈ {1,2,3}` show pure gravity, matching the `> 3` guard (trace r2_move idx 252-256).

**Fall** — `FallCheck` (`HC:3853-3910`), first thing in Update. If `v.y ≤ −1e−6` and
`!CheckTouchingGround()`: `falling = true`, `onGround = false`, `wallJumping = false`,
`SetState(airborne)` unless `no_input`; `fallTimer += Time.deltaTime` (reset to 0 while
`wallSliding`); at `fallTimer > BIG_FALL_TIME` set `willHardLand` and start the fall rumble.
Else `falling = false`, `fallTimer = 0`, `willHardLand = false`.
`BIG_FALL_TIME = 1.1` (`hero.json#BIG_FALL_TIME`).
Fall clamp: `MAX_FALL_VELOCITY = 20.0` (`hero.json#MAX_FALL_VELOCITY`), applied in FixedUpdate
(`HC:1104-1107`) when `!inAcid && !controlReqlinquished && !shadowDashing && !spellQuake`.
*Verified*: 1800/1800 frames satisfy `v.y ≥ −20`; the clamp is observed active at r2_rand1 f24787.

**Hard land** — `ShouldHardLand(collision)` (`HC:4832-4839`) = no `NoHardLanding` component on the
collider ∧ `cState.willHardLand` ∧ `!inAcid` ∧ `hero_state != hard_landing`. `DoHardLanding()`
(`HC:3494-3503`): gravity on, `ResetInput()`, `SetState(hard_landing)`, `CancelAttack()`,
`hardLanded = true`. Exit: `hardLandingTimer += dt > HARD_LANDING_TIME → SetState(grounded);
BackOnGround()` (`HC:5159-5167`), plus a failsafe at `HARD_LANDING_TIME + 0.3` (`HC:3948-3952`).
While in `hard_landing`, FixedUpdate calls `ResetMotion()` every step (`HC:927-930`) ⇒ velocity 0.
`HARD_LANDING_TIME = 0.8` (`hero.json#HARD_LANDING_TIME`).

**Dash** — press: `LookForQueueInput` `HC:3365-3376` (`CanDash → HeroDash`, else queue) and held-key
retry `HC:3415-3418` with `DASH_QUEUE_STEPS`. `HeroDash` (`HC:3511-3607`): `airDashed = true` if
airborne and not in acid; `ResetAttacksDash`; `CancelBounce`; `ResetLook`; face by held direction
(or flip if wall-sliding); `cState.dashing = true` (`HC:3536`); `dashQueueSteps = 0` (`HC:3537`);
`dashingDown = down.IsPressed && !cState.onGround && charm31 && !left.IsPressed && !right.IsPressed`
(`HC:3539-3543`, else-branch `HC:3545-3550`);
`dashCooldownTimer = charm31 ? DASH_COOLDOWN_CH : DASH_COOLDOWN` (`HC:3551-3558`);
if `hasShadowDash && shadowDashTimer ≤ 0` → `shadowDashTimer = SHADOW_DASH_COOLDOWN`,
`cState.shadowDashing = true` (`HC:3559-3562`). [D23]
`Dash()` in FixedUpdate (`HC:1508-1521`): gravity off, `ResetHardLandingTimer()`;
if `dash_timer > DASH_TIME → FinishedDashing()`; else `v = OrigDashVector()`;
`dash_timer += Time.deltaTime`.
`OrigDashVector()` (`HC:5428-5433`) = speed `charm16 && shadowDashing ? DASH_SPEED_SHARP : DASH_SPEED`;
`dashingDown → (0, −speed)`; else `(±speed, 0)`, or `(±speed, onGround ? 4 : 5)` if
`CheckForBump(side)` (the literals 4/5 are inline at `HC:5431`; the equivalent `orig_Dash` at
`HC:5453,5462` writes them as `BUMP_VELOCITY` / `BUMP_VELOCITY_DASH`).
`FinishedDashing` (`HC:4230-4255`): `CancelDash`, gravity on, and if `touchingWall && !onGround &&
hasWalljump && (touchingWallL || touchingWallR)` → enter `wallSliding`.

| const | value | path |
|---|---|---|
| `DASH_SPEED` | 20.0 | `hero.json#DASH_SPEED` |
| `DASH_SPEED_SHARP` | 28.0 | `hero.json#DASH_SPEED_SHARP` |
| `DASH_TIME` | 0.25 | `hero.json#DASH_TIME` |
| `DASH_COOLDOWN` | 0.6 | `hero.json#DASH_COOLDOWN` |
| `DASH_COOLDOWN_CH` | 0.4 | `hero.json#DASH_COOLDOWN_CH` |
| `DASH_QUEUE_STEPS` | 10 | `hero.json#DASH_QUEUE_STEPS` |
| `SHADOW_DASH_COOLDOWN` | 1.5 | `hero.json#SHADOW_DASH_COOLDOWN` |
| `BUMP_VELOCITY` | 4.0 | `hero.json#BUMP_VELOCITY` (= `HC:205`) |
| `BUMP_VELOCITY_DASH` | 5.0 | `hero.json#BUMP_VELOCITY_DASH` (= `HC:207`) |
| `SHADOW_DASH_SPEED` 20.0 / `SHADOW_DASH_TIME` 0.25 / `SUPER_DASH_SPEED` 20.0 / `BACK_DASH_*` | — | declared, **no read site** in the decomp (grep) |

*Verified*: 39/39 dash frames have `|v.x| = 20.0` and `gravityScale = 0`; `dash_timer` runs
0.02→0.26 in 13 fixed steps and `FinishedDashing` fires on the step after it exceeds 0.25.
Also verified: on the FinishedDashing step, `Move()` is skipped (`HC:974` still sees
`cState.dashing == true`) so `v.x` stays 20 for one extra step before dropping to 8.3 —
trace r2_move idx 374→375.

**Wall slide / wall jump** — entry in `LookForInput` (`HC:3265-3293`) requires
`hasWalljump && CanWallSlide() && !cState.attacking` and the matching `touchingWallL/R` +
`left/right.IsPressed`; sets `wallSliding`, clears `airDashed`/`doubleJumped`, faces the wall.
FixedUpdate wall-slide block (`HC:1071-1102`): `wallUnstickSteps++` while pressing *away* from the
wall, `≥ WALL_STICKY_STEPS → CancelWallsliding()`; also cancel + flip if `CheckStillTouchingWall`
fails. Velocity ramp (`HC:1124-1142`) drives `v.y` toward `WALLSLIDE_SPEED` by `WALLSLIDE_DECEL`
per step — with `WALLSLIDE_DECEL = 0` the ramp is a no-op, so `v.y` is pinned to `WALLSLIDE_SPEED`
only via the `v.y < WALLSLIDE_SPEED` branch (`HC:1134-1141`). **UNKNOWN** — see Q-hero-1.
`DoWallJump` (`HC:3447-3478`): face away, `wallJumpedL/R`, `CancelWallsliding`, clear
`touchingWall*`, `airDashed = doubleJumped = false`, `currentWalljumpSpeed = WJ_KICKOFF_SPEED`,
`walljumpSpeedDecel = (WJ_KICKOFF_SPEED − RUN_SPEED)/WJLOCK_STEPS_LONG`, `cState.jumping = true`,
`wallLockSteps = 0`, `wallLocked = true`.
FixedUpdate wall-lock block (`HC:1054-1070`): `v.x = ±currentWalljumpSpeed`, `wallLockSteps++`,
`wallLocked = false` once `> WJLOCK_STEPS_LONG`, `currentWalljumpSpeed -= walljumpSpeedDecel`.
Early release: pressing the opposite direction with `wallLockSteps ≥ WJLOCK_STEPS_SHORT`
(`HC:3299-3306`).

| const | value | path |
|---|---|---|
| `WALLSLIDE_SPEED` | −8.0 | `hero.json#WALLSLIDE_SPEED` |
| `WALLSLIDE_DECEL` | 0.0 | `hero.json#WALLSLIDE_DECEL` |
| `WALL_STICKY_STEPS` | 3 | `hero.json#WALL_STICKY_STEPS` |
| `WJ_KICKOFF_SPEED` | 16.0 | `hero.json#WJ_KICKOFF_SPEED` |
| `WJLOCK_STEPS_SHORT` | 5 | `hero.json#WJLOCK_STEPS_SHORT` |
| `WJLOCK_STEPS_LONG` | 10 | `hero.json#WJLOCK_STEPS_LONG` |

⇒ `walljumpSpeedDecel = (16.0 − 8.3)/10 = 0.77`.
**Not exercised by the r2 corpus** (0 frames with `cState.wallSliding`, `wallLocked`, or
`wallJumpedL/R`) — ported, unverified. Q-hero-2.

**Look up / down** — only while `hero_state == idle`, `!controlReqlinquished`, `!gm.isPaused`
(`HC:5225-5275`): holding up/down accumulates `lookDelayTimer += dt`; `cState.lookingUp/Down` at
`LOOK_DELAY`, the `…Anim` flags at `LOOK_ANIM_DELAY`; any other input → `ResetLook()`.
FixedUpdate also calls `ResetLook()` if looking and `|move_input| > 0.6` (`HC:1015-1018`).
`LOOK_DELAY = 0.85` (`hero.json#LOOK_DELAY`, = `HC:193`), `LOOK_ANIM_DELAY = 0.25`
(`hero.json#LOOK_ANIM_DELAY`, = `HC:195`).

**Damage recoil** — `TakeDamage` (`HC:1825`) → `StartCoroutine(StartRecoil(side, …))` `HC:2014`.
`StartRecoil` (`HC:3782-3833`): no-op if already `recoiling`; `ResetMotion()`; gravity off;
`recoilVector = (±RECOIL_VELOCITY, RECOIL_VELOCITY·0.5)` (`HC:3794,3801`, sign away from the impact
side, with a `FlipSprite` to face the source); `SetState(no_input)`; `recoilFrozen = true`;
`Invulnerable(charm4 ? INVUL_TIME_STAL : INVUL_TIME)`;
`yield gm.FreezeMoment(DAMAGE_FREEZE_DOWN, DAMAGE_FREEZE_WAIT, DAMAGE_FREEZE_UP, 0.0001)`;
then `recoilFrozen = false`, `recoiling = true`.
While `no_input && recoiling`, FixedUpdate holds `v = recoilVector` with gravity off (`HC:952-956`).
Exit (`HC:5170-5189`): `recoilTimer += dt` until `RECOIL_DURATION` (`RECOIL_DURATION_STAL` with
charm 4), then `CancelDamageRecoil()` (`HC:4095-4102`: `ResetMotion()`, gravity on, damage mode
full) and `SetState(previous)` — or `airborne` if `prev_hero_state ∈ {idle, running}` and
`!CheckTouchingGround()`.

| const | value | path |
|---|---|---|
| `RECOIL_VELOCITY` | 15.0 | `hero.json#RECOIL_VELOCITY` |
| `RECOIL_DURATION` | 0.2 | `hero.json#RECOIL_DURATION` |
| `RECOIL_DURATION_STAL` | 0.08 | `hero.json#RECOIL_DURATION_STAL` |
| `INVUL_TIME` | 1.3 | `hero.json#INVUL_TIME` |
| `INVUL_TIME_STAL` | 1.75 | `hero.json#INVUL_TIME_STAL` |
| `DAMAGE_FREEZE_DOWN / WAIT / UP` | 0.001 / 0.25 / 0.05 | `hero.json#DAMAGE_FREEZE_DOWN`, `…_WAIT`, `…_UP` |

*Verified*: 55/59 recoil frames read exactly `(±15.000, +7.500)` with `gravityScale = 0`; the 4
mismatches are the first frame of each recoil, where `cState.recoiling` was set by the coroutine
*after* that frame's FixedUpdate — consistent with the ported ordering.
**`gm.FreezeMoment` is no-op'd by the oracle mod** (`docs/frame-order.md`, `TE` Setup), so
`recoilFrozen` lasts exactly 1 frame in R2 traces and `DAMAGE_FREEZE_*` are inert. Q-hero-5.

**Horizontal recoil (nail on terrain / enemy)** — `RecoilLeft/Right` (`HC:2260-2285`) require
`!recoilingLeft && !recoilingRight && !charm14 && !controlReqlinquished`; they `CancelDash()`,
`recoilSteps = 0`, and set `v.x = ∓RECOIL_HOR_VELOCITY`. `…Long` variants (`HC:2286-2313`) ignore
charm 14, also `ResetAttacks()`, set `recoilLarge = true` and use `RECOIL_HOR_VELOCITY_LONG`.
FixedUpdate re-applies each step (`HC:990-1013`): `v.x = ∓num` if `|v.x|` is on the wrong side, else
`v.x ∓= num`. Duration: `recoilSteps++` while `≤ RECOIL_HOR_STEPS`, else `CancelRecoilHorizontal()`
(`HC:912-922`).
`RECOIL_HOR_VELOCITY = 3.75`, `RECOIL_HOR_VELOCITY_LONG = 16.0`, `RECOIL_HOR_STEPS = 8.0`
(`hero.json#RECOIL_HOR_VELOCITY`, `…_LONG`, `…_STEPS`).
Trigger: `CheckForTerrainThunk` (`HC:4358-4425`) — box-cast against layers 8 ∪ 25 for
`NAIL_TERRAIN_CHECK_TIME = 0.12` (`hero.json#NAIL_TERRAIN_CHECK_TIME`, = `HC:203`) after each swing.
**Not exercised by the r2 corpus** (0 frames with `recoilingLeft/Right`). Q-hero-2.

**Attack lockouts of movement** — `Attack()` (`HC:1322-1506`) never writes velocity. It gates motion
only through:
* `attackDuration` — latched at the *start* of each swing, `attackDuration = charm32 ?
  ATTACK_DURATION_CH : ATTACK_DURATION` (`HC:1330-1337`). Nothing decrements it. `orig_Update`
  **increments `attack_time`** and compares it against that constant:
  `attack_time += Time.deltaTime; if (attack_time >= attackDuration) { ResetAttacks(); … }`
  (`HC:5200-5208`; `ResetAttacks` zeroes `attack_time`, `HC:4118`). [D22]
* `attack_cooldown = charm32 ? ATTACK_COOLDOWN_TIME_CH : ATTACK_COOLDOWN_TIME` (`HC:5517-5524`),
  counted down in `orig_Update` (`HC:5357-5360`), gating `CanAttack`;
* `cState.attacking && attack_time < ATTACK_RECOVERY_TIME` blocks the sprite flip (`HC:977`) and is a
  term in `CanDash`, `CanCast`, `CanSuperDash`, `CanDreamNail` (§4).

| const | value | path |
|---|---|---|
| `ATTACK_DURATION` | 0.35 | `hero.json#ATTACK_DURATION` |
| `ATTACK_DURATION_CH` | 0.28 | `hero.json#ATTACK_DURATION_CH` |
| `ATTACK_COOLDOWN_TIME` | 0.41 | `hero.json#ATTACK_COOLDOWN_TIME` |
| `ATTACK_COOLDOWN_TIME_CH` | 0.25 | `hero.json#ATTACK_COOLDOWN_TIME_CH` |
| `ATTACK_RECOVERY_TIME` | 0.1 | `hero.json#ATTACK_RECOVERY_TIME` |
| `ATTACK_QUEUE_STEPS` | 5 | `hero.json#ATTACK_QUEUE_STEPS` (= `HC:189`) |
| `ALT_ATTACK_RESET` | 0.5 | `hero.json#ALT_ATTACK_RESET` |

**`attackDuration` is state, not a constant — do not read it from the dump.** [D21, C14]
`hero.json#attackDuration = 0.0`: it is assigned only inside `Attack()` (`HC:1332` / `HC:1336`), and
the hero is idle at `SceneReady` when the dumper runs, so the dump captures a stale 0. The value the
sim must use is the **code path**: with `equippedCharm_32` true (§5), `attackDuration =
ATTACK_DURATION_CH = 0.28`. Corroborated independently by the trace and by
`analysis/specs/damage-path.md` §6.2 ("Attack duration: … ⇒ `attackDuration = ATTACK_DURATION_CH =
0.28`", currently `:1037-1039`).

*Verified*: `attack_time` ticks 0.02/Update and `ResetAttacks` fires on the Update where it first
reaches ≥ 0.28 — i.e. the 15th (float accumulation: 14 × 0.02 evaluates just below 0.28).
Trace r2_move idx 480-495. This is also the trace confirmation that `attackDuration` is 0.28 and not
the dumped 0 (with 0 the very first Update after `Attack()` would reset it).

**Gravity** — `DEFAULT_GRAVITY = 0.79` (`hero.json#DEFAULT_GRAVITY`), matching the live
`phys.json#rb2d.gravityScale = 0.79`. `UNDERWATER_GRAVITY = 0.225` (`hero.json#UNDERWATER_GRAVITY`)
is only set from `EnterAcid`, which is `[Obsolete]` and has no call site (`HC:4286-4292`).
*Verified*: 1646/1646 non-dash non-recoil frames read `gravityScale = 0.79`; dash and recoil frames
read `0.0`.

---

## 4. The nine `Can*` predicates

`gm.isPaused` is `GameManager.isPaused`; `CanInput()` returns `acceptingInput` (`HC:1770-1773`).
Under R2 in a boss arena: `acceptingInput = true`, `controlReqlinquished = false`,
`isGameplayScene = true`, `damageMode = FULL_DAMAGE`, `transitionState = WAITING_TO_TRANSITION`
(`hero.json#acceptingInput`, `#controlReqlinquished`, `#isGameplayScene`, `#damageMode`,
`#transitionState`).

| predicate | cite | exact expression |
|---|---|---|
| `CanJump` | `HC:4717-4733` | `hero_state ∉ {no_input, hard_landing, dash_landing} ∧ ¬wallSliding ∧ ¬dashing ∧ ¬backDashing ∧ ¬jumping ∧ ¬bouncing ∧ ¬shroomBouncing ∧ ( onGround ∨ ( ledgeBufferSteps > 0 ∧ ¬dead ∧ ¬hazardDeath ∧ ¬controlReqlinquished ∧ headBumpSteps ≤ 0 ∧ ¬CheckNearRoof() ) )` — **the ledge branch has the side effect `ledgeBufferSteps = 0`** (`HC:4727`) |
| `CanDoubleJump` | `HC:4735-4742` | `pd.hasDoubleJump ∧ ¬controlReqlinquished ∧ ¬doubleJumped ∧ ¬inAcid ∧ hero_state ∉ {no_input, hard_landing, dash_landing} ∧ ¬dashing ∧ ¬wallSliding ∧ ¬backDashing ∧ ¬attacking ∧ ¬bouncing ∧ ¬shroomBouncing ∧ ¬onGround` |
| `CanWallJump` | `HC:4811-4830` | `pd.hasWalljump ∧ ¬touchingNonSlider ∧ ( wallSliding ∨ ( touchingWall ∧ ¬onGround ) )` |
| `CanDash` | `HC:4762-4769` | `hero_state ∉ {no_input, hard_landing, dash_landing} ∧ dashCooldownTimer ≤ 0 ∧ ¬dashing ∧ ¬backDashing ∧ ( ¬attacking ∨ attack_time ≥ ATTACK_RECOVERY_TIME ) ∧ ¬preventDash ∧ ( onGround ∨ ¬airDashed ∨ wallSliding ) ∧ ¬hazardDeath ∧ pd.canDash` |
| `CanAttack` | `HC:4771-4778` | `attack_cooldown ≤ 0 ∧ ¬attacking ∧ ¬dashing ∧ ¬dead ∧ ¬hazardDeath ∧ ¬hazardRespawning ∧ ¬controlReqlinquished ∧ hero_state ∉ {no_input, hard_landing, dash_landing}` |
| `CanCast` | `HC:2973-2980` | `¬gm.isPaused ∧ ¬dashing ∧ hero_state ≠ no_input ∧ ¬backDashing ∧ ( ¬attacking ∨ attack_time ≥ ATTACK_RECOVERY_TIME ) ∧ ¬recoiling ∧ ¬recoilFrozen ∧ ¬transitioning ∧ ¬hazardDeath ∧ ¬hazardRespawning ∧ acceptingInput ∧ preventCastByDialogueEndTimer ≤ 0` |
| `CanNailCharge` | `HC:4780-4787` | `¬attacking ∧ ¬controlReqlinquished ∧ ¬recoiling ∧ ¬recoilingLeft ∧ ¬recoilingRight ∧ pd.hasNailArt` |
| `CanDreamNail` | `HC:3038-3045` | `¬gm.isPaused ∧ hero_state ≠ no_input ∧ ¬dashing ∧ ¬backDashing ∧ ( ¬attacking ∨ attack_time ≥ ATTACK_RECOVERY_TIME ) ∧ ¬controlReqlinquished ∧ ¬hazardDeath ∧ **rb2d.velocity.y > −0.1** ∧ ¬hazardRespawning ∧ ¬recoilFrozen ∧ ¬recoiling ∧ ¬transitioning ∧ pd.hasDreamNail ∧ onGround` |
| `CanSuperDash` | `HC:3029-3036` | `¬gm.isPaused ∧ hero_state ≠ no_input ∧ ¬dashing ∧ ¬hazardDeath ∧ ¬hazardRespawning ∧ ¬backDashing ∧ ( ¬attacking ∨ attack_time ≥ ATTACK_RECOVERY_TIME ) ∧ ¬slidingLeft ∧ ¬slidingRight ∧ ¬controlReqlinquished ∧ ¬recoilFrozen ∧ ¬recoiling ∧ ¬transitioning ∧ pd.hasSuperDash ∧ ( onGround ∨ wallSliding )` |

Related predicates HeroController itself uses (needed for the ported transitions, not exported to
the observation): `CanWallSlide` `HC:4789-4800`, `CanInfiniteAirJump` `HC:4744-4751`,
`CanSwim` `HC:4753-4760`, `CanTakeDamage` `HC:4802-4809`, `CanFocus` `HC:2982-2989`
(**= `CanCast` ∧ `onGround`, minus the dialogue timer**), `CanNailArt` `HC:2991-3000`
(has the side effect `nailChargeTimer = 0` on **both** paths).

### 4.1 Input-side gates in `oracle/Game/ProxyController.cs`

`ActionDecoder.ApplyAction` (`PC:321-405`) resolves the hard-commit state machine, then applies
**movement → direction → jump → action** in that order (`PC:364-402`).

| decoder case | shim method | gate | key writes |
|---|---|---|---|
| `a[0]=0/1/else` | `Left/Right/StopLR` `PC:159-160,257` | none | `KeyLeft/KeyRight` mutually exclusive |
| `a[1]=0/1/else` | `Up/Down/StopUD` `PC:161-162,258` | none | `KeyUp/KeyDown` mutually exclusive |
| `a[3]=0` | `Jump()` `PC:164-169` | **`CanJump() ∨ CanDoubleJump() ∨ CanWallJump()`** (via reflection on the private methods, `PC:122-132`) | `KeyJump=true`, `KeyDash=false` |
| `a[2]=0` attack tap | `AttackTap()` `PC:178-187` | `CanAttack()` | `_retapAttack = KeyAttack`; `KeyAttack=true`; clears cast/dream/superdash |
| `a[2]=1` nail charge | `NailCharge()` `PC:190-200` | **`KeyAttack` already held ⇒ pass through unconditionally**; else `CanNailCharge()` | `KeyAttack=true`; clears cast/dream/superdash |
| `a[2]=2` spell tap | `SpellTap()` `PC:203-212` | `CanCast()` | `_retapCast`; `KeyCast=true`; clears attack/dream/superdash |
| `a[2]=3` focus | `Focus()` `PC:215-223` | `KeyCast ∨ CanCast()` — note it gates on **`CanCast`, not `CanFocus`** | `KeyCast=true`; clears attack/dream/superdash |
| `a[2]=4` dash | `Dash()` `PC:225-235` | `CanDash()` | `KeyDash=true`; clears **jump**, attack, cast, dream, superdash |
| `a[2]=5` dream nail | `DreamNail()` `PC:238-245` | `KeyDreamNail ∨ CanDreamNail()` | `KeyDreamNail=true`; clears attack/cast/superdash |
| `a[2]=6` super dash | `SuperDash()` `PC:248-255` | `KeySuperDash ∨ CanSuperDash()` | `KeySuperDash=true`; clears attack/cast/dream |
| `a[2]=7` none | `StopActions()` `PC:260-269`, plus `StopJD()` **only if `a[3] ≠ 0`** `PC:400` | none | clears attack/cast/dash/dream/superdash (+ jump only in that sub-case) |

Semantics the sim must reproduce:
* **`KeyJump` is only released** by `a[2]=4` (dash) or by `a[2]=7 ∧ a[3]≠0`. With `a[2] ∈ {0..3,5,6}`
  and `a[3]=1`, a previously-pressed jump stays held (`PC:388-402`).
* `AttackTap`/`SpellTap` force **one device tick of `false`** (`_retap*`, consumed in
  `InputDeviceShim.Update` `PC:99-102`) so `WasPressed` fires again on a re-tap.
* `FaceDirection()` (`PC:171-175`) calls `HeroController.FaceRight/FaceLeft` **directly** at
  StepBegin — i.e. `cState.facingRight` and `transform.localScale.x` can change *outside* any
  HeroController callback, before the next FixedUpdate. Applies to attack/nail-charge/spell/focus/dash.
* Hard commit (`PC:281-362`): a freely-chosen hold (`a[2] ∈ {1,3,5,6}`) locks `a[2]` for
  `ceil(hold_seconds / (frames_per_wait × 0.00848))` steps then forces one `a[2]=7` release step.
  `hold_seconds` = 1.5 / 0.5 / 3.0 / 1.0 for nail_charge / focus / dream_nail / super_dash
  (`PC:281-287`). The divisor uses the hard-coded `kCaptureDeltaTime = 0.00848` (`PC:295`),
  **not** the R2 value 0.02 — under R2 with `frames_per_wait = 2` the lock is
  `ceil(1.5/0.01696) = 89` steps for nail charge, i.e. 3.56 s of game time. Q-hero-6.
* The nine `Can*` are evaluated **at StepBegin**, on the pause frame after that frame's
  HeroController.Update — one Update earlier than the FixedUpdate that will act on the key
  (§2.4). The trainer's `can_*` observation is sampled at the same point.

---

## 5. Save-file context

`oracle/Resource/save_file.json` and the live `analysis/dumps/GG_Hornet_1/playerdata.json` agree on
every field below.

| field | value | gates |
|---|---|---|
| `hasDash` / `canDash` | true / true | `CanDash` `HC:4764` reads **`canDash`** |
| `hasWalljump` / `canWallJump` | true / true | `CanWallJump` `HC:4813`, `CanWallSlide` `HC:4795`, `LookForInput` `HC:3265`, `FinishedDashing` `HC:4236` read **`hasWalljump`** |
| `hasDoubleJump` | true | `CanDoubleJump` `HC:4737` |
| `hasSuperDash` | true | `CanSuperDash` `HC:3031` |
| `hasDreamNail` | true | `CanDreamNail` `HC:3040` |
| `hasShadowDash` | true | `HeroDash` `HC:3559` — **every dash off cooldown is a shadow dash** (`shadowDashTimer` gate) |
| `hasNailArt` | true | `CanNailCharge` `HC:4782` |
| `hasAcidArmour` | true | not motion-relevant |
| `infiniteAirJump` | false | `CanInfiniteAirJump` `HC:4746` |
| `isInvincible` | false | `CanTakeDamage` `HC:4804` |
| `health` / `maxHealth` / `maxHealthBase` | 9 / 9 / 9 | — |
| `nailDamage` | 21 | — |
| `charmSlots` / `charmSlotsFilled` | 11 / 11 | — |

Equipped charms (`save#equippedCharm_N`, `pd.json#equippedCharm_N`): **13, 18, 25, 32, 36**.
All other `equippedCharm_*` are false.

| charm | motion / combat effect in HeroController | cite | state here |
|---|---|---|---|
| 13 | `+0.3` nail-range multiplier in `CheckForTerrainThunk`; `MANTIS_CHARM_SCALE` on grubberfly beams | `HC:4373-4376`, `HC:1378` | **equipped** |
| 18 | `+0.2` nail-range multiplier in `CheckForTerrainThunk` | `HC:4369-4372` | **equipped** |
| 25 | no read site in `HeroController.cs` (grep) | — | **equipped** |
| 32 | `attackDuration = ATTACK_DURATION_CH (0.28)`; `attack_cooldown = ATTACK_COOLDOWN_TIME_CH (0.25)` | `HC:1330-1337`, `HC:5517-5522` | **equipped** |
| 36 | no read site in `HeroController.cs` (grep) | — | **equipped** |
| 37 | `RUN_SPEED_CH` / `RUN_SPEED_CH_COMBO` on the ground | `HC:1265-1272` | not equipped ⇒ **`RUN_SPEED = 8.3`** |
| 31 | down-dash condition `HC:3539-3543`; `dashCooldownTimer = DASH_COOLDOWN_CH` `HC:3551-3554`; `RUN_SPEED_CH_COMBO` with 37 `HC:1265` [D23] | not equipped ⇒ **`DASH_COOLDOWN = 0.6`, `dashingDown` unreachable** |
| 16 | `DASH_SPEED_SHARP (28)` while shadow-dashing | `HC:5430`, `HC:5444` | not equipped ⇒ **`DASH_SPEED = 20`** |
| 26 | `nailChargeTime = NAIL_CHARGE_TIME_CHARM (0.75)` | `HC:832-838`, `HC:5473-5479` | not equipped ⇒ **`nailChargeTime = 1.35`** (`hero.json#nailChargeTime` confirms 1.35) |
| 4 | `RECOIL_DURATION_STAL (0.08)`, `INVUL_TIME_STAL (1.75)` | `HC:5172`, `HC:3821-3828` | not equipped ⇒ **0.2 / 1.3** |
| 14 | blocks `RecoilLeft`/`RecoilRight` (not the `…Long` variants) | `HC:2262,2275` | not equipped ⇒ horizontal recoil active |
| 23 / 27 | `maxHealth + 2` / Joni's (maxHealth = 1, blue = 1.4×) | `HC:5481-5493` | not equipped ⇒ **maxHealth 9** |
| 40 | `carefreeShieldEquipped` (requires `grimmChildLevel == 5`) | `HC:5503-5510` | not equipped |

Charm **names** are not in the decompiled assemblies (grep for a charm-name map returns only
`GameManager.StoryRecord_charmEquipped(string)` `GameManager.cs:1167`); they live in the
localisation data. Charms are therefore identified by ID + effect only. See Q-hero-7.

---

## 6. Open questions

Format per the P1 consolidation script: one `### Q-hero-<n>` heading + one paragraph.
`analysis/open-questions.md` is owned by the orchestrator and is not edited from here.

### Q-hero-1 — `WALLSLIDE_DECEL = 0` makes the wall-slide ramp degenerate
`HC:1124-1142` runs two blocks: `if (v.y > WALLSLIDE_SPEED) { v.y -= WALLSLIDE_DECEL; if (v.y < WALLSLIDE_SPEED) v.y = WALLSLIDE_SPEED; }` then `if (v.y < WALLSLIDE_SPEED) { v.y += WALLSLIDE_DECEL; if (v.y < WALLSLIDE_SPEED) v.y = WALLSLIDE_SPEED; }`. With `WALLSLIDE_DECEL = 0` (`hero.json#WALLSLIDE_DECEL`) the first block is a no-op and the second clamps `v.y` **up** to `−8.0` whenever it is below it, so a wall slide caps fall speed at −8 instantly and never *decelerates* a slower descent — the ramp the constant names does not exist. That is what the code says; it is unverified because the corpus contains no wall-slide frame. Settled by: a corpus entry that holds a direction into a wall while airborne in `GG_Hornet_1`, then checking `v.y` per FixedUpdate against both readings.

### Q-hero-2 — motion paths with zero trace coverage
Zero of the 1800 FRAME records in the four r2 traces exercise `cState.wallSliding`, `hero_state ∈ {grounded(0), wall_sliding(4), hard_landing(5), dash_landing(6)}`, `recoilingLeft/Right`, `casting`/`castRecoiling`/`focusing`, `superDashing`, `bouncing`/`shroomBouncing`, `swimming`, `onConveyor*`, `slidingLeft/Right`, or the `hero.f` bools `wallLocked`, `wallJumpedL/R`, `wallSlidingL/R`, `hardLanded`, `recoilLarge`, `dashingDown` (measured: all false in all 1800). Every rule for those in §3 is a code port with no empirical confirmation, so the §3 "Verified" lines cover run / jump / double jump / dash / damage-recoil / gravity / fall-clamp and nothing else. Super-dash and dream-nail charge *are* present in the corpus, but only as the `acceptingInput = false` lockout of §3.2b plus `cState.freezeCharge` — never as `cState.superDashing`. Settled by: corpus entries per path (wall approach + hold direction; a fall longer than `BIG_FALL_TIME = 1.1`; attack into terrain for horizontal recoil; hold cast / superdash), then re-running the §3 verification table over them.

### Q-hero-3 — `ActorStates.wall_sliding` (4) is never assigned
grep over `HeroController.cs` finds no write of `ActorStates.wall_sliding`; `SetState` (`HC:3616-3633`) has no case for it and no caller passes it. Wall sliding is tracked only by `cState.wallSliding`. If some FSM sets `hero_state` through a different route (e.g. a PlayMaker `SetProperty` on the field) the sim will mis-model wall slides and every `hero_state != …` conjunct in §4. Settled by: grep for `ActorStates.wall_sliding` and for `hero_state` writes across the whole decomp and the FSM action dumps, plus a trace that wall-slides.

### Q-hero-4 — the ground-contact separation constant (surface now known, rule still not)
Partly closed by `analysis/dumps/GG_Hornet_1/scene.json`: the terrain under the hero is `Hornet Saver/Colliders`, a `BoxCollider2D` on layer 8 spanning x ∈ [12.6, 40.2] with **bounds top y = 27.0** exactly, material `Terrain`, `isTrigger=false`. With `phys.json#heroColliders[0]` (`offset.y = −0.75`, `size.y = 1.28125`, `edgeRadius = 0.0025`), the measured resting `rb2d.position.y = 28.40812` puts the box bottom at `27.017495` and the edge-radius-extended shape bottom at `27.014995` — i.e. the body rests `0.0175` (box) or `0.0150` (with edge radius) **above** the surface, and the one-step landing target `28.40809` is 3e-5 below that. What remains open is the *rule* generating that gap. It is numerically equal to `2 × defaultContactOffset (0.01) − 0.005`, but Box2D's `linearSlop` is not in any dump and Physics2D has no decomp (`open-questions.md` Q16), so decomposing it that way would be exactly the kind of memory-sourced constant rule 2.2 forbids. Settled by: an oracle run that varies `Physics2D.defaultContactOffset` (and the hero collider's `edgeRadius`) and re-measures the resting y — two points determine whether the gap is `2·offset − slop`, `offset + edgeRadius`, or something else.

### Q-hero-5 — hit-stop is disabled in the oracle, so `DAMAGE_FREEZE_*` are inert
`StartRecoil` yields on `gm.FreezeMoment(DAMAGE_FREEZE_DOWN, DAMAGE_FREEZE_WAIT, DAMAGE_FREEZE_UP, 0.0001f)` (`HC:3829`) between setting `cState.recoilFrozen = true` (`HC:3812`) and `cState.recoiling = true` (`HC:3831`), but `GameManager.FreezeMoment*` is no-op'd by the mod (`docs/frame-order.md`). Measured consequence: `recoilFrozen` is true on exactly 5 FRAMEs across the whole corpus, i.e. one frame per recoil, and `DAMAGE_FREEZE_WAIT = 0.25` never elapses. Whether the sim should model the real freeze depends on whether P8 transfer evaluates against an unmodded build, where the knight would be frozen for ~0.3 s per hit — a large behavioural difference. Settled by: an owner decision on the P8 target build, plus one trace from a build with `FreezeMoment` intact to measure the real `recoilFrozen` length.

### Q-hero-6 — `ActionDecoder`'s commit length is computed from a stale capture-dt constant
`PC:295` hard-codes `const float kCaptureDeltaTime = 0.00848f` and `PC:296-299` derives the hold-lock length as `ceil(hold_seconds / (framesPerWait × kCaptureDeltaTime))`, but R2 pins `Time.captureDeltaTime = 0.02` (`phys.json#Time.captureDeltaTime`, and the trace header `capture.capture_dt = 0.02`). With `frames_per_wait = 2` the nail-charge lock is therefore `ceil(1.5 / 0.01696) = 89` agent steps = 3.56 s of game time, against the 1.5 s the comment at `PC:283` intends — a 2.36× overshoot, and similarly for focus / dream nail / super dash. The sim must reproduce one or the other exactly, since the lock length is what the policy observes through the `commit_*` block of the global state. Settled by: an owner decision. Default: port the code as written (the bug), because that is what the recorded corpus and the trained policies were produced under.

### Q-hero-7 — charm identities for the two equipped IDs with no HeroController effect
`equippedCharm_25` and `equippedCharm_36` are true in both `save#` and `pd.json#` but have no read site in `HeroController.cs` (grep), so whether they change hero motion at all is unknown; and charm *names* are absent from all three decompiled assemblies (the only hit is `GameManager.StoryRecord_charmEquipped(string charmName)` at `analysis/decomp/Assembly-CSharp/GameManager.cs:1167`), so charms can only be identified by ID + effect. Settled by: grep for `equippedCharm_25` / `equippedCharm_36` across the full decomp **and** the FSM action dumps (their effects are likely FSM-side, e.g. on `Knight/Charm Effects`), plus the localisation table for the names.

### Q-hero-8 — the Knight's own FSMs own super-dash, spell, focus and nail-art motion
`SuperDash()` (`HC:1531-1533`), `BackDash()` (`HC:1523-1525`) and `ShadowDash()` (`HC:1527-1529`) are **empty bodies**, and `cState.casting`, `focusing`, `superDashing`, `freezeCharge` are written only through `HeroController.SetCState(name, value)` (`HC:2853-2856`) by PlayMaker. §3.2c names the boundary — 12 FSMs on the `Knight` root in `analysis/fsm/GG_Hornet_1.json`, of which `Superdash` (28 states), `Spell Control` (98), `Nail Arts` (35) and `Dream Nail` (47) are motion-relevant — but their *contents* are the FSM port's, not this spec's. They also drive the `acceptingInput = false` lockout of §3.2b (measured: 139/480 FRAMEs in r2_rand2, 65/480 in r2_rand1), which pins `v.x` because `Move()` is skipped at `HC:1255`. Settled by: the FSM worker enumerating, for those four FSMs, every action that writes `rb2d.velocity`, calls `SetCState`, or calls `IgnoreInput` / `AcceptInput` / `RelinquishControl` / `RegainControl`; the hero port then consumes those as external writes.

### Q-hero-9 — `Time.deltaTime` read from a FixedUpdate context (`Dash` only)
`Dash()` accumulates `dash_timer += Time.deltaTime` (`HC:1520`) while being called from `HeroController.FixedUpdate` (`HC:1029`) — the only such site in the class. (An earlier draft also listed `CheckForTerrainThunk`; that was wrong. `CheckForTerrainThunk` is an `IEnumerator` (`HC:4358`) that `yield return null`s once per loop iteration (`HC:4423`), so its `thunkTimer -= Time.deltaTime` at `HC:4421` is a per-frame countdown in the coroutine phase, not a fixed-step one.) Under R2 `captureDeltaTime = fixedDeltaTime = 0.02` so the two candidate values coincide and the trace shows exactly 0.02 per FixedUpdate; the ambiguity is invisible here and would only matter under a regime where they differ. Settled by: re-measuring `dash_timer` increments in a trace with `captureDeltaTime ≠ fixedDeltaTime` — `analysis/traces/p0/dt01_seed.a.hktrace` and `base_seed.a.hktrace` may already answer it without a new capture.

### Q-hero-10 — engine semantics asserted nowhere in `analysis/`
Several sentences in §1–§2 describe Unity/Box2D/InControl *mechanism* rather than a measured outcome, and no decomp line, dump key or trace record establishes them: that the integrator is symplectic rather than something else that happens to agree on these frames; that the ≤2-frame settle to `28.40812` is Baumgarte positional correction; that `collisionDetectionMode = Continuous` is *why* a −20 landing resolves in one step; that `velocityThreshold = 1.0` is *why* there is no rebound; that `queriesHitTriggers = true` is why `CheckTouchingGround` would accept a trigger on layer 8; what `queriesStartInColliders = false` does to a ray starting inside the hero's own bounds; and that the damage path runs "inside the physics step" rather than merely between `HC_FIXED_POST` and `HC_UPDATE_PRE`. Each is marked `[engine-assumption → Q-hero-10]` in place; each is an outcome the sim can reproduce from the measurement alone, so none blocks P3, but a wrong *mechanism* will mispredict the first case outside the corpus. This is the hero-side instance of `analysis/open-questions.md` Q16 (Physics2D is native, has no decomp, must not be assumed from memory). Settled by: targeted oracle experiments that vary one setting at a time (`defaultContactOffset`, `collisionDetectionMode`, `velocityThreshold`, `queriesHitTriggers`) and re-measure, per Q16's own instruction to pin the solver by experiment.

### Q-hero-11 — the hero `Rigidbody2D.interpolation` the sim should port
`phys.json#rb2d.interpolation` reads `Interpolate`, because the dump run did not set `HK_ORACLE_NOINTERP=1`; regime R2 sets it to `None` on the hero and every `HealthManager` body from `sceneLoaded` and again at `SceneReady` (`oracle/Oracle/RegimeTweaks.cs:61-75`, armed at `:30-36`), and the r2 traces were captured with it (`STATE.md`, R2 definition). §1.4 records that this spec ports **`None`**, so `transform.position == rb2d.position` and no accumulator residual reaches game logic. This is only safe as long as the training regime keeps the flag; if the owner reverts it, every boss FSM that reads `transform.position` sees `rb2d.position + v·(Time.time − Time.fixedTime)` and the hero port would need an interpolated transform alongside the physics position. Ties to `analysis/open-questions.md` Q13 and review items C10 / G12. Settled by: the owner's training-regime decision being recorded, plus a re-dump with `HK_ORACLE_NOINTERP=1` so the dump and the traces stop disagreeing.

### Q-hero-12 — which callback drives InControl, and how many commit ticks occur per frame
`InControlManager.updateMode` is a public serialized field with no initializer (`ICM:19`) and is in no dump, so whether `InputManager.UpdateInternal()` runs from `InControlManager.Update` (`ICM:86-92`) or `InControlManager.FixedUpdate` (`ICM:94-100`) is unknown from `analysis/`. This sets how often `lastState` rotates in `OneAxisInputControl` (`OAIC:264-280`, `:334-347`) and therefore when `WasPressed` / `WasReleased` edges are visible to `LookForQueueInput`. The trace does not narrow it: `analysis/specs/frame-order.md` §2 (the `InputDeviceShim.Update` row) and §4 ("What is and is not established about the InControl sample") bound the sample only to the open interval *(`ApplyAction` in frame F's coroutine phase, `HeroController.Update` in frame F+1)*, and that spec states "InControl samples in the Update phase" is **not** established (its Q-frame-3). What is measured is the consequence — the one-FixedUpdate input lag, verified 1463/1463 in §2.4 — so the sim can reproduce the observed behaviour with one commit per Update; but a frozen frame (`timeScale = 0`) also satisfies the second disjunct of `ICM:88`, which would double-tick under the `FixedUpdate` setting. Settled by: a reflection dump of the `InControlManager` component (add `updateMode` to the dumper), or a trace field recording `InputManager.CurrentTick` per FRAME.

### Q-hero-13 — `fsm_orbitShield` target is not determined by the decomp
`fsm_orbitShield` (`HC:699`) is a plain inspector-assigned `PlayMakerFSM` field with no `FSMUtility.LocateFSM` binding in `Start`/`SetupGameRefs` (unlike `superDash` `HC:815`, `fsm_thornCounter` `HC:820`, `spellControl` `HC:830`, `proxyFSM` `HC:5015`), so which FSM receives its `SLASH` event (`HC:1503`) cannot be read from the decomp. Candidates in `analysis/fsm/GG_Hornet_1.json` are `Knight/Charm Effects` / `Spawn Orbit Shield` (6 states) and `_GameManager/GlobalPool/Orbit Shield(Clone)` / `Control` (7 states). Low priority: the send is guarded by `equippedCharm_38`, which is false in this save (§5), so the edge is unreachable in the current corpus. Settled by: widening the reflection dumper to record `HeroController`'s `PlayMakerFSM`-typed fields by `gameObject` path + `Fsm.Name` (it currently records primitives only).

---

## Review fixes

Applied against `analysis/specs/REVIEW-p1.md` (2026-08-31). Only this file was edited.

| id | defect | change |
|---|---|---|
| D21 | `hero.json#attackDuration` is 0.0, not 0.28 — it is assigned only inside `Attack()` and the hero is idle at dump | §3.3: removed from the constants table; replaced by a note that `attackDuration` is mutable state, that the dump captures a stale 0, and that the sim must take 0.28 from the code path (`charm32 ⇒ ATTACK_DURATION_CH`, `HC:1330-1337`), cross-cited to `analysis/specs/damage-path.md` §6.2 and to the 15-tick trace measurement. Also resolves **C14**. |
| D22 | "`attackDuration` … decremented in Update" — nothing is decremented | §3.3 rewritten to the real mechanism: `attack_time += Time.deltaTime; if (attack_time >= attackDuration) ResetAttacks()` (`HC:5200-5208`), with `ResetAttacks` zeroing `attack_time` (`HC:4118`). `attack_cooldown` is the value that counts down (`HC:5357-5360`), now cited separately. |
| D23 | dash `dashingDown` / `dashCooldownTimer` lines paired backwards | §3.3 and the §5 charm-31 row: `dashingDown` condition → `HC:3539-3543` (else-branch `HC:3545-3550`); `dashCooldownTimer` → `HC:3551-3558`. |
| D24 | "908 − 600 = 308 pause frames" mixes `HC_UPDATE` and `FRAME` counts | §2.1: now **302** (`908 HC_UPDATE_PRE − 606 FIXED`), with the 303-frozen-frames-by-`frameCount` figure and the `SCENE_READY` frame that accounts for the difference, cross-cited to `analysis/specs/frame-order.md` §1.2. Resolves **C2**. |
| D25 | Q9 misread `CheckForTerrainThunk` as a fixed-step context | Q-hero-9 rewritten: it is an `IEnumerator` (`HC:4358`) that `yield return null`s per loop iteration (`HC:4423`), so `HC:4421` is a per-frame countdown; `Dash()` (`HC:1520`, called from `HC:1029`) is the only FixedUpdate-context `Time.deltaTime` read. The same correction is now inline in §2.3. |
| D26 | two over-strong "only" claims | §1.1: `AffectedByGravity` is no longer "the only gravity control" — `EnterAcid`/`ExitAcid` (`HC:4289`, `HC:4297`) write `gravityScale` directly, both `[Obsolete]` (`HC:4286`, `HC:4294`) with no call site, so it is the only *reachable* path. §1.3: the "collision callbacks are the only writers" sentence is replaced by an exhaustive per-flag writer table naming the `BackOnGround()` calls from `orig_Update` (`HC:5156`, `HC:5165`) and `FailSafeChecks` (`HC:3954`, `HC:3990`) plus the six scene-entry direct writes. |
| D27 | initialiser count | Header: 23 initialisers (19 at `HC:183-219`, plus `HC:233`, `HC:713`, `HC:723`, and the `const` at `HC:719`) ⇒ **324 of 347**. |
| D28 | ~15 uncited engine/Unity/Box2D/InControl sentences | Added an `[engine-assumption → Q-hero-10]` convention in the header plus the new Q-hero-10. Reworked in place: the integrator is now an H1-vs-H2 hypothesis pair discriminated by the trace (404/404 for semi-implicit, explicit Euler off by exactly `g·dt²`); the Baumgarte and Continuous-CCD *causal* claims are marked as assumptions with the measurement retained; the no-bounce claim leads with the 13 measured landings; `queriesHitTriggers` is separated from what `HC:4614` actually inspects; a `queriesStartInColliders` note covers rays starting inside the hero's bounds; the "inside the physics step" attribution for the damage path is downgraded to the trace's actual bound (cross-cited to `analysis/specs/frame-order.md` §1.4). The coroutine-resume slot and `Time.deltaTime = 0` on frozen frames now cite the `analysis/specs/frame-order.md` measurements (§1.4, §2 "Coroutine resume", §3.2) instead of engine rules; "FixedUpdate precedes Update" cites the 0-inversions measurement (`analysis/specs/frame-order.md` §1.2, §2, §4); the `Time.deltaTime`-in-FixedUpdate claim is replaced by the measured 0.02 increment. InControl: `updateMode` is UNKNOWN → Q-hero-12 rather than "default", and the commit-tick rotation cites `OAIC:264-280` / `:334-347`. |
| D28 (0.7071) | the ±0.7071 `move_input` was attributed to `moveVector`'s deadzone — wrong mechanism | §2.4 rewritten from source. `PlayerTwoAxisAction.LowerDeadZone`/`UpperDeadZone` are `[Obsolete]` **no-op** setters (`PlayerTwoAxisAction.cs:23-45`, quoted verbatim), so `HA:75-77` does nothing; the constructor sets `Raw = true` (`:57`), so `TwoAxisInputControl.UpdateWithAxes` skips `DeadZoneFunc` entirely (`:181`). The normalisation is in the **device**: `InputDevice.Commit` → `ProcessDPad` (`InputDevice.cs:708-714`, `:681-706`) takes the deadzone branch because the shim uses `UpdateWithState` (`PC:104-107`; `OAIC:286-292` never sets `Raw`), applies `DeadZone.Separate` (`InputDevice.cs:414`; `DeadZone.cs:16-26` divides by the vector length) and writes the normalised components back onto the digital controls (`InputDevice.cs:702-705`). Reached because `InputDevice.IsKnown => true` (`InputDevice.cs:141`), unoverridden by the shim. |
| D29 | `rb2d.interpolation` not mentioned | §1.4: new row plus a paragraph — the dump says `Interpolate`, R2 sets `None` (`oracle/Oracle/RegimeTweaks.cs:61-75`, armed `:30-36`); **the spec ports `None`** and states why. New Q-hero-11. Resolves **C10** and **G12**. |
| C4 | dump-regime staleness | Header: dump-provenance block added (`meta.json` `timestampUtc 2026-08-31T02:49:29Z`, `frameCountAtDump 29672`, `timeScaleAtDump 0.0`, captured under R2 with `captureDeltaTime = 0.02`), plus the warning that fields assigned only while an action is in flight read their idle value — which is exactly D21. |
| G2 | nobody named the Knight's own FSMs | New §3.2c: the four FSM handles HeroController caches (`proxyFSM`/`ProxyFSM` `HC:5015`, `superDash`/`Superdash` `HC:815`, `spellControl`/`Spell Control` `HC:830`, `fsm_thornCounter`/`Thorn Counter` `HC:820`) with state counts and `restartOnEnable` from `analysis/fsm/GG_Hornet_1.json`; an exhaustive table of every `SendEvent` HeroController raises and the motion path that raises it; the three reverse channels (`SetCState`, the `acceptingInput` calls, the public velocity pokes); and the remaining hero-root FSMs named. Scope is explicitly handed to the FSM port. Q-hero-8 rewritten to match; new Q-hero-13 covers the one handle whose target the decomp does not determine. |
| G7 / Q4 | ground-contact geometry now partly determinable from `scene.json` | Q-hero-4 rewritten: `scene.json` gives the terrain under the hero (`Hornet Saver/Colliders`, layer 8, bounds top **y = 27.0**), so the resting gap is now a number (0.0175 box / 0.0150 with `edgeRadius`) rather than an unknown; what stays open is only the *rule* generating it, and the tempting `2·contactOffset − linearSlop` decomposition is explicitly refused as memory-sourced (rule 2.2, Q16). A `queriesStartInColliders` note was added to §1.4 for rays that start inside the hero's own bounds. |
| R2-6 | cross-spec citations into `analysis/specs/frame-order.md` / `analysis/specs/damage-path.md` drifted when those specs were revised in the same pass | Every cross-spec reference re-pointed to a **section heading + quoted anchor text** (which survives edits), with the current line range kept only as a parenthetical hint: `analysis/specs/frame-order.md` §1.2 "Two frame classes", §1.4, §2 "Coroutine resume after `yield return null`", §2 `InputDeviceShim.Update` row, §2 `RecorderBehaviour.FixedUpdate` row, §3.2 "The frozen frame", §3.2 "`HeroController.Update` still runs", §4 "Why exactly one FixedUpdate of delay", §4 "What is and is not established about the InControl sample"; `analysis/specs/damage-path.md` §6.2. **Two substantive corrections fell out of the re-read**, not just re-pointing: (a) §2.4 and Q-hero-12 claimed the InControl device sample "lands in the Update phase" — the revised `analysis/specs/frame-order.md` §4 now states that is **not** established and bounds it only to the open interval *(`ApplyAction` at F, `HeroController.Update` at F+1)* (its Q-frame-3), so this spec now carries the interval and keeps only the regime-independent consequence (the one-FixedUpdate lag, still 1463/1463); (b) §2.2 said `Time.deltaTime = 0` on the frozen frame was "measured directly from the `FRAME.dt` field" — there is no `FRAME` on a frozen frame, so it now cites the accumulation-identity derivation (599/599, 479/479) and notes that the `dt = 0.02`-after-the-`timeScale`-write argument is circular (its Q-frame-4). |
| — | Q format | All open questions renumbered to `### Q-hero-<n> — <title>` + one paragraph, per the coordinator's consolidation script; `analysis/open-questions.md` untouched. New entries: Q-hero-10 (engine assumptions), Q-hero-11 (interpolation), Q-hero-12 (InControl driver), Q-hero-13 (`fsm_orbitShield`). |
