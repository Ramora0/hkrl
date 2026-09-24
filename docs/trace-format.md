# Trace format (`.hktrace`)

The binary record stream that the game mod records and the sim emits. The producers are
`oracle/Record/TraceRecorder.cs` + `TraceWriter.cs` (game, schema v2) and `sim/core/tracew.c` (sim, schema v1).
The reader is `hkpy/hktrace.py`, which accepts v1 and v2. All three must agree with this file. A layout
change bumps the version.

Little-endian throughout. `str16` = u16 byte length + UTF-8 bytes. Bools are u8 (0/1).

## Header
```
u8[4] "HKTR"  |  u32 version  |  u32 json_len  |  u8[json_len] UTF-8 JSON
```
JSON keys:
- `capture`: `{scene, level_requested, unity_version, mod_commit, tier_requested, tier_loaded, boss_level,
  timestamp_utc, capture_dt, fixed_dt, frames_per_wait, launch_args:[..], mode:"script"|"ws"|"sim",
  script_path, script_sha256, seed:int|null, exe_tag}`.
  `mod_commit` is the commit the mod was built from (`<sha>[-dirty]`, `docs/oracle.md` Build; "" for a
  build that predates the stamp). Traces from mods that still read the opt-in switches may also carry
  `oracle_env:{HK_ORACLE_*: value}`, those switches as the process had them; `hkpy/provenance.py` reads it
  to label the recording LEGACY.
- `fields`:
  - `hero`: `[{name,type}]`, every primitive-typed instance field of `HeroController` (float/int/bool/enum;
    public and nonpublic; declaration order; float as f32, everything else as i32).
  - `cstate`: `[name]`, every public bool field of `HeroControllerStates` in declaration order; bit i of
    `FRAME.cstate` = field i.
  - `playerdata`: `[{name,type}]`, every primitive-typed instance field of `PlayerData`, same rules.
  - `input`: `[name]`, `InputDeviceShim.KeyNames`; bit i of `FRAME.input` = key i.
  - `hero_go`: the hero GameObject's name.

## Records
A stream of `u8 kind` + payload until EOF. An unknown kind is a parse error, never skipped.

Arming: boot and scene-load frames depend on wall-clock time. So nothing is recorded before
`Hooks.SceneReady` except the EVENTs RESET_BEGIN, SCENE_LOADED and RNG_SEED. The next record is EVENT
SCENE_READY, and from then on every capture point emits.

### 0x01 FRAME: once per rendered frame, at `Hooks.Frame` (coroutine resume after `yield return null`)
```
u32 frame           Time.frameCount
u32 fixed_count     FixedUpdate calls seen by the recorder since process start
f32 time            Time.time
f32 dt              Time.deltaTime
f32 unscaled_dt     Time.unscaledDeltaTime
f32 fixed_time      Time.fixedTime
f32 time_scale      Time.timeScale
u32[4] rng          UnityEngine.Random.state s0..s3
u32 input           InputDeviceShim.KeyBits()
u32 step            agent step index (TrainingEnv._stepCount)
HERO   block
ENTITY block
```
HERO block:
```
f32 pos_x, pos_y            hero transform.position
f32 scale_x                 transform.localScale.x
f32 rb_pos_x, rb_pos_y      rb2d.position
f32 rb_vel_x, rb_vel_y      rb2d.velocity
f32 rb_gravity              rb2d.gravityScale
u8  rb_kinematic            rb2d.isKinematic
u64 cstate                  bitset per fields.cstate
f32|i32 x len(fields.hero)        in header order
f32|i32 x len(fields.playerdata)  in header order
ANIM sub-block
u8 n_col; per Collider2D on the hero GameObject (not children): str16 type, u8 enabled,
          f32 off_x, off_y, size_x, size_y (size = 0,0 for non-box)
```
ANIM sub-block (the tk2dSpriteAnimator on the same GameObject; all zero/"" if absent):
```
str16 clip   i32 frame (CurrentFrame)   f32 clip_time (ClipTimeSeconds)   u8 playing   f32 clip_fps
```
ENTITY block: every `HealthManager` from `Resources.FindObjectsOfTypeAll<HealthManager>()` whose
`gameObject.scene.isLoaded`. That excludes prefab assets and includes inactive/pooled objects. Entities are
sorted by (name, GameObject instance id):
```
u16 n; per entity:
  str16 name            gameObject.name
  i32 instance_id       gameObject.GetInstanceID()
  u8  active            gameObject.activeInHierarchy
  i32 hp
  u8  is_dead
  u8  invincible        HealthManager.IsInvincible
  f32 pos_x, pos_y, scale_x
  f32 vel_x, vel_y      Rigidbody2D.velocity if present, else 0
  f32 rot               v2 only: Rigidbody2D.rotation (degrees) if present, else transform.eulerAngles.z;
                        what the solver sees
  f32 rot_t             v2 only: transform.eulerAngles.z; what the colliders and the observation follow
                        (differs from rot on a body with a frozen rotation whose transform an FSM turns)
  ANIM sub-block
  u16 n_fsm; per PlayMakerFSM in GetComponentsInChildren<PlayMakerFSM>(true):
    str16 owner_path    transform path relative to the entity root ("" = root)
    str16 fsm_name
    str16 active_state  ("" if none)
    u8  enabled         isActiveAndEnabled
    u16 n_var; per variable in FloatVariables, then IntVariables, then BoolVariables:
      str16 name   u8 type (0 float, 1 int, 2 bool)   f32 value
```
Readers report `rot = rot_t = 0` for v1.

