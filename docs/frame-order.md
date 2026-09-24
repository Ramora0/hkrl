# Frame order and regime R2

The sim implements one execution regime, R2. The oracle mod runs the real game in the same regime in every
launch mode (`oracle/Env/RegimeTweaks.cs`, `oracle/Env/TrainingEnv.cs`). Unity's order of callbacks within a
frame is in `docs/engine-lifecycle.md`. This file covers the regime and the shape of one agent step.

## Regime R2

- `Time.captureDeltaTime = Time.fixedDeltaTime = 0.02`. The capture dt is pinned in `Reset()` and held, so
  each live frame runs exactly one FixedUpdate, independent of any accumulator residual. Capture mode needs
  a renderer: `-batchmode` works, `-nographics` does not.
- `UnityEngine.Random.InitState(seed)` runs at `sceneLoaded` of the boss scene (`HK_ORACLE_SEED` in the mod;
  `hksim_reset(s, seed)` in the sim).
- No `Rigidbody2D` interpolation, for the whole episode. With it, `transform.position` carries a per-run
  offset `v·(Time.time − Time.fixedTime)` that boss FSMs read. Every managed call of
  `Rigidbody2D.set_interpolation` in the game's assemblies is IL-rewritten to pass `None`: the hazard
  respawn (HeroController.cs:2829), `HeroPlatformStick` (HeroPlatformStick.cs:22, :44), and the actions
  `SetInterpolate` / `SetExtrapolate` / `SetInterpolateNone` (SetInterpolate.cs:30, SetExtrapolate.cs:30,
  SetInterpolateNone.cs:30; the Knight's `Control Interpolation` FSM runs `SetInterpolate` 0.5 s after every
  `LEVEL LOADED`): 6 call sites, one per method, two of which already pass `None`. The serialized value (the
  Knight and Grimmchild are `Interpolate`) is forced to `None` on every loaded body, prefab assets included,
  at each scene load and at SceneReady (`oracle/Env/RegimeTweaks.cs`). The sim has no interpolation.
- `ShakePositionV2` without its frame-rate limit: its `FpsLimit` reads in `UpdateShaking` are IL-rewritten
  to read 0, for every instance. The branch it skips rate-limits on `Time.unscaledTime`, which makes the
  action's `Random.Range` draw count wall-clock dependent (ShakePositionV2.cs:81-96). The sim traps on
  `FpsLimit > 0`.
- The wall clock runs on frames inside the episode (`oracle/Env/RegimeClock.cs`). From the boss scene's
  `sceneLoaded` (where the seed is applied) to the next reset, a frame lasts 0.02 s, frozen frames included:
  PlayMaker's `FsmTime.RealtimeSinceStartup`, `WaitForSecondsRealtime`, and `AudioSource.isPlaying` at every
  call site in the game's assemblies (IL-rewritten: a clip plays `(length - time) / |pitch|` s from its `Play`
  or `PlayOneShot`). Every particle system gets a fixed random seed. The Workshop and the transitions keep the
  wall clock, because they wait in real time for asynchronous loading. Background loading runs at `High`
  priority (preload thread priority and a 50 ms per-frame integration budget), so the uncapped main threads of
  concurrent instances do not starve it; it only changes how long a load takes, before the frame clock arms.
- The regime fails closed. If a pin throws, rewrites no call in a method the scan found, or the
  `ShakePositionV2` pin does not rewrite exactly its 2 reads (ShakePositionV2.cs:82, :88), or the frame clock
  cannot be installed, the mod logs `[Regime] FAILED` and does not start the env, so the instance never
  connects.
- Hit-stop is off: `GameManager.FreezeMoment*` are replaced with no-ops.
- Between agent steps `timeScale = 0`. Update and LateUpdate still run with `deltaTime = 0`, and
  `oracle/Game/FsmPauseGate.cs` gates the PlayMaker Update, FixedUpdate and LateUpdate proxies off while
  `timeScale <= 0`.
