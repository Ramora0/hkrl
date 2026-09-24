# tk2d sprite animator — semantics, hero control, PlayMaker coupling, collider coupling

P1.5. Every claim cites `analysis/decomp/<Assembly>/<path>:<line>`, a dump under `analysis/dumps/`,
or a trace under `analysis/traces/`. Unverified statements are marked UNVERIFIED and repeated in §6.

Decomp paths below are relative to `analysis/decomp/Assembly-CSharp/`.
Oracle paths are relative to the repo root.

---

## 1. Frame advance semantics

### 1.1 Which callback advances it

`tk2dSpriteAnimator.LateUpdate()` — `UpdateAnimation(Time.deltaTime)`
(`tk2dSpriteAnimator.cs:586-589`). "Unity dispatches `LateUpdate` at most once per rendered frame"
is an **[ENGINE]** assumption (Q-tk2d-7), not derivable from `analysis/`; what is measured here is
that advancing the model by **exactly one** `UpdateAnimation` per `FRAME` record reproduces
3600/3600 records (§1.6) — which is what the port needs. It is `public virtual`; the only override in the assembly is
`tk2dUISpriteAnimator.LateUpdate()`, which feeds `tk2dUITime.deltaTime` instead
(`tk2dUISpriteAnimator.cs:6-9`). No gameplay animator in the dumps is a `tk2dUISpriteAnimator`
(`analysis/dumps/GG_Hornet_1/{physics,bosses}.json`: `animator.type == "tk2dSpriteAnimator"`).

There is no `Update`/`FixedUpdate` on the animator. `Start()` calls `Play(DefaultClip)` iff
`playAutomatically` (`tk2dSpriteAnimator.cs:193-199`); `OnEnable()` only disables the component when
no `tk2dBaseSprite` is present (`:185-191`).

**Position in the frame (measured).** The mod's `FRAME` record is written when the step coroutine
resumes after `yield return null` (`docs/trace-format.md`, capture points). In `analysis/traces/p0/*`:

* A clip started by `HeroAnimationController.Update` in frame N appears in frame N's `FRAME` record at
  its *initial, unadvanced* `clip_time` — e.g. `PlayFromFrame("Airborne", 0)` shows exactly
  `6.25e-05 == (0 + 0.001f)/16` at `analysis/traces/p0/r2_move.a.hktrace` record 241 / `Time.frameCount` 25125; likewise
  `0.3125625 == (5+0.001)/16` for frame 5 and `0.1875625` for frame 3.
* Simulating **exactly one** `UpdateAnimation(dt)` per `FRAME` record reproduces the next record's
  `(clip_time, CurrentFrame, Playing)` for **3600/3600 records** (hero + Hornet over the four
  `analysis/traces/p0/r2_*.hktrace` corpora — per-trace counts and method in §1.6).

⇒ order within a rendered frame is `HeroAnimationController.Update` → … → mod coroutine `FRAME`
capture → `tk2dSpriteAnimator.LateUpdate`. This closes the "tk2d animator position" half of Q4.

### 1.2 The accumulator

```
UpdateAnimation(deltaTime):                       // tk2dSpriteAnimator.cs:433-526
  if ((state | globalState) != State.Playing) return;    // :435   (Playing && !Paused && !g_Paused)
  clipTime += deltaTime * clipFps;                       // :439   float32 throughout
  <wrap-mode dispatch, :441-525>
```

* `state` is a flag enum `{Init=0, Playing=1, Paused=2}` (`:7-12`, field `:36`). The guard at `:435`
  is an equality against `Playing`, so *any* extra bit (Paused, or the static `globalState`, `:22`)
  suppresses the whole update. `globalState` is only written through
  `tk2dSpriteAnimator.g_Paused` (`:40-50`), whose sole caller in the assembly is the **static**
  property `tk2dAnimatedSprite.g_paused` (`tk2dAnimatedSprite.cs:61-71`), on the obsolete
  component. The *instance* property `tk2dAnimatedSprite.Paused` (`tk2dAnimatedSprite.cs:73-77`)
  proxies `Animator.Paused` and does not touch `globalState`. The component name
  `tk2dAnimatedSprite` occurs 0 times in the component lists of
  `analysis/dumps/GG_Hornet_1/hero.json`, `analysis/dumps/GG_Hornet_1/bosses.json` and the three
  sibling scenes, so nothing dumped can reach either ⇒ in practice `globalState == Init`.
* `clipFps` is the *effective* fps: `Play` sets it to `overrideFps > 0 ? overrideFps : clip.fps`
  (`:295`), and `DefaultFps` is `0f` (`:144`), so the ordinary `Play(clip)` path uses `clip.fps`.
  `ClipFps` setter clamps a non-positive value back to `currentClip.fps` (`:129-142`).
* `ClipTimeSeconds` (what the trace records) is `clipTime / (clipFps > 0 ? clipFps : currentClip.fps)`
  (`:117-127`). So `clipTime` is in **frames**, `ClipTimeSeconds` in **seconds**.
* Frame index selection truncates: every wrap mode casts `(int)clipTime` (`:146-183`, `:441-525`).
  "C# `(int)` on a `float` truncates toward zero" is an **[ENGINE/LANG]** assumption (Q-tk2d-7), but
  the data discriminates it: at `Turn` LateUpdate 4 (§1.6) `clipTime = 1.5999999` and the trace
  reports `CurrentFrame = 1`; round-to-nearest would give 2, out of range for `n = 2`. All 3600
  records are consistent with truncation and with no other rounding rule.

**dt = 0 frames are exact no-ops.** With `deltaTime == 0`: `clipTime` is unchanged, `SetFrameInternal`
sees `previousFrame == currFrame` and skips `SetSprite` (`:551-558`), and `ProcessEvents` returns on
`start == last` (`:562-565`). Measured: under regime R2 the trainer pauses between agent steps with
`Time.timeScale = 0`, "which gives deltaTime = 0 even with capture"
(`oracle/Environment/TrainingEnv.cs:376`; writes at `:161,373,492,596,627`). In
`analysis/traces/p0/r2_move.a.hktrace` consecutive `FRAME` records differ in `Time.frameCount` by 1
(×300) or 2 (×299) while `Time.time` always advances by one `dt` (0.0199966–0.0200005) and every
recorded `Time.deltaTime` is 0.02 with `timeScale == 1`
(`analysis/traces/p0/r2_move.a.hktrace`, all 600 `FRAME` records) — i.e. the unrecorded interleaved
frames contribute zero game time, and the animator does not advance on them.