### 0x02 FIXED: once per FixedUpdate, from the recorder MonoBehaviour
```
u32 frame  u32 fixed_count  f32 fixed_time
f32 pos_x, pos_y, rb_pos_x, rb_pos_y, rb_vel_x, rb_vel_y      (hero)
```

### 0x03..0x08 HC_FIXED_PRE, HC_FIXED_POST, HC_UPDATE_PRE, HC_UPDATE_POST, HC_LATE_PRE, HC_LATE_POST
Hooks around `HeroController.FixedUpdate` / `Update` / `LateUpdate`, with the same payload as FIXED. The
order of 0x01..0x08 in the stream is a direct record of the frame order (`docs/frame-order.md`).

### 0x09 OBS: the wire payload the trainer receives, at `Hooks.Obs`
```
u8 which (0 reset, 1 step)   u32 reset_index   u32 step   u32 frame   u32 len   u8[len] BinaryProtocol.Pack(msg)
```
Decode it with `hkpy/obs_codec.py`. The layout is `analysis/specs/obs-wire.md`.

### 0x10 EVENT
```
u8 ev   u32 frame   u32 fixed_count   u8 phase (0 update-coroutine, 1 fixed, 2 late, 3 other/hook)
ev 0  SCENE_LOADED   str16 scene
ev 1  STEP           u32 step  i32[4] action  u8 committed        at Hooks.StepBegin; action as applied (after hard commit)
ev 2  HERO_DAMAGE    str16 source  i32 amount  i32 hazard_type  i32 hp_after   HeroController.TakeDamage, post
ev 3  ENEMY_DAMAGE   str16 owner  i32 attack_type  i32 damage  i32 hp_after    HealthManager.TakeDamage, post
ev 4  FSM_TRANSITION str16 owner  str16 fsm  str16 from  str16 to            Fsm.SwitchState (Fsm.cs; every transition funnels here)
ev 5  FSM_EVENT      str16 owner  str16 fsm  str16 event                      Fsm.Event(FsmEvent), recorded before the call
ev 6  SPAWN          str16 name     an ENTITY became active (or appeared active); emitted right after the FRAME that shows it
ev 7  DESPAWN        str16 name     an ENTITY became inactive; "#<instance_id>" if it disappeared from the list
ev 8  RNG_SEED       i32 seed       where Random.InitState was applied
ev 9  LOG            str16 text
ev 10 EPISODE_END    str16 info     at Hooks.EpisodeEnd
ev 11 RESET_BEGIN    str16 level_requested   at Hooks.ResetBegin
ev 12 SCENE_READY    str16 level    at Hooks.SceneReady
```

