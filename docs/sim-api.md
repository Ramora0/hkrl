# Simulator API

The C ABI in `sim/core/hksim.h`, as the trainer (`train/sim_worker.py`) uses it. `tests/test_contract.py`
checks the stable surface against the built DLL: symbols, ABI version, scene list, wire decode,
determinism, instance independence, and fast path = wire path.

Build: `cmake -S sim -B sim/build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DHKSIM_MODULES=core;hero;phys;fsm;obs"`
and `cmake --build sim/build` produce `sim/build/hksim.dll`; `$HKSIM_DLL` overrides the path everywhere.

## One instance

    hksim_abi_version()                      == 1; refuse to run on a mismatch
    s = hksim_create(&cfg)                   NULL on failure; reason in hksim_last_error(NULL)
    hksim_set_obs_mode(s, HKSIM_OBS_BATCH)   from the next reset/step
    hksim_reset(s, seed)                     scene from its dump, Random.InitState(seed)
    hksim_step(s, action[4], &res)           frames_per_wait frames; 0 or HKSIM_ERR_*
    hksim_obs_batch(sims, n, vocab, &b)      observations of n instances in one call
    hksim_destroy(s)

- **`hksim_config`:** `level` (e.g. `GG_Hornet_1`), `frames_per_wait`, `seed`, `trace` (0 for training).
  Leave the four clock fields at 0; they are for replaying a recorded trace. Changing `level` or
  `frames_per_wait` needs destroy + create.
- **Scenes:** `hksim_scene_count()`/`hksim_scene_name(i)` list the scenes in the build. An unknown level
  fails create with `HKSIM_ERR_UNKNOWN_LEVEL`.
- **`hksim_step_result`:** `done`, then `damage_landed`, `hits_taken` and `hp_healed` (TrainingEnv's
  per-step signals, same units), then `frame`. The sim computes no reward.
- **Episode end:** knight death, `OnBossesDead` or every boss HealthManager dead, or leaving the arena.
  A step after `done` runs no frame. The caller resets.
- **Errors are traps.** A non-zero return means the sim hit something it does not implement
  (`HKSIM_ERR_UNIMPLEMENTED`) or broke an invariant (`HKSIM_ERR_INTERNAL`), and `hksim_last_error(s)`
  names it. The trainer raises and the run stops. Never reset past a trap.

## Actions

`int32 action[4]`, identical to the mod's `ActionDecoder` (`oracle/Game/ProxyController.cs`):

| index | values |
|---|---|
| 0 movement | 0 left, 1 right, 2 none |
| 1 direction | 0 up, 1 down, 2 none |
| 2 action | 0 attack, 1 nail charge (hold), 2 spell, 3 focus (hold), 4 dash, 5 dream nail (hold), 6 super dash (hold), 7 none |
| 3 jump | 0 yes, 1 no |

Holds are hard-committed: `action[2]` is locked for the hold's duration, then forced to one release step.

## Observation (fast path)

`hksim_batch` points at caller-owned, row-major arrays: numpy `(n_sims, cap, feat)`, which the trainer
places in shared memory. Any pointer can be NULL to skip that block. Column order is
`analysis/specs/obs-wire.md`, and `train/observation.py` names the columns.

| array | shape | content |
|---|---|---|
| `combat` | `[n][cap_combat][14]` f32 | rel_x, rel_y, w, h, vel_x, vel_y, is_trigger, gives_damage, takes_damage, is_target, is_invincible, hp_raw, hp_max_raw, anim_phase |
| `combat_kind`, `combat_parent` | `[n][cap_combat]` i32 | vocab ids of the entity kind and of the clip key `<entity>\|<clip>` |
| `terrain` | `[n][cap_terrain][8]` f32 | mx, my, hdx, hdy, npx, npy, dist, is_trigger (knight-relative) |
| `n_combat`, `n_terrain` | `[n]` i32 | rows present; can exceed the cap, in which case the extra rows are not written |
| `global_state` | `[n][33]` f32 | velocity, hp, soul, knight size, 7 ability flags, 9 `can_*` validity flags, 11 hard-commit values |
| `step`, `done` | `[n][3]` f32, `[n]` u8 | damage_landed, hits_taken, hp_healed; done |

Every instance in the call needs `HKSIM_OBS_BATCH`. The trainer takes `step`/`done` from `hksim_step`,
because a pack after an auto-reset describes the new episode.