That `Time.deltaTime` is the **scaled** delta (`timeScale × captureDeltaTime` under a pinned capture)
is an **[ENGINE]** assumption (Q-tk2d-7); the evidence for it here is
`oracle/Environment/TrainingEnv.cs:376` ("`Time.timeScale = 0`, which gives deltaTime = 0 even with
capture") plus the game-time accounting measured above. `unscaledDeltaTime` in
`analysis/traces/p0/r2_move.a.hktrace` is raw wall clock (0.3–11 ms in batchmode) and the animator
never reads it (`tk2dSpriteAnimator.cs:586-589`).

### 1.3 Wrap modes

`tk2dSpriteAnimationClip.WrapMode = {Loop=0, LoopSection=1, Once=2, PingPong=3, RandomFrame=4,
RandomLoop=5, Single=6}` (`tk2dSpriteAnimationClip.cs:7-16`). Clip fields: `name, frames[], fps=30,
loopStart, wrapMode` (`:18-26`); `Duration => frames.Length / fps` (`:28`).

`n = frames.Length`, `k = (int)clipTime`, `ls = loopStart`, `prev = previousFrame`.

| WrapMode | `CurrentFrame` (`:146-183`) | `UpdateAnimation` (`:441-525`) | end behaviour |
|---|---|---|---|
| `Loop` | `k % n` (`:154-156`) | `f = k % n`; `SetFrameInternal(f)`; if `f < prev` → `ProcessEvents(prev, n-1, +1)` then `ProcessEvents(-1, f, +1)`, else `ProcessEvents(prev, f, +1)` (`:443-457`) | never stops |
| `RandomLoop` | same as `Loop` (`:154-156`) | same branch as `Loop` (`:443-444`) | never stops; start frame is `Random.Range(0, n)` (`:309-312`) |
| `LoopSection` | `k >= ls ? ls + (k-ls) % (n-ls) : k` (`:157-166`) | intro (`k < ls`) plays straight; from `ls` onward it cycles `[ls, n-1]`; three event cases for entering the section / wrapping inside it (`:459-488`) | never stops |
| `Once` | `Mathf.Min(k, n)` — **`n`, not `n-1`** (`:152-153`) | if `k >= n`: `SetFrameInternal(n-1)`, clear `Playing`, `ProcessEvents(prev, n-1, +1)`, `OnAnimationCompleted()` (`:506-515`); else `SetFrameInternal(k)`, `ProcessEvents(prev, k, +1)` (`:516-520`) | **last sprite holds**, `Playing → false`, `previousFrame → -1` (`:579`) |
| `PingPong` | `j = n>1 ? k % (2n-2) : 0`; if `j >= n` → `2n-2-j` (`:167-175`) | same index math; `direction = -1` on the return leg or when `j < prev`; `ProcessEvents(prev, j, direction)` (`:489-505`) | never stops |
| `RandomFrame` | falls through `default` → logs "Unhandled clip wrap mode" and uses the `Loop` formula (`:176-181`) | **empty case — no advance at all** (`:523-524`) | frozen on the frame `Play` picked; `Playing` already cleared at `Play` time (`:313-317`) |
| `Single` | `0` (`:176-177`) | not reachable: `Play` warps to 0 and clears `Playing` (`:304-308`) | static single frame; `CopyFrom` truncates such clips to 1 frame (`tk2dSpriteAnimationClip.cs:75-79`) |

Note the `Once` asymmetry: after completion the *displayed sprite* is index `n-1` but `CurrentFrame`
returns `n`, an out-of-range index. This is load-bearing for `anim_phase` (§5).

**Wrap-mode census over the dumped libraries** (`analysis/dumps/*/bosses.json`,
`analysis/dumps/GG_Hornet_1/physics.json`):

| library | Loop | LoopSection | Once | PingPong | Single | RandomFrame | RandomLoop |
|---|---:|---:|---:|---:|---:|---:|---:|
| Knight (214 clips) | 37 | 51 | 120 | 0 | 6 | 0 | 0 |
| Hornet Boss 1 (61) | 6 | 9 | 46 | 0 | 0 | 0 | 0 |
| False Knight New (37) | 9 | 5 | 21 | 0 | 2 | 0 | 0 |
| Giant Fly (17) | 5 | 2 | 8 | 2 | 0 | 0 | 0 |
| Mega Moss Charger (14) | 6 | 1 | 7 | 0 | 0 | 0 | 0 |

`Loop / LoopSection / Once` cover 333/343 clips. The two `PingPong` clips are Giant Fly `Death`
(20 fps, 4 frames) and `Wiggle` (12 fps, 3 frames) (`analysis/dumps/GG_Gruz_Mother/bosses.json`).
Knight `Single` clips (`analysis/dumps/GG_Hornet_1/physics.json`): `Death Head Normal`,
`Death Head Cracked`, `Sitting Asleep`, `Prostrate`, `SD Crys Idle`, `Spike Death Antic`.
**No `RandomFrame` / `RandomLoop` clip exists in any dumped library** ⇒ the animator consumes no
`UnityEngine.Random` draws for these five entities (the only `Random.Range` in the animator is the
random-start path `:311`).

Watch out for junk `loopStart` on non-LoopSection clips: Hornet `Fall` is `Loop, n=4, loopStart=4`
(`analysis/dumps/GG_Hornet_1/bosses.json`); `loopStart` is read only by the `LoopSection` branches, so
it is ignored there.

### 1.4 Play / stop / pause API

`Play(clip, clipStartTime, overrideFps)` is the single entry point (`:291-340`); every other overload
funnels into it.

```
Play(clip, t0, fpsOverride):                                        // :291
  if clip == null: LogError; OnAnimationCompleted(); state &= ~Playing; return   // :334-339
  fps = fpsOverride > 0 ? fpsOverride : clip.fps                                 // :295
  if t0 == 0f && IsPlaying(clip):  clipFps = fps; return          // NO RESTART   // :296-300
  state |= Playing; currentClip = clip; clipFps = fps                            // :301-303
  if wrap == Single || clip.frames == null:  Warp(clip, 0);   state &= ~Playing  // :304-308
  elif wrap == RandomFrame || RandomLoop:
        r = Random.Range(0, n); Warp(clip, r)                                     // :309-312
        if wrap == RandomFrame: previousFrame = -1; state &= ~Playing             // :313-317
  else:
        x = t0 * clipFps                                                          // :321
        if wrap == Once && x >= clipFps * n:  Warp(clip, n-1); state &= ~Playing  // :322-326
        else:                                 Warp(clip, x); clipTime = x         // :329-331
```

`WarpClipToLocalTime(clip, time)` (`:538-549`) sets `clipTime = time`, does `SetSprite` for frame
`(int)time % n`, **fires `AnimationEventTriggered` if that frame has `triggerEvent`**, and sets
`previousFrame` to it. So a `Play` can emit an animation event for its landing frame.

| call | effect | cite |
|---|---|---|
| `Play()` | `currentClip ?? DefaultClip`, then `Play(currentClip)` | `:226-233` |
| `Play(string)` | `library.GetClipByName`; on miss `LogError` **and `Play(null)`** → complete-callback + stop | `:235-238`, `:210-224`, `:334-339` |
| `Play(clip)` | `Play(clip, 0f, DefaultFps=0)` ⇒ restart only if not already playing that clip | `:240-243`, `:144` |
| `PlayFromFrame(int)` | `PlayFromFrame(currentClip, frame)` | `:245-252` |
| `PlayFromFrame(clip, f)` | `PlayFrom(clip, ((float)f + 0.001f) / clip.fps)` — **the 0.001 epsilon is observable** | `:259-262` |
| `PlayFrom(clip, t0)` | `Play(clip, t0, DefaultFps)`; because `t0 != 0` for any `PlayFromFrame`, the no-restart guard never applies ⇒ always restarts | `:286-289`, `:296` |
| `Stop()` | `state &= ~Playing`. Leaves `clipTime`, `currentClip`, `previousFrame` untouched | `:342-345` |
| `StopAndResetFrame()` | `SetSprite(frames[0])` then `Stop()` | `:347-354` |
| `Pause()` / `Resume()` | set/clear the `Paused` bit; `clipTime` frozen, `CurrentFrame` still readable | `:401-409`, `:52-69` |
| `IsPlaying(name|clip)` | `Playing && CurrentClip != null && (name/ref match)` — **false once a `Once` clip has completed** | `:356-372` |
| `SetFrame(f, triggerEvent=true)` | `SetFrameInternal(f % n)`; if `triggerEvent && f >= 0` also `ProcessEvents(f%n - 1, f%n, +1)`. Does **not** touch `clipTime` or `Playing` | `:411-431` |

`PlayFromFrame` epsilon, confirmed against `analysis/traces/p0/r2_move.a.hktrace`
(`hero.anim.clip_time` at the first record of each Airborne segment,
`analysis/traces/p0/r2_move.a.hktrace`): frame 0 → `6.25e-05`, frame 3 → `0.1875625`,
frame 5 → `0.3125625`, i.e. `(f + 0.001)/16` exactly.

### 1.5 Events and delegates

* `tk2dSpriteAnimationFrame` carries `triggerEvent`, `eventInfo` (string), `eventInt`, `eventFloat`
  (`tk2dSpriteAnimationFrame.cs:6-16`).
* `ProcessEvents(start, last, direction)` (`:560-575`) returns immediately when
  `AnimationEventTriggered == null`, `start == last`, or `Mathf.Sign(last-start) != Mathf.Sign(direction)`.
  Otherwise it iterates the **half-open interval `(start, last]`** in `direction` steps and fires
  `AnimationEventTriggered(this, clip, i)` for each frame with `triggerEvent`. `start` is the frame
  *before* this update (`previousFrame`), so events on frames skipped by a large `dt` still fire, and
  the frame you land on fires in the same call — **same frame, not the next**.
* `AnimationCompleted` fires from `OnAnimationCompleted()` (`:577-584`), which also sets
  `previousFrame = -1`. Reached from the `Once` completion (`:514`) and from `Play(null)` (`:337`).
  It fires **in the same `LateUpdate` in which `clipTime` first reaches `n`**, after
  `SetFrameInternal(n-1)` and after that update's `ProcessEvents`.
* Both are plain `Action<>` **fields assigned with `=`, not `+=`**, by every consumer
  (`HutongGames.PlayMaker.Actions/Tk2dPlayAnimationWithEvents.cs:59,63`; `HutongGames.PlayMaker.Actions/Tk2dWatchAnimationEvents.cs:62,66`;
  `HeroAnimationController.cs:126,138,144,150,156`; `NailSlash.cs:98`). Last writer wins, the
  previous handler is silently dropped, and the handler **persists after the FSM state that installed
  it exits** — nothing ever clears them.

**Trigger frames are essentially absent in the fights we care about.** Zero clips with
`triggerEvent` in the Hornet, False Knight, Giant Fly and Mega Moss Charger libraries
(`analysis/dumps/*/bosses.json`). The Knight library (`analysis/dumps/GG_Hornet_1/physics.json`) has exactly 3 of 214:
`Quake Land 2` (frame 7), `Shadow Recharge` (17), `Challenge Start` (7) — all with empty
`eventInfo`, `eventInt = 0`, `eventFloat = 0`
(`analysis/dumps/GG_Hornet_1/physics.json`, `heroAnimator.library.clips`).

### 1.6 Verification against the traces

Model implemented and run (script under the session scratchpad, not committed): seed `clipTime` from
the recorded `ClipTimeSeconds × clip_fps` at each segment start, then per `FRAME` record predict
`(ClipTimeSeconds, CurrentFrame, Playing)` and advance
`clipTime = fl32(clipTime + fl32(fl32(dt) * fl32(clipFps)))` in **IEEE binary32**, clearing `Playing`
when a `Once` clip reaches `(int)clipTime >= n`. Clip metadata from
`analysis/dumps/GG_Hornet_1/{physics,bosses}.json`. A segment ends on a clip-name change *or* a
decrease in `clip_time` (a restart).

Trace column is `analysis/traces/p0/<name>.hktrace`.

| trace | animator | records exact | segments |
|---|---|---:|---:|
| `r2_idle.a` | hero | 240/240 | 1 |
| `r2_idle.a` | Hornet Boss 1 | 240/240 | 9 |
| `r2_move.a` | hero | 600/600 | 47 |
| `r2_move.a` | Hornet Boss 1 | 600/600 | 26 |
| `r2_rand1.a` | hero | 480/480 | 89 |
| `r2_rand1.a` | Hornet Boss 1 | 480/480 | 18 |
| `r2_rand2.a` | hero | 480/480 | 88 |
| `r2_rand2.a` | Hornet Boss 1 | 480/480 | 23 |

**3600/3600, zero mismatches** (`clip_time` within 1e-6, `CurrentFrame` and `Playing` exact).
Criterion `1e-6` on seconds, not bit-exact, only because the seed is re-read from the f32 trace value.
The single 240-record `Idle` segment in `analysis/traces/p0/r2_idle.a.hktrace` is an unbroken 240-step accumulation with no reseed.

Requested worked example, clip metadata from `analysis/dumps/GG_Hornet_1/physics.json` — hero `Run`
(`LoopSection`, 13 frames, `loopStart = 6`, 12 fps) at
`dt = 0.02`, `analysis/traces/p0/r2_move.a.hktrace` records 127-148 (`Time.frameCount` 24954-24986):

| rec | predicted `clip_time` | predicted frame | observed `clip_time` | observed frame |
|---:|---|---:|---|---:|
| 127 | 0 | 0 | 0 | 0 |
| 128 | 0.0199999996 | 0 | 0.0199999996 | 0 |
| 131 | 0.0799999982 | 0 | 0.0799999982 | 0 |
| 132 | 0.099999994 | 1 | 0.099999994 | 1 |
| 136 | 0.179999992 | 2 | 0.179999992 | 2 |
| 140 | 0.25999999 | 3 | 0.25999999 | 3 |
| 144 | 0.340000004 | 4 | 0.340000004 | 4 |
| 148 | 0.419999927 | 5 | 0.419999927 | 5 |

All 22 records match. **Mismatches found: none**, once same-clip restarts are treated as new segments.
(The first pass, which segmented on clip name only, reported 10 "mismatches" across
`analysis/traces/p0/r2_move.a.hktrace` and `analysis/traces/p0/r2_rand1.a.hktrace`;
all 10 were `Play`-induced restarts of the currently playing clip, explained in §2.3 —
they are a hero-controller behaviour, not an accumulator error).

**Float32 accumulation is load-bearing, not cosmetic.** Per
`analysis/dumps/GG_Hornet_1/physics.json`, hero `Turn` is `Once, n = 2, fps = 20`; nominal duration
exactly 0.1 s = 5 steps of `dt = 0.02`. In binary32:

| LateUpdates | `clipTime` | `(int)` | `ClipTimeSeconds` | complete? |
|---:|---|---:|---|---|
| 3 | 1.1999999284744263 | 1 | 0.059999994933605194 | no |
| 4 | 1.5999999046325684 | 1 | 0.07999999821186066 | no |
| 5 | **1.9999998807907104** | 1 | 0.09999999403953552 | **no** |
| 6 | 2.3999998569488525 | 2 | 0.11999998986721039 | yes |

The clip therefore lives **6** rendered frames, not 5. The trace agrees: `analysis/traces/p0/r2_rand1.a.hktrace` record 162 shows
`Turn`, frame 1, `clip_time = 0.1`, `playing = True`. A `double` accumulator would complete at step 5
and desynchronise the FSM by one frame. `deltaTime * clipFps` must also not be contracted into the
following add — a build requirement already in the contract (`docs/float-parity.md:10`,
`-ffp-contract=off -fno-fast-math`), not a claim about Mono's codegen.

---

## 2. Hero animation control

`HeroAnimationController` (`HeroAnimationController.cs`) is on the `Knight` GameObject alongside
`HeroController`, `tk2dSprite`, `tk2dSpriteAnimator`, `BoxCollider2D`, `Rigidbody2D` and **12**
`PlayMakerFSM`s (`analysis/dumps/GG_Hornet_1/hero.json`, `components[0]` — 12 occurrences of
`PlayMakerFSM` in the 29-component list; `analysis/fsm/GG_Hornet_1.json` names all 12 for
`gameObject == "Knight"`: `Map Control`, `Spore Cooldown`, `Spell Control`, `Nail Arts`,
`Superdash`, `ProxyFSM`, `Dream Nail`, `Roar Lock`, `Dream Return`, `Surface Water`, `Globalise`,
`Control Interpolation`).
`Awake` caches `animator = GetComponent<tk2dSpriteAnimator>()` and `cState = heroCtrl.cState` (`:42-47`).
`Update()` calls `UpdateAnimation()` iff `controlEnabled`, else only tracks `wasFacingRight` (`:71-85`).
`controlEnabled` is toggled by `StopControl` / `StartControl` (`:525-539`), exposed on the hero as
`HeroController.StopAnimationControl` / `StartAnimationControl` (`HeroController.cs:3127-3135`).

### 2.1 State → clip (order of evaluation inside one `UpdateAnimation`)

The method is **not** a switch: it is a prologue of one-shot plays, then an if/else-if chain, then an
unconditional facing check. Later `Play` calls in the same frame override earlier ones.

| # | guard | clip played | cite |
|---:|---|---|---|
| P1 | `playLanding` | `Play("Land")`, install `AnimationCompleteDelegate` | `:135-140` |
| P2 | `playRunToIdle` | `Play("Run To Idle")` + delegate | `:141-146` |
| P3 | `playBackDashToIdleEnd` | `Play("Backdash Land 2")` + delegate — **dead code**, see §2.5 | `:147-152` |
| P4 | `playDashToIdle` | `Play("Dash To Idle")` + delegate | `:153-158` |
| 1 | `actorState == no_input` and `cState.recoilFrozen` | `Stun` | `:159-163` |
| 1 | … `cState.recoiling` | `Recoil` | `:165-167` |
| 1 | … `cState.transitioning` | ground: `Run`/`Sprint` (charm 37) on EXITING; `PlayFromFrame("Run",3)`/`Sprint` on ENTERING; air: `PlayFromFrame("Airborne",7)` on EXITING / WAITING_TO_ENTER; ENTERING+`!setEntryAnim`: `Airborne` from 7 (gate top) or 3 (gate bottom) | `:169-227` |
| 2 | `setEntryAnim` | clears the flag, plays nothing | `:230-233` |
| 3 | `cState.dashing` | `dashingDown` → `Shadow Dash Down Sharp` / `Shadow Dash Down` / `Dash Down`; else `Shadow Dash Sharp` / `Shadow Dash` / `Dash` | `:234-269` |
| 4 | `cState.backDashing` | `Back Dash` | `:270-273` |
| 5 | `cState.attacking` | `upAttacking`→`UpSlash`, `downAttacking`→`DownSlash`, `wallSliding`→`Wall Slash`, `!altAttack`→`Slash`, else `SlashAlt` | `:274-296` |
| 6 | `cState.casting` | `Fireball` | `:297-300` |
| 7 | `cState.wallSliding` | `Wall Slide` | `:301-304` |
| 8 | `actorState == idle` | `lookingUpAnim && !IsPlaying("LookUp")` → `LookUp`; `CanPlayLookDown()` → `LookDown`; else if neither look flag and `CanPlayIdle()` → `PlayIdle()` | `:305-319` |
| 9 | `actorState == running` | if `!IsPlaying("Turn")`: `inWalkZone` → `Walk` (guarded by `!IsPlaying("Walk")`), else `PlayRun()` | `:320-336` |
| 10 | `actorState == airborne` | `swimming`→`Swim`; `heroCtrl.wallLocked`→`Walljump`; `doubleJumping`→`Double Jump`; `jumping`→`PlayFromFrame("Airborne",0)`; `falling`→`…5`; else `…3` — the three `Airborne` entries all guarded by `!IsPlaying("Airborne")` | `:337-369` |
| 11 | `actorState == dash_landing` | `Dash Down Land` | `:370-373` |
| 12 | `actorState == hard_landing` | `HardLand` | `:374-377` |
| E | facing changed vs `wasFacingRight`, `cState.onGround`, `canPlayTurn()` | **`Play("Turn")` — unconditional epilogue, overrides everything above** | `:378-393` |
| E | — | `wasAttacking = cState.attacking`; `ResetPlays()` clears P1/P2/P4 | `:394-402` |

Helpers: `PlayIdle()` (`:456-485`) picks `Idle Hurt` at 1 HP without charm 6, `LookUpEnd` /
`LookDownEnd` when the corresponding look clip is playing, `Lantern Idle` when
`heroCtrl.wieldingLantern`, else `Idle`. `PlayRun()` picks `Lantern Run`, `Sprint` (charm 37),
`PlayFromFrame("Run", 3)` if `wasAttacking`, else `Run` (`HeroAnimationController.cs:487-505`).
`CanPlayIdle()` blocks on `Land`, `Run To Idle`, `Dash To Idle`, `Backdash Land`, `Backdash Land 2`,
`LookUpEnd`, `LookDownEnd`, `Exit Door To Idle`, `Wake Up Ground`, `Hazard Respawn` (`:405-412`).
`canPlayTurn()` blocks on `Wake Up Ground`, `Hazard Respawn` (`:423-430`).
`UpdateState(newState)` (`:103-118`) is what arms `playLanding` (airborne→idle) and `playRunToIdle`
(running→idle, unless `inWalkZone` or `attacking`); it is called from `HeroController.cs:3631,4283`,
and `playLanding` is also set directly at `HeroController.cs:4274`;
`FinishedDash()` arms `playDashToIdle` (`:550-553`, called from `HeroController.cs:4234`).

`HeroController` also drives clips directly via `PlayClip` (`:120-130`): `Idle`
(`HeroController.cs:2572`), `Exit Door To Idle` (`:2600`), `Run` (`:2675`), `Wake Up Ground`
(`:2780`), `Hazard Respawn` (`:2826`); and `StopAttack()` → `animator.Stop()` when `UpSlash` or
`DownSlash` is playing (`HeroAnimationController.cs:555-561`, called `HeroController.cs:5206`).

### 2.2 Restart vs continue

`Play(clip)` with `clipStartTime == 0` is a **no-op when that clip is already playing**
(`tk2dSpriteAnimator.cs:296-300`), which is what lets `UpdateAnimation` call `Play("Slash")`
unconditionally every frame. A restart therefore happens only when either

1. `Playing` has gone false (a `Once` clip completed, or something called `Stop()`), or
2. a **different** clip was played earlier in the same `Update` (prologue P1-P4, or the state branch
   before the `Turn` epilogue), so `IsPlaying(clip)` is false by the time the second `Play` runs, or
3. the call was `PlayFromFrame` / `PlayFrom` (`t0 != 0`, guard never applies).

Case 2 is real and frequent — see §2.3. `PlayFromFrame` is also used deliberately to *resume*
mid-clip: `PlayFromFrame("Run", 3)` after an attack (`:499`), `PlayFromFrame("Airborne", 7|5|3|0)`
per air sub-state (`:58,193,206,213,220,224,355,362,367`).

### 2.3 Measured restart mechanism (the "same clip, `clip_time` jumps to 0" case)

`analysis/traces/p0/r2_move.a.hktrace`, records 537→538 (`Time.frameCount` 25569 → 25571):
`Slash` at frame 3 / `clip_time` 0.18 → `Slash` at frame 0 / 0.0, while `cState.attacking` stays
`True` and `attack_time` keeps counting 0.18 → 0.20. Per `analysis/dumps/GG_Hornet_1/physics.json`
`Slash` is `Once, 15 frames @ 20 fps`, duration 0.75 s, so it had not completed. The distinguishing change is
`cState.onGround: False → True` across the unrecorded intervening frame ⇒ `UpdateState(idle)` armed
`playLanding` ⇒ next `UpdateAnimation` ran `Play("Land")` (P1, `:137`) and then `Play("Slash")`
(branch 5, `:290`) in the same call; the second `Play` saw `CurrentClip == "Land"` and restarted
`Slash` from 0. The identical pattern occurs for `SlashAlt` at records 585→586 of the same trace
(`analysis/traces/p0/r2_move.a.hktrace`).

`Turn` restarts (`analysis/traces/p0/r2_rand1.a.hktrace` records 163, 173; `analysis/traces/p0/r2_rand2.a.hktrace` 153, 285, 291, 392) have two distinct
causes, both visible in `analysis/traces/p0/r2_rand1.a.hktrace`: at record 162→163 the previous
frame's clip is `Run To Idle`-armed (P2 runs, then the epilogue `Play("Turn")` restarts it);
at 172→173 the hero is idle, so branch 8 runs `PlayIdle()` → `Play("Idle")` and the epilogue then
restarts `Turn`. Conversely at 170→171 the hero is in `actorState == running`, branch 9
short-circuits on `IsPlaying("Turn")`, no other `Play` runs, and `Turn` **continues**
(`clip_time` 0.06 → 0.08, `analysis/traces/p0/r2_rand1.a.hktrace`). Reproducing hero animation requires
executing `UpdateAnimation` literally, in order, including the calls that are immediately overridden.

### 2.4 Gameplay-relevant hero animation events

* **Frame events: none that matter.** Per `analysis/dumps/GG_Hornet_1/physics.json` only 3 of 214
  Knight clips have `triggerEvent` frames, all with empty payloads (§1.5), and
  `HeroAnimationController` never installs an `AnimationEventTriggered` handler.
* **`AnimationCompleted`:** `HeroAnimationController.AnimationCompleteDelegate` (`:432-454`) calls
  `PlayIdle()` when the completed clip is `Land`, `Run To Idle`, `Backdash To Idle`, `Dash To Idle`,
  or `Exit Door To Idle`. Only three of those five are reachable: `Backdash To Idle` is not in the
  library at all and P3 (its only producer) never runs (§2.5).
* **The nail hitbox is *not* animation-driven.** `NailSlash` enables its `PolygonCollider2D` (and the
  `Clash Tink` poly) on `stepCounter == 1` and disables it at `stepCounter >= 5` — counted in
  `FixedUpdate` (`NailSlash.cs:103-116`). The animation only sets `animCompleted` through
  `anim.AnimationCompleted = Disable` (`:98`, `:155-158`), which together with `polyCounter > 1`
  triggers `CancelAttack()` (`:117-120`). `StartSlash` plays the clip and then forces
  `anim.PlayFromFrame(0)` (`:92`), i.e. always a hard restart (case 3 of §2.2).
* `HeroController.GetClipDuration(name)` / `GetCurrentClipDuration()` return `frames.Length / fps`
  — the **nominal** duration, which §1.6 shows is up to one frame shorter than the realised one
  (`HeroAnimationController.cs:563-581`, used at `HeroController.cs:2599,2779,2825`).

### 2.5 Clip names that do not resolve

**Six clip names referenced by `HeroAnimationController` do not exist in the Knight library.**
Method: every `Play("…")` / `PlayFromFrame("…")` / `IsPlaying("…")` / `PlayClip("…")` string
literal in `analysis/decomp/Assembly-CSharp/HeroAnimationController.cs` was matched against the 214
clip names in `analysis/dumps/GG_Hornet_1/physics.json` (`heroAnimator.library.clips`); 37 names
resolve, these 7 call sites over 6 distinct names do not:

| site | call | name | consequence |
|---|---|---|---|
| `HeroAnimationController.cs:149` | `Play` | `Backdash Land 2` | `Play(null)` path |
| `HeroAnimationController.cs:272` | `Play` | `Back Dash` | `Play(null)` path |
| `HeroAnimationController.cs:299` | `Play` | `Fireball` | `Play(null)` path |
| `HeroAnimationController.cs:341` | `Play` | `Swim` | `Play(null)` path |
| `HeroAnimationController.cs:407` | `IsPlaying` | `Backdash Land` | always `false` |
| `HeroAnimationController.cs:407` | `IsPlaying` | `Backdash Land 2` | always `false` |
| `HeroAnimationController.cs:416` | `IsPlaying` | `Lookup` | always `false` (library has `LookUp`) |

The library does hold `Fireball1 Cast`, `Fireball Antic`, `Fireball2 Cast`, `Surface Swim` — no
exact match for any of the six. The five `HeroController.PlayClip` targets — `Idle`,
`Exit Door To Idle`, `Run`, `Wake Up Ground`, `Hazard Respawn`
(`HeroController.cs:2572`, `:2600`, `:2675`, `:2780`, `:2826`) — all resolve.

**The `Play(null)` path**, `tk2dSpriteAnimator.cs:220,334-339`: `GetClipByNameVerbose` logs
"Unable to find clip" and returns `null`; `Play(null)` logs again, calls `OnAnimationCompleted()`
— which fires whatever handler is *currently* installed, passing the **previous** `currentClip`,
since `Play` never assigns `currentClip` on this path — and then clears `Playing`. Net effect for the
port: a spurious `AnimationCompleted` on the wrong clip, the animator stopped, and therefore a
forced restart of whatever the state branch plays later in the same `UpdateAnimation` (§2.2 case 1).

**P3 is unreachable.** `playBackDashToIdleEnd` is declared at `HeroAnimationController.cs:21`, read
at `:147` and cleared at `:151`; a grep of `analysis/decomp/Assembly-CSharp/` finds **no writer that
ever sets it true**. So the `Backdash Land 2` `Play(null)` is dead in the vanilla assembly, and so is
the `Backdash To Idle` branch of `AnimationCompleteDelegate` (`:442-445`) — `Backdash To Idle` is
also absent from the library, so that comparison can never match either.

`Back Dash` / `Fireball` / `Swim` are live branches (`cState.backDashing` / `casting` / `swimming`),
and none of the four traces enters them. Whether the game actually hits `Play(null)` there, or
`HeroController.StopAnimationControl()` (`HeroController.cs:3127-3129`) is invoked first by one of
the Knight's FSMs, is UNVERIFIED — see §6 Q-tk2d-1.

---

## 3. Boss animation via PlayMaker

### 3.1 Action census (all four dumped boss scenes, `analysis/fsm/*.json`)

| action | total | FK | Gruz | Hornet | MMC |
|---|---:|---:|---:|---:|---:|
| `Tk2dPlayAnimation` | 1483 | 420 | 351 | 360 | 352 |
| `Tk2dPlayAnimationWithEvents` | 1080 | 267 | 259 | 281 | 273 |
| `Tk2dWatchAnimationEvents` | 356 | 105 | 85 | 83 | 83 |
| `Tk2dPlayFrame` | 280 | 71 | 64 | 68 | 77 |
| `Tk2dPlayAnimationV2` | 160 | 40 | 40 | 40 | 40 |
| `Tk2dSpriteSetColor` | 119 | 32 | 29 | 29 | 29 |
| `Tk2dSpriteGetColor` | 52 | 13 | 13 | 13 | 13 |
| `Tk2dStopAnimation` | 8 | 2 | 2 | 2 | 2 |
| `Tk2dPauseAnimation` | 8 | 2 | 2 | 2 | 2 |

`Tk2dResumeAnimation`, `Tk2dIsPlaying`, `Tk2dSetAnimationFrameRate` occur **zero** times in these
scenes. Event wiring across all four scenes:
counted over `analysis/fsm/GG_False_Knight.json`, `analysis/fsm/GG_Gruz_Mother.json`,
`analysis/fsm/GG_Hornet_1.json`, `analysis/fsm/GG_Mega_Moss_Charger.json`:
`Tk2dPlayAnimationWithEvents` — 918 with only `animationCompleteEvent`, 72 with only
`animationTriggerEvent`, 90 with neither; `Tk2dWatchAnimationEvents` — 340 complete-only,
16 trigger-only. Since no dumped boss library has a `triggerEvent` frame (§1.5), the 88
trigger-wired instances on those bosses are dead wiring.

### 3.2 Per-action semantics

All of them resolve the animator once, in `OnEnter`, via
`Fsm.GetOwnerDefaultTarget(gameObject).GetComponent<tk2dSpriteAnimator>()` and cache it in `_sprite`;
a null owner leaves the previous `_sprite` in place.

| action | `OnEnter` | `OnUpdate` | notes |
|---|---|---|---|
| `Tk2dPlayAnimation` | `_sprite.Play(clipName)`, then `Finish()` | — | fire-and-forget; the `animLibName` field is evaluated and discarded (`HutongGames.PlayMaker.Actions/Tk2dPlayAnimation.cs:53`) — dead code. `:39-55` |
| `Tk2dPlayAnimationV2` | `Play(clipName)` unless `doNotResetCurrentClip && clipName == CurrentClip.name`, then `Finish()` | — | the extra guard is redundant with `tk2dSpriteAnimator.cs:296` **except** when the clip has stopped: V2 with the flag will not restart a finished `Once` clip, plain `Play` would. `HutongGames.PlayMaker.Actions/Tk2dPlayAnimationV2.cs:43-60` |
| `Tk2dPlayAnimationWithEvents` | `Play(clipName)`; then `AnimationEventTriggered = …` if `animationTriggerEvent != null`; `AnimationCompleted = …` if `animationCompleteEvent != null`. **No `Finish()`** | none defined | the state stays active until an event moves it. Delegates installed **after** `Play`, so the `WarpClipToLocalTime` event fired by `Play` itself (`:544-547`) is missed. `HutongGames.PlayMaker.Actions/Tk2dPlayAnimationWithEvents.cs:43-65` |
| `Tk2dWatchAnimationEvents` | installs the same two delegates; does **not** play anything, does not `Finish` | `if (!_sprite.Playing) { Fsm.Event(animationCompleteEvent); Finish(); }` | two independent completion paths: the `AnimationCompleted` delegate (fires the frame the clip completes) **and** the polled `!Playing` check. A `Loop`/`LoopSection` clip never satisfies either. NRE if `_sprite` is null (no guard in `OnUpdate`). `HutongGames.PlayMaker.Actions/Tk2dWatchAnimationEvents.cs:38-51` |
| `Tk2dPlayFrame` | `_sprite.PlayFromFrame(frame.Value)` on the **current** clip, then `Finish()` | — | `PlayFromFrame(int)` → `PlayFrom(currentClip, (f+0.001f)/fps)` ⇒ always restarts and re-enables `Playing`. `HutongGames.PlayMaker.Actions/Tk2dPlayFrame.cs:34-46` |
| `Tk2dStopAnimation` | `_sprite.Stop()`, `Finish()` | — | leaves the sprite on its current frame. `HutongGames.PlayMaker.Actions/Tk2dStopAnimation.cs:30-46` |
| `Tk2dPauseAnimation` | `Pause()`/`Resume()` per `pause`; `Finish()` unless `everyframe` | same | `HutongGames.PlayMaker.Actions/Tk2dPauseAnimation.cs:40-72` |
| `Tk2dResumeAnimation` | `Resume()` if `Paused`; `Finish()` | — | `HutongGames.PlayMaker.Actions/Tk2dResumeAnimation.cs:31-48` |
| `Tk2dIsPlaying` | `IsPlaying(clipName)` → `isPlaying` var + `isPlayingEvent`/`isNotPlayingEvent`; `Finish()` unless `everyframe` | same | `HutongGames.PlayMaker.Actions/Tk2dIsPlaying.cs:52-84` |
| `Tk2dSetAnimationFrameRate` | writes `_sprite.CurrentClip.fps = framePerSeconds` | same if `everyFrame` | mutates the **shared clip asset**, permanently, for every animator using it. Unused in these four scenes. `HutongGames.PlayMaker.Actions/Tk2dSetAnimationFrameRate.cs:39-63` |

Event payloads. `AnimationEventDelegate` writes `Fsm.EventData.{IntData, StringData, FloatData}` from
the frame's `eventInt` / `eventInfo` / `eventFloat`, then `Fsm.Event(animationTriggerEvent)`
(`HutongGames.PlayMaker.Actions/Tk2dPlayAnimationWithEvents.cs:67-74`, `HutongGames.PlayMaker.Actions/Tk2dWatchAnimationEvents.cs:70-77`).
`AnimationCompleteDelegate` writes `Fsm.EventData.IntData` = the clip's index in
`sprite.Library.clips` (linear search, `-1` if absent), then `Fsm.Event(animationCompleteEvent)`
(`HutongGames.PlayMaker.Actions/Tk2dPlayAnimationWithEvents.cs:76-93`, `HutongGames.PlayMaker.Actions/Tk2dWatchAnimationEvents.cs:79-96`).

### 3.3 Measured FSM ↔ animator coupling (frame-exact)

Prediction: for each `Once` clip on `Hornet Boss 1`, the rendered frame in whose `LateUpdate`
`(int)clipTime` first reaches `frames.Length`. Comparison: the first
`EVENT FSM_TRANSITION` with `owner = "Hornet Boss 1"`, `fsm = "Control"` at
`Time.frameCount >= ` that frame, over `analysis/traces/p0/r2_idle.a.hktrace`, `analysis/traces/p0/r2_move.a.hktrace`, `analysis/traces/p0/r2_rand1.a.hktrace`, `analysis/traces/p0/r2_rand2.a.hktrace`.

Traces: `analysis/traces/p0/r2_idle.a.hktrace`, `r2_move.a`, `r2_rand1.a`, `r2_rand2.a`.
**44 completions, delta = 0 frames in 44/44 cases.** The FSM leaves the animation-driven state on the
**same rendered frame** as the completing `LateUpdate` — never the next one. (Clips involved: `Land`,
`Flourish`, `Evade Antic`, `Jump Antic`, `A Dash Antic`, `G Dash Antic`, `G Dash Recover1`,
`G Dash Recover2`, `Sphere Antic A`, `Sphere Recover A`, `Throw Antic`, …)

Worked example, `analysis/traces/p0/r2_rand1.a.hktrace`, `Throw Antic` (`Once, 14 frames @ 18 fps`, played by
`Tk2dPlayAnimationWithEvents` in state `Throw Antic` of `Hornet Boss 1 / Control`,
`analysis/fsm/GG_Hornet_1.json`): last record 177 at `frameCount` 24970 shows frame 13,
`clip_time` 0.76 (`clipTime` 13.68); that frame's `LateUpdate` adds `0.02 × 18 = 0.36` → 14.04 ≥ 14 →
complete. In the same trace (`analysis/traces/p0/r2_rand1.a.hktrace`) the recorded transitions
`Throw Antic → Lock? → Lock UL → Throw` carry `frame = 24970`, and the next `FRAME` record
(24972) already shows clip `Throw` at `clip_time` 0.

Caveat on sub-frame ordering: the `phase` byte on `EVENT` records is a static set by the recorder
MonoBehaviour's own `Update`/`LateUpdate` (`oracle/Oracle/TraceRecorder.cs:285-286`), so its value
depends on script execution order and does **not** prove the transition ran inside `LateUpdate`.
The frame-number alignment above is the solid claim; the intra-frame slot is Q4's business.

Also observed in `analysis/traces/p0/r2_*.hktrace`: `Playing == false` appears in **0** of the
1800 hero and 1800 Hornet `FRAME` records across the four traces — every completion is followed by a new `Play` before the next capture point.
The "finished `Once` clip holds its last sprite" behaviour is therefore code-derived (`:511-514`),
not directly observed here.

---

## 4. Sprite ↔ collider coupling

### 4.1 The mechanism that could exist

`tk2dBaseSprite.spriteId` setter calls `UpdateCollider()` on every change
(`tk2dBaseSprite.cs:159-189`, specifically `:182`), and `tk2dSpriteAnimator.SetSprite` →
`Sprite.SetSprite(collection, id)` → that setter (`tk2dSpriteAnimator.cs:591-594`,
`tk2dBaseSprite.cs:233-248`). `UpdateCollider` (`:434-624`) with `PhysicsEngine.Physics2D`:

* `ColliderType.Box` → creates/enables a `BoxCollider2D` and writes
  `offset = colliderVertices[0] * scale`, `size = |2 * colliderVertices[1] * scale|` (`:467-503`);
* `ColliderType.Mesh` → rebuilds `PolygonCollider2D` / `EdgeCollider2D` paths (`:504-590`);
* `ColliderType.Unset` → `if (colliderType != None) return;` — **immediate no-op** (`:591-596`);
* `ColliderType.None` → disables the box/polygon/edge colliders it owns (`:597-621`).

`CreateCollider()` (called from `tk2dSprite.Build`, `tk2dSprite.cs:81`) returns immediately on
`Unset` (`:629-632`).

### 4.2 It is not active in this game

**Hero — direct dump evidence.** `analysis/dumps/GG_Hornet_1/physics.json`:
`heroSprite.currentSpriteDef.colliderType = "Unset"`, `physicsEngine = "Physics2D"`,
`colliderVertexCount = 0`, and `heroSprite.boxCollider2D = null` (the cached field the setter would
have populated). ⇒ `UpdateCollider` bails at `:593` on every frame change.

**Hero — direct trace evidence.** The `FRAME` record carries every `Collider2D` on the Knight
GameObject (`docs/trace-format.md`). Over `analysis/traces/p0/r2_move.a.hktrace` (600 frames, 47 clip segments including
`Slash`, `Dash`, `Shadow Dash`, `Airborne`, `UpSlash`, `DownSlash`) and `analysis/traces/p0/r2_rand1.a.hktrace` (480 frames,
89 segments) there is exactly **one** distinct collider configuration in the `FRAME` collider block
(`docs/trace-format.md`): `BoxCollider2D enabled offset (0, -0.75) size (0.5, 1.28125)`.
The hero body collider never changes
with the animation.

**Boss — the collider changes, but the FSM does it, not the sprite.** Decoding the `OBS` payloads
(`oracle/Net/BinaryProtocol.cs:40-93`, 14 combat features per row,
`oracle/Game/HitboxObserver.cs:793-797`) across all four traces and grouping the
`Hornet Boss 1` row's `(w, h)` by `(clip, anim_phase)` gives one size **per clip**, constant across
every frame of that clip (`Flourish`: 14 distinct phases, one size; `Throw Antic`: 14 phases, one
size; over `analysis/traces/p0/r2_idle.a.hktrace` and its three siblings). A sprite-driven collider would change per *frame*, not per clip. The sizes match FSM literals
in `analysis/fsm/GG_Hornet_1.json`, FSM `Control` on `Hornet Boss 1`, action
`HutongGames.PlayMaker.Actions.SetBoxCollider2DSizeVector`:

| FSM variable | FSM value | observed `(w, h)` | states |
|---|---|---|---|
| `Box Size Idle` | (0.8946984, 2.564674) | (0.8947, 2.5647) | `Idle`, `Jump Antic`, `Land`, `GDash Recover2`, `Hit Roof`, `Wall L`, `Wall R`, `Sphere Antic G`, `Hard Land`, `Stun Start`, `Wake`, `GG Land` |
| `Box Size Antic` | (1.229008, 1.380672) | (1.229, 1.3807) | `GDash Antic`, `GDash Recover1`, `ADash Antic` |
| `Box Size GDash` | (1.56331, 1.506035) | (1.5633, 1.506) | `G Dash` |
| `Box Size Throw` | (0.9817337, 2.564674) | (0.9817, 2.5647) | `Throw Antic` |
| `Box Size Throwing` | (1.393576, 1.156178) | (1.3936, 1.1562) | `Throw` (`Throw Recover` sets no size and inherits it) |
| `Box Size ADash` | (1.460247, 1.025071) | seen as rotated AABBs (1.6478, 0.8821) / (1.7444, 1.5133) | `Fire` |

The `A Dash` / `G Dash` multiplicity is `SetRotation` in the same FSM (`Land`, `Hit Roof`, `Wall L/R`,
`Hard Land`, `Stun Start`, `GG Land` all reset it). `Collider2D.bounds` is a **world-space AABB**: in
`analysis/dumps/GG_Hornet_1/bosses.json` the Hornet root `BoxCollider2D` carries local
`size` (1.39357567, 1.15617847) and `bounds` center (31.2684021, 43.6259766) / size
(1.39357758, 1.15618134) — a world centre, and a size ≈ `size` at identity rotation — while
`A Dash Range`'s `PolygonCollider2D` (path spanning ±25) reports `bounds.size`
(51.64321, 41.3797874). That an axis-aligned box reports a rotation-dependent size is geometry, not
an engine claim. Two further points make it conclusive: state `GG Fall` plays
clip `Fall` with **no** `SetBoxCollider2DSizeVector`, so the collider keeps whatever value was last
written — at SceneReady `analysis/dumps/GG_Hornet_1/bosses.json` records size (1.39357567, 1.15617847) with
`animator.currentClip == "Fall"`, i.e. the `Box Size Throwing` literal under the `Fall` clip — and
across the corpus the `Fall` clip is observed with **two** different sizes. Clip and collider are
independent state.

The same FSM also flips `isTrigger` with `SetBoxColliderTrigger` (`Fire` → true; `Land`, `Hit Roof`,
`Wall L`, `Wall R`, `Hard Land`, `Stun Start` → false) and toggles child hitboxes with `SetCollider`
(`Needle Tink`). Hornet's damage/detector hitboxes are separate static child colliders enabled and
disabled by FSMs — `Hit GDash`, `Hit ADash` (`PolygonCollider2D`, layer `Enemy Attack`, both
`activeSelf: false` at SceneReady), `Sphere Ball` (`CircleCollider2D`), `A Dash Range`,
`Sphere Range`, `Evade Check`, `Run Away Check`, `Evade Range`, `A Sphere Range`, `Refight Range`
(`analysis/dumps/GG_Hornet_1/bosses.json`, `colliders` + `components`).

**Attach points are absent.** `tk2dSpriteAttachPoint` repositions child transforms on the
`SpriteChanged` event (`tk2dSpriteAttachPoint.cs:30-44`, `:46-52`, `:65+`), which *would* move child
colliders per animation frame. `grep tk2dSpriteAttachPoint` over
`analysis/dumps/{GG_False_Knight,GG_Gruz_Mother,GG_Hornet_1,GG_Mega_Moss_Charger}/*.json` returns
**zero** hits.

**`SpriteFlash` does not touch colliders** — no `Collider`/`size` reference in
`analysis/decomp/Assembly-CSharp/SpriteFlash.cs`. It is present on both the Knight
(`analysis/dumps/GG_Hornet_1/hero.json`, `components[0]`) and on `Hornet Boss 1`
(`analysis/dumps/GG_Hornet_1/bosses.json`, `components[0]`).

**Conclusion for the simulator.** Colliders are static shapes whose `size` / `offset` / `isTrigger` /
`enabled` / `activeSelf` are written by FSM actions and by C# components (`NailSlash`, §2.4), never by
the animator. The animator's only observable coupling into physics is *none*; its coupling into the
game is through `AnimationCompleted` / `AnimationEventTriggered` and through `IsPlaying`/`Playing`
polls. **A simulator can therefore run the animator purely as an observation channel** — provided it
also implements the completion events, which drive boss state transitions frame-exactly (§3.3).

---

## 5. What the observation needs

`oracle/Game/HitboxObserver.cs`:

* **Animator resolution (`ClassifyEntity`, `:241-260`).** From the collider's own transform, walk
  `t = t.parent` up to **8** levels, taking the first `tk2dSpriteAnimator` found via
  `t.GetComponent<tk2dSpriteAnimator>()`. That animator is stored in `animCache[col]`; the entity
  *kind* string is `Strip(t.gameObject.name)`. If none is found within 8 levels,
  `animCache[col] = null` and the kind falls back to `Strip(col.gameObject.name)` (or `"unknown"`).
  `Strip` removes a `"(Clone)"` suffix and trims (`:262-268`). Both caches are keyed by `Collider2D`
  and only evicted when the collider is destroyed (`:88`).
  Note the walk is by *transform ancestry*, and it is a different walk from `ClassifyParent`
  (`:221-239`), which looks for the nearest `HealthManager` — so `kind` (visual entity) and `parent`
  (HP owner) can resolve to different objects.

* **`anim_phase` (`GetClipKey`, `:152-175`):**

  ```
  anim = animCache[col]            // populated by GetKind/ClassifyEntity
  if anim == null              -> ("none", 0.0)
  clip = anim.CurrentClip
  if clip == null              -> ("none", 0.0)
  len = clip.frames?.Length ?? 0
  phase = len > 0 ? Mathf.Clamp01((float)anim.CurrentFrame / len) : 0.0
  ```

  Denominator is `len`, **not** `len - 1`, so a running clip yields `{0, 1/n, …, (n-1)/n}` and a
  finished `Once` clip yields exactly `1.0` (because `CurrentFrame` returns `n` there,
  `tk2dSpriteAnimator.cs:152-153`) — the `Clamp01` is a safety net, not the mechanism.
  Verified in the decoded `OBS` stream of `analysis/traces/p0/r2_rand1.a.hktrace`:
  `Hornet Boss 1|Throw Antic` (14 frames) emits `0, 0.0714, 0.1429, …, 0.9286`;
  `Fall` (4 frames) emits `0, 0.25, 0.5, 0.75`.

* **Clip identity string:** `Strip(anim.gameObject.name) + "|" + clip.name`, memoised per clip object
  in `clipKeyCache` (`:169-174`). Note the entity half comes from the *animator's* GameObject, which
  is the same object `GetKind` returned. Examples decoded from `OBS`:
  `"Hornet Boss 1|Fall"`, `"Hornet Boss 1|Throw Antic"`, `"Slash|SlashEffect M"`,
  `"UpSlash|UpSlashEffect M"`, `"Cyclone Slash|Cyclone Effect"`, `"Needle|Needle"`.

* **Where it lands on the wire.** Combat row layout, 14 floats
  (`oracle/Game/HitboxObserver.cs:793-797`):
  `[rel_x, rel_y, w, h, vel_x, vel_y, is_trigger, gives_damage, takes_damage, is_target,
  is_invincible, hp_raw, hp_max_raw, anim_phase]` — `anim_phase` is index **13**. In this oracle build
  the `combat_kinds` string list carries `GetKind(col)` and the `combat_parents` string list carries
  the **clip key**, not the HealthManager parent name (`:798-799`); the packer emits kinds then
  parents, one length-prefixed UTF-8 string each per combat row
  (`oracle/Net/BinaryProtocol.cs:75-93`). `w`/`h` are `Collider2D.bounds.size.{x,y}`
  (`oracle/Game/HitboxObserver.cs:744-756`) — the world-space AABB evidenced in §4.2, so object
  rotation and `localScale` feed into them.
  Global state is 33 floats (`oracle/Game/StateExtractor.cs:11`) and contains no animation data.

* **Consequences for a simulator.** To reproduce the observation byte-for-byte it must track, per
  animator: `currentClip` (name + `frames.Length`), `clipTime`, `clipFps`, `Playing`, `Paused`, and
  the animator↔collider ownership walk. It does **not** need per-frame sprite geometry — no sprite
  definition data reaches the wire (bounds come from colliders, §4).

---

## 6. Open questions

Closed during the P1 review, kept here for the trail: the former **Q-C** (does an `Fsm.Event()` raised
from the animator's `LateUpdate` commit immediately or get queued?) is answered by
`analysis/specs/fsm-runtime.md` §2.2 and the PlayMaker decomp it cites. `EventData.SentByFsm` is
stamped from `FsmExecutionStack.ExecutingFsm`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:2128`,
`analysis/decomp/PlayMaker/HutongGames.PlayMaker/FsmExecutionStack.cs:9-19`), and `DoTransition`
calls `UpdateStateChanges()` immediately when `SentByFsm != this`
(`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:2340-2343`). The animator's `LateUpdate` is a
Unity callback that pushes nothing onto that stack, so `ExitState` + `EnterState` run synchronously
inside it — which is exactly the delta = 0 measured in 44/44 completions (§3.3).

### Q-tk2d-1 — Which agent stops hero animation control before an absent clip is played?

§2.5 lists six clip names referenced by `HeroAnimationController` that are absent from the Knight
library. Three of them sit on live branches — `Back Dash` (`HeroAnimationController.cs:272`),
`Fireball` (`:299`), `Swim` (`:341`) — and none of the four corpora
(`analysis/traces/p0/r2_idle.a.hktrace` and its three siblings) enters `cState.backDashing` /
`casting` / `swimming`, so it is unknown whether the game really takes the `Play(null)` path there
(LogError + a spurious `AnimationCompleted` on the previous clip + `Stop`,
`tk2dSpriteAnimator.cs:334-339`) or whether one of the Knight's 12 FSMs calls
`HeroController.StopAnimationControl()` (`HeroController.cs:3127-3129`) first. It matters to the port
because the `Play(null)` path stops the animator and therefore forces a restart of the next state clip
(§2.2 case 1). Evidence needed: a corpus that casts a spell / focuses / back-dashes, plus a
state-level dump of the Knight's 12 FSMs — `analysis/dumps/GG_Hornet_1/hero.json` only counts the
components and `analysis/fsm/GG_Hornet_1.json` names them (`Spell Control`, `Nail Arts`, `Superdash`,
`Dream Nail`, `Map Control`, `Roar Lock`, `Surface Water`, `Dream Return`, `Spore Cooldown`,
`Globalise`, `Control Interpolation`, `ProxyFSM`) without any spec owning their behaviour (review gap
G2).

### Q-tk2d-2 — What is `colliderType` on the boss sprite definitions?

`tk2dSpriteDefinition.colliderType` is read as `Unset` only for the Knight's *current* sprite at
SceneReady (`analysis/dumps/GG_Hornet_1/physics.json`, `heroSprite.currentSpriteDef`); no boss sprite
definition is dumped at all. §4.2's conclusion for bosses is therefore behavioural — the collider size
is constant across every frame of a clip and matches `SetBoxCollider2DSizeVector` literals — rather
than a direct read of the field that makes `tk2dBaseSprite.UpdateCollider` a no-op
(`tk2dBaseSprite.cs:591-596`). Evidence needed: extend the reflection dumper to emit
`tk2dSprite.CurrentSprite.{colliderType, physicsEngine}` for every entity, ideally as a histogram over
the whole sprite collection so that one frame's value cannot mislead.

### Q-tk2d-3 — Does any animator's completion handler drive a different animator?

Each boss carries several animators (`Hornet Boss 1`, `Sphere Ball`, `A Dash Effect`, `G Dash Effect`,
`Flash Effect`, `Throw Effect`, the corpse and its `Thread` / `Leave Anim`;
`analysis/dumps/GG_Hornet_1/bosses.json` `components`), and all of them advance in `LateUpdate` with
the same `Time.deltaTime`, so their relative order is irrelevant *unless* one animator's completion
handler plays on another. Partial answer: in `analysis/fsm/GG_Hornet_1.json` **0** of the `Tk2d*`
actions owned by a `Hornet Boss 1` FSM target a non-owner GameObject, so the coupling does not exist
for this boss; scene-wide, though, 217 `Tk2d*` action instances do specify a foreign target (e.g.
`Grimmball(Clone)` / `Control` / `Impact`), so the hazard is real for other entities. Evidence needed:
the same census over the other three boss scenes, plus the LateUpdate-ordering half of review gap G5.

### Q-tk2d-4 — Which libraries in these scenes actually have `triggerEvent` frames?

88 FSM action instances are wired to `animationTriggerEvent` (§3.1), yet none of the five dumped
libraries (Knight plus the four bosses) contains a single frame with `triggerEvent` set
(`analysis/dumps/GG_Hornet_1/physics.json`, `analysis/dumps/GG_Hornet_1/bosses.json` and the three
sibling scenes), so all 88 are dead wiring *for the objects that have been dumped*. Which objects own
the libraries those actions point at, and whether any of them is combat-relevant, is unknown.
Evidence needed: a dump of every `tk2dSpriteAnimation` in the scene rather than only the five
reachable from the hero and the `HealthManager`s.

### Q-tk2d-5 — The finished-`Once` hold is code-derived, never observed

`Playing == false` appears in 0 of the 3600 `FRAME` records (§3.3), because every completion is
followed by a new `Play` before the next capture point. The behaviour the port depends on — a
finished `Once` clip holding sprite `n-1` while `CurrentFrame` returns `n`, hence
`anim_phase == 1.0` (`tk2dSpriteAnimator.cs:152-153`, `:511-514`) — is therefore read from the decomp
and never witnessed. Evidence needed: a corpus that parks an entity in a state whose
`Tk2dWatchAnimationEvents` has no `animationCompleteEvent`, or a second capture point placed inside
`LateUpdate` so the post-completion slot becomes visible.

### Q-tk2d-6 — Is the clip table shared-immutable or per-world?

`Tk2dSetAnimationFrameRate` writes `_sprite.CurrentClip.fps`
(`HutongGames.PlayMaker.Actions/Tk2dSetAnimationFrameRate.cs:62`), mutating the shared clip *asset*
permanently for every animator that uses it. It appears 0 times in the four dumped boss scenes
(§3.1), so the simulator can currently treat the clip table as shared and immutable across worlds —
but a single occurrence in a later scene forces it to become per-world mutable, which is a data-layout
decision (P7), not a patch. Evidence needed: an action census over the full scene set the sim is
expected to cover.

### Q-tk2d-7 — Engine semantics this spec assumes rather than derives

Five statements in §1 and §4 are properties of Unity or of C#, not of anything in `analysis/`, and are
tagged **[ENGINE]** at their use sites: (a) `LateUpdate` is dispatched at most once per rendered
frame; (b) a C# `(int)` cast on a `float` truncates toward zero; (c) `Time.deltaTime` is the scaled
delta, i.e. `timeScale` × `captureDeltaTime` under a pinned capture; (d) Mono's codegen for
`clipTime += deltaTime * clipFps` does not contract the multiply-add (the *sim* side is pinned by
`docs/float-parity.md:10`, the *oracle* side is assumed); (e) `Collider2D.bounds` is the world-space
AABB. Each of (a), (b), (c) and (e) is consistent with the measurement cited beside it — (b) is
positively discriminated by the `Turn` row of §1.6 and (e) by the dump values in §4.2 — but none is
proven by it, and (d) is untested in either direction. Evidence needed to close: a probe mod that
reads each quantity directly, or an explicit decision to carry them as load-bearing engine
assumptions (in which case they belong on the P8 domain-randomisation list).

---

## Review fixes

Applied 2026-08-31 in response to `analysis/specs/REVIEW-p1.md` (verdict PASS-WITH-FIXES).

| defect | change |
|---|---|
| **D40** (MISREAD, §1.2) | The sole caller of `tk2dSpriteAnimator.g_Paused` is the **static** property `tk2dAnimatedSprite.g_paused` (`tk2dAnimatedSprite.cs:61-71`), not the instance property `Paused`. Corrected the name and the line range, and noted that the instance `Paused` (`tk2dAnimatedSprite.cs:73-77`) proxies `Animator.Paused` and never touches `globalState`. |
| **D41** (WRONG-LINE, path) | Every `Tk2d*.cs` citation now carries the `HutongGames.PlayMaker.Actions/` directory — the delegate-assignment list in §1.5, all ten rows of the §3.2 action table, and the two event-payload citations after it. |
| **D42** (MISREAD, count) | "Three referenced clips do not exist" was wrong. Replaced by a new **§2.5** built from a full sweep of every clip-name literal in `HeroAnimationController.cs` against the 214 library names: **6 distinct names over 7 call sites** — `Play`: `Backdash Land 2` (`:149`), `Back Dash` (`:272`), `Fireball` (`:299`), `Swim` (`:341`); `IsPlaying`: `Backdash Land` (`:407`), `Backdash Land 2` (`:407`), `Lookup` (`:416`). The `IsPlaying("Backdash Land 2")` site is one the review did not list. §2.5 also documents the `Play(null)` side effects: a spurious `AnimationCompleted` fired on the *previous* clip, and the animator stopped, which forces a restart of the next state clip. |
| **D42** (rider, stronger than the review) | **P3 is unreachable.** `playBackDashToIdleEnd` is declared (`HeroAnimationController.cs:21`), read (`:147`) and cleared (`:151`) but has **no writer anywhere in `analysis/decomp/Assembly-CSharp/`**, so the `Backdash Land 2` `Play(null)` never happens in the vanilla assembly. The P3 row of the §2.1 table is marked dead code, P3 is folded into Q-tk2d-1, and the moot §2.4 delegate note is rewritten: `Backdash To Idle` is absent from the library *and* its only producer never runs, so 3 of the 5 `AnimationCompleteDelegate` branches are reachable. |
| **D43** (UNCITED, 5 facts) | Each engine fact is now tagged **[ENGINE]** at its use site and collected in **Q-tk2d-7**: `LateUpdate` once per frame (§1.1, paired with the 3600/3600 measurement that is what the port actually needs); C# truncation (§1.2, now positively discriminated by `Turn` LateUpdate 4, `clipTime = 1.5999999` with `CurrentFrame = 1`, which rules out round-to-nearest); scaled `deltaTime` (§1.2, evidenced by `oracle/Environment/TrainingEnv.cs:376`); FMA (§1.6, reframed as the `docs/float-parity.md:10` build contract rather than a claim about Mono); and `Collider2D.bounds` = world AABB (§4.2 and §5, now evidenced from the size-vs-bounds values in `analysis/dumps/GG_Hornet_1/bosses.json`). |
| **count fix** (not in the review's defect list; corrects this spec and review gap G2) | The Knight root carries **12** `PlayMakerFSM` components, not 13 — 12 occurrences in the 29-entry component list of `analysis/dumps/GG_Hornet_1/hero.json`, and `analysis/fsm/GG_Hornet_1.json` names exactly 12 for `gameObject == "Knight"`. All 12 names are now listed in §2 and in Q-tk2d-1. |
| **Q-C closed** | Closed as the review directed, by `analysis/specs/fsm-runtime.md` §2.2: the animator's `LateUpdate` is outside any FSM's execution stack, so `SentByFsm != this` and `DoTransition` commits `UpdateStateChanges()` synchronously (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:2340-2343`) — the mechanism behind §3.3's 44/44 delta = 0. Recorded above the numbered list, not as an open question. |
| **format** | Open questions renumbered `Q-tk2d-1` … `Q-tk2d-7` with `### ` headings and one paragraph each, per the coordinator's consolidation format. `analysis/open-questions.md` was not touched. |

Confirmed by the review and left as written: the frame-advance formula and its 3600/3600 trace
verification (§1.6), the wrap-mode table (§1.3), the `Tk2d*` action semantics (§3.2), the 44/44
frame-exact FSM-animator coupling (§3.3), and the sprite-collider conclusion (§4).