## Simulator traces
`hksim_drain` emits the same record stream with the same byte layout (schema v1, so no `rot`/`rot_t`). Its
header's `fields.hero` / `fields.playerdata` / `fields.cstate` lists are the subset the sim models, under
the real names. Its entities carry only the HealthManagers, FSMs and variables it models. `capture.mode` is
`"sim"`. A comparison reads the intersection: a field the game recorded and the sim does not model is
unmodelled, not a divergence. `hkpy/sim_driver.py run_corpus` writes the file header.

## Capture points (where TrainingEnv raises `Hooks`)
- `RaiseResetBegin`: top of `Reset()`.
- `RaiseSceneReady`: end of `Reset()`, after the boss refs are bound and `Time.timeScale = 0`, before the
  reset reply. The dumpers run here.
- `RaiseStepBegin`: in `Step()`, right after `ActionDecoder.ApplyAction`.
- `RaiseFrame`: in `Step()`, after every `yield return null` of the frame loop.
- `RaiseEpisodeEnd`: in `Step()`, once the episode is done.
- `RaiseObs`: immediately before every reset/step reply is sent.

## Side files
Record mode also writes these next to `<base>.hktrace`: `<base>.rngdraws.jsonl` (`RngDrawRecorder`, every
`UnityEngine.Random` call with its site key; read by `gate/build_oracle.py`), `<base>.fsmticks.jsonl`
(`FsmTickRecorder`, which FSMs the game ticked), with `HK_ORACLE_LIFECYCLE=1`, `<base>.lifecycle.gz`
(`docs/engine-lifecycle.md`), and with `HK_ORACLE_STATE=1`, `<base>.hkstate` (`docs/state-record.md`).

## Corpus file (`<name>.corpus.json`)
```json
{"name": "ph_ep00", "level": "GG_Hornet_1", "frames_per_wait": 2, "seed": 40000,
 "provenance": {"dump_sha256": "...", "mod_commit": "...", "mod_sha256": "...",
                "sim_keys": [], "game_env": {},
                "frames_per_wait": 2, "recorder": "tools/record_corpus.py", "play": "policy"},
 "steps": [[1,2,0,0], [2,1,7,1], ...]}
```
`steps[i]` = `[move, dir, action, jump]` (`docs/sim-api.md` Actions). In script mode the mod drives
`init` → `reset{level, frames_per_wait}` → one `action` per step, stopping early on `done`, → `close`. It
then flushes the trace and quits. Its recording is `<name>.a.hktrace`. The gate's corpora are
`analysis/polbat_<SCENE>/`.

`provenance` is written by the recorders (`tools/record_corpus.py`, `tools/rerecord.py`) and checked by
every gate (`hkpy/provenance.py`): the dump the sim's tables are generated from, the deployed mod's commit
and bytes, the configuration the game ran in (`hkpy/sim_config.py` `sim_keys` and `game_env`, both empty:
the sim and the mod have one configuration), and whether a policy or a script chose the actions (`play`).
A gate refuses a stamped corpus whose dump or mod (`oracle/` at `mod_commit`) differs from this checkout's,
and a recording whose trace header's `oracle_env` lies outside regime R2. A corpus with no stamp, a stamp of
another configuration, or a trace header with `oracle_env` is LEGACY: it was recorded by a mod with opt-in
switches, set or not, and the sim replays it in the one configuration.

## Oracle environment variables
`docs/oracle.md` has the list. The seed that counts is `HK_ORACLE_SEED`; a corpus's `seed` is informational
to the mod.

## Capture conditions
`tools/run_oracle.py` launches a fresh process per trace (a cold load) with `-batchmode -screen-width 64
-screen-height 64 -screen-quality 0 -screen-fullscreen 0 -logFile <path>`, in regime R2 (`docs/frame-order.md`).