**Which colliders are rows.** One predicate on the live world at observation time, the same in the sim
(`sim/fsm/runtime/observer.c`) and the mod (`HitboxObserver.Classify`): every Box/Polygon/Edge/Circle
collider whose object is active, however the object came to exist (scene, pool, runtime Instantiate), goes
to the first bucket it matches. **Enemy** (combat rows): its own GameObject has a `damages_hero` FSM or a
`DamageHero` (the test `HeroBox.CheckForDamage` applies, HeroBox.cs:43, :58) and its layer collides with the
knight's HeroBox layer; a disabled collider is an armed row. **Terrain**: layer 8, not a trigger. **Knight**:
the hero's own non-trigger colliders (their size is in `global_state`). **Attack** (combat rows):
`DamageEnemies`, a `damages_enemy` FSM, or a `Damager` with a `Damage` FSM. Rows come in collider order,
Enemy before Attack. `gives_damage` is 1 when an Enemy collider hurts on contact now: it is enabled and its
hit gets past the source-dependent gates of the damage path. `HeroBox.CheckForDamage` passes on the
`damages_hero` FSM's `damageDealt` / `hazardType`, else the `DamageHero`'s, and skips a `DamageHero` that is a
`shadowDashHazard` while the knight shadow dashes (HeroBox.cs:43-62). `TakeDamage` returns at damage 0 or less
(HeroController.cs:1829), and for `hazardType` 1 in `HAZARD_ONLY` damage mode, while shadow dashing, or while
`parryInvulnTimer > 0` (HeroController.cs:1847). `CanTakeDamage` (the knight's own invulnerability,
HeroController.cs:1845) is the same for every row and is not part of it. Reading a `damages_hero` FSM the sim
does not tick traps, as does a Terrain collider the core's terrain tables do not hold (a runtime clone's).

**Vocab.** `hksim_vocab_create(max_size)` (0 means 512). Id 0 is `unknown` (also NULL/""), 1 is `terrain`
(reserved), and later strings get ids in arrival order. Past `max_size`, new strings encode to 0.
Replaying a saved list through `hksim_vocab_intern` reproduces its ids. The trainer owns one canonical list
and seeds every worker's vocab from it. A worker whose vocab grows reports the new strings and re-packs
before publishing.

**Wire path.** `hksim_obs(s, buf, cap)` (`cap = 0` sizes) returns the mod's `BinaryProtocol.Pack` bytes,
decoded by `hkpy/obs_codec.py`. The parity gate verifies this path; the fast path is tested equal to it.

## One configuration

The sim has one behaviour, the same as the mod's in every launch mode, so the trainer, the gate and
`hkpy/sim_driver.py run_corpus` run the same sim. The hold table (`oracle/Game/ProxyController.cs`
`HoldGameSeconds`: nail charge 1.71 s, focus 1.51 s, dream nail 1.09 s, super dash 0.91 s, in 0.02 s
frames), the focus action holding Cast (Action2, so it heals), and the observation's membership (above:
armed rows included, runtime clones observed) are fixed in both.

## `hksim_set_value` keys the trainer sets

- **Once per instance:** none. `hkpy/sim_config.py` is the one place the configuration comes from (`sim_keys`,
  applied after the first reset by the trainer, every gate replay, every soak and the fingerprint), and it
  sets nothing: the sim has one configuration (above).
- **Per training episode,** after `hksim_reset`: `hero.pd.maxHealth`, `hero.pd.health`, then `hp.resync`,
  which re-takes the hit/heal baseline. These are episode setup; eval plays the game's 9/9.
- The other keys (`hero.f.*`, `hero.cstate.*`, `hero.rb_*`, `rng.s0..s3`, `input`) are for verification.

## Determinism and threads

- **Seeded only.** The same seed and actions give the same bytes. The trainer keeps one numpy stream per
  global env index (`seed_stream(seed, env_id)`), and each reset draws a seed in `[1, 2^31-1]`.
- Instances stepped round-robin in one process are independent. `sim/` holds process-global scratch, so
  use **one thread per process** and scale with worker processes (`train/sim_env.py`).
- All game quantities are binary32. The flags in `sim/CMakeLists.txt` are part of the contract
  (`docs/float-parity.md`).

## Checkpoints

`hksim_checkpoint_new(s)` captures the instance, `hksim_checkpoint_save(s, c)` re-captures into the same
object, `hksim_checkpoint_restore(s, c)` returns to it, and `hksim_checkpoint_free(c)` releases it. The
instance's whole state is one arena that never moves (`sim/core/alloc.h`), so save and restore are each a
memcpy of the arena's used bytes (`hksim_checkpoint_bytes`), and a checkpoint restores only into the
instance that made it. A restore undoes everything since the save: steps, resets, `hksim_set_value`
writes and runtime pool growth. The caller's `hksim_vocab` is not instance state and keeps its ids.
`tests/test_checkpoint.py` replays NKG byte for byte after a restore, including into an instance whose
pools have grown past the checkpoint's.

## Internal surface and `hkpy/sim_driver.py`

Verification only: `hksim_frame`, `hksim_drain`/`hksim_trace_header` (`docs/trace-format.md`),
`hksim_get_value`, the RNG controls, and `hksim_checkpoint_read` (a checkpoint's raw bytes).

`hkpy/sim_driver.py` holds `DLL` (`sim/build/hksim.dll` or `$HKSIM_DLL`), `load()` (the ctypes
signatures), `Config`, `StepResult`, `Batch`, `BatchBuffers` (numpy arrays with a `Batch` over them) and
`obs_batch()`. For replays it adds `replay()` / `replay_episode()` (a recorded episode, the way every gate
replays it), `run_corpus()` (writes the sim's `.hktrace`), `trace_clocks()`,
`seed_from_trace()` and `drain()`, and `checkpoint_bytes()` for comparing two saves. `HKSIM_RNG_ORACLE` /
`HKSIM_RNG_MODE` select the replay's RNG mode.