- `frames_per_wait` comes from the reset message. Every recorded corpus uses 2. The trainer sets its own in
  `train/config.py`.

## One agent step

Every step is exactly one frozen frame plus `frames_per_wait` live frames, whatever the policy's latency.
The reply carrying an observation (reset or step) is sent in frame F. The request pump
(`oracle/Env/WebsocketEnv.cs`) serves the next request in frame F+1 and blocks the main thread inside that
frame until the request arrives, so no frame runs while the policy thinks. After each 60 s without a request
the peer must answer a websocket ping, or the env ends as it does when the socket closes. `pause`, `resume` and `close` are
answered inside the frame they are read in. A request served in any other frame is logged (`[Pump] ... late
serves`). Waiting by yielding frames would run one more frozen frame per frame of latency, each with its
Update, LateUpdate, pending Starts and coroutine resumes.

The step request is handled in the frozen frame (`timeScale` 0), inside the env coroutine:

1. **Frozen frame:** no fixed step, and Update runs with dt 0. In `update_delayed` the Step coroutine sets
   `timeScale = 1`, runs `ActionDecoder.ApplyAction` (the input for this step, with its hard-commit
   override), and raises `StepBegin` (EVENT STEP). The frame's Animators (dt 0) and LateUpdate follow.
2. **`frames_per_wait` live frames,** each consisting of:
   - FixedUpdate: PlayMaker proxies and Recoil, then `HeroController` (208), then `NailSlash` (500);
   - Physics2D step and its Enter/Stay/Exit callbacks;
   - Update;
   - `update_delayed`: pending Starts, then coroutine resumes, including the env coroutine, which writes
     FRAME;
   - the Animators (PreLateUpdate/DirectorUpdateAnimation, dt 0.02 even in the step's last live frame);
   - LateUpdate.

   The loop breaks after the frame in which the boss died or the knight's health reached 0.
3. **`timeScale = 0`.** The episode-end checks run, then the observation is packed (OBS) and the reply sent.

The reset observation is built before `SceneReady`, while interpolation is still on. It can differ between
runs by a few float32 ULP in knight-relative positions. The observations from step 2 on do not.

In the sim, `hksim_step` is this sequence: `lc_frozen_pre` / `hero_apply_action` / `lc_frozen_late`, then
`live_frame_pre` for each live frame (`sim/core/sim.c`), with the stages dispatched by `sim/fsm/lifecycle.c`.

## Wall-clock reads

`python tools/wallclock_sites.py --out <file>` lists every call site in Assembly-CSharp, -firstpass and
PlayMaker of an API that reads wall-clock time or counts rendered frames (`Time.realtimeSinceStartup`,
`unscaledTime`, `unscaledDeltaTime`, `frameCount`, `timeSinceLevelLoad`, `FsmTime.RealtimeSinceStartup`,
`WaitForSecondsRealtime`, `AudioSource.isPlaying`/`time`, `DateTime.Now`, `Stopwatch`, `Environment.TickCount`;
`oracle/Record/WallClockSites.cs`), read from the IL with Mono.Cecil, and the ones the dumped scenes can
reach, and every dumped FSM action with `realTime` true. In a record run, `HK_ORACLE_WALLCLOCK=1` writes
which of the sites fire in each episode and in which object (`<trace>.wallclock.jsonl`,
`oracle/Record/WallClockCounter.cs`).

The reachable sites, by what they do in a fight under R2:

| sites | in a fight |
|---|---|
| `Time.frameCount`: `HeroController.Update` (`Update10` every 10 frames: out-of-bounds check, scale and z clamps, HC:5108, HC:1212), `AutoRecycleSelf.Update` (every 20 frames, AutoRecycleSelf.cs:49), PlayMaker's `FsmTime`/`FsmLog`, rendering | Deterministic once every step runs a fixed number of frames (the pump). The phase comes from the initial frame count, which the sim takes as a config clock. |
| `Time.timeSinceLevelLoad`: the alternate-slash timer (HC:1325, HC:1496), a camera log line | Game time: it advances only in live frames. Its SceneReady value is an initial condition (hksim `tsll`). |
| `ShakePositionV2.UpdateShaking` (`unscaledTime`) | Draws RNG. Pinned: `FpsLimit` reads 0 (above). |
| `GameManager.FreezeMoment*` (`unscaledDeltaTime`) | Hit-stop. Replaced with no-ops (`TrainingEnv.Setup`). |
| `realTime` waits and tweens (`Wait`, `WaitRandom`, `RandomWait`, `Ease*`, `CameraFade*`, iTween `ignoretimescale`) | Only the camera's `CameraFade` and the HUD's `Blanker Control` FSMs set `realTime` in the 16 dumped scenes: cosmetic. The PlayMaker ones read `FsmTime.RealtimeSinceStartup`, which is the frame clock in the mod. The sim traps on a `realTime` `Wait`/`WaitRandom`/`EaseFloat`/`EaseColor` and on iTween `ignoretimescale`. |
| `FsmState.OnEnter` `RealStartTime` | Read only by `FsmLog` and by `GetTimeInfo`, which no dumped FSM uses. |
| InControl's clock (`InputManager.UpdateCurrentTime`, key repeat) | Repeat is menu-only; the `deltaTime` it passes to devices feeds only `Utility.ApplySmoothing`, which nothing calls. |
| `AudioPlay` finishing when its clip stops (AudioPlay.cs:74-77) | It changes a fight only through `finishedEvent`, or as the last unfinished action of a state with a `FINISHED` transition (FsmState.cs:609-620): the sim traps on both (`sim/fsm/actions/audio_fx.c`). In the mod `isPlaying` is on the frame clock, so the finish comes a fixed number of frames after `Play`. |
| `AutoRecycleSelf` (`AUDIO_CLIP_END`), `PlayAudioAndRecycle`, `HeroAudioController`, `AudioManager` cues (`isPlaying`, `WaitForSecondsRealtime`) | Audio objects returning to their pools, and sound. On the frame clock in the mod. |
| music (`AudioLoopMaster`: `AudioSource.time`, `timeSamples`) | Sound only. Not pinned. |
| `ParticleSystemAutoRecycle` (`IsAlive`, particle lifetimes from an automatic random seed) | Pooled effects returning to their pools. Fixed seed in the mod. |
| scene transitions, pause menu, UI fades, `HeroController.EnterScene`, `ObjectPool.Awake`, the mod loader | Outside the fight. |

## Known residue

These differ between runs of the game and are initial conditions, not behaviour:
- Absolute clocks (`frame`, `fixed_count`, `time`) carry a per-run boot offset. So do fields stamped from
  them (`hero.f.altAttackTime` and other `*Time*` fields). The sim takes the initial clocks as config
  (`hksim_config` clock fields).
- `PlayerData.brettaPosition` is drawn before the seed is applied.
- Scene fades use `realTime = true` but complete before SceneReady.
- `Time.frameCount` modulo 10 or 20 at SceneReady (the phase of `HeroController.Update10` and
  `AutoRecycleSelf.Update20`) carries the boot offset too.
- Box2D broadphase proxy ids carry the tree's history since boot (`analysis/native_specs/native-physics2d.md`),
  so contacts that begin in the same physics step can call back in a different order.
- The HUD title card (`Area Title Control`, switched on by the boss FSM before SceneReady, e.g. GG_Nosk
  `Mimic Spider | Roar Init`) can reach SceneReady one frame further into its `Visited Appear` wait, so its
  `FINISHED` comes one frame earlier or later. No FSM in the 16 dumped scenes listens for the `TITLE
  DISPLAYED` it sends. The mod logs its state and `Wait.timer` at SceneReady (`[RegimeClock] SceneReady`).
