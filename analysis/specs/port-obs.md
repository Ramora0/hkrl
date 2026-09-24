# port-obs — P6 observation packer (`sim/obs/`, worker O)

Owner: worker O.  Files: `sim/obs/obs.h`, `sim/obs/obs.c`, `harness/tests/test_obs.py`, this document.
Spec: `analysis/specs/obs-wire.md` (citation keys reused here: `BP:` `SE:` `HO:` `TE:` `PC:` `PR:` `TR:`).
Build: `cmake -S sim -B sim/build-o -G Ninja -DHKSIM_MODULES="core;obs" && cmake --build sim/build-o`;
test: `HKSIM_DLL=sim/build-o/hksim.dll python harness/tests/test_obs.py`.

## 1. Design

- `obs.h` is a plain-data **view** the core fills; it includes only `core/hksim.h` and `core/scene_GG_Hornet_1.h`
  (for `hk_scene_def`).  Field names are the C# identifiers they transcribe (§6 lists them).
- `obs_pack_step` / `obs_pack_reset` write the exact `BinaryProtocol.Pack` bytes (`BP:32-176`) into a caller buffer;
  they return the full length and never write past `cap` (`cap = 0` sizes the reply).  `obs_pack_ack` produces the
  one-byte `init`/`pause`/`resume` replies; `obs_unpack_request` parses `BP:179-205`.
- **Terrain rows are computed inside the packer** from the compiled statics (`hk_scene_def.statics`, scene order) and
  `hero.knightPos_*`, per `HO:416-518` / `HO:803-822` (obs-wire.md §4).  `obs_terrain_rows` exposes the same walk for
  tests and for the glue.  `terrain_debug` strings are generated as `"|seg_idx=<i>"` (`HO:517`, `eval = false`);
  `eval_mode = 1` traps (`BuildTerrainDebug` is engine queries, obs-wire.md §4.5).
- Helpers with the cited float rules so the glue does not re-derive them: `obs_combat_fill` (`HO:753-797`),
  `obs_global_state` (`SE:86-96`), `obs_box_bounds_size` (Q-pobs-5), `obs_damage_landed_accumulate` (`TE:1183`, UNVERIFIED).
- A `done` step packs the `TE:735-746` shape (empty sets, 33 zeros, empty fsm block, real scalars/diag/info) from the
  same view; the "step after done" (`TE:573-589`: zero rewards/times, diag zeros, `action_committed` 0) is the glue's
  job — it fills zeros.
- Traps (`HKSIM_UNKNOWN`) for the open cases: rotated/scaled terrain transforms (Q-pobs-1), circle terrain
  (Q-pobs-2), multi-path polygon terrain (Q-pobs-3), `eval_mode`.

## 2. Terrain geometry evidence

- Membership walk: statics in `scene_order` (asserted monotone; obs-wire.md Q-obs-1), `layer == 8 && !isTrigger`
  (`HO:111-113`), `isActiveAndEnabled` = `statics[i].active` (gen_scene: `activeInHierarchy && enabled`) or the
  optional `terrain_active` override, `!usedByComposite` (`HO:808-810`), shape filter (`HO:104`).  Enemy-bucket
  precedence (`HO:107-109`) needs no handling: no layer-8 non-trigger collider in any of the four dumps carries
  `DamageHero`/`damages_hero` (`components` scan of `dumps/*/scene.json`).
- `Transform.TransformPoint` [ENGINE]: for identity rotation/scale, `world = fl32(local + offset) + position` matches
  `dumps/GG_Hornet_1/scene.json#colliders[*].world` bit-for-bit on all 54 polygon/edge terrain points and all
  28 box corners (`SceneDumper.cs:28` computes `world` with the same `TransformPoint` call).  All 14 GG_Hornet_1
  terrain colliders are identity; the other three dumps have scaled ones (Q-pobs-1).
- Result: 64 rows per payload, 34 688 / 34 688 rows (277 504 floats) bit-exact over both traces, including the
  `−0.0f` values (`hdx`/`hdy` after the `HO:496-500` flip) and the `1e-5`-scale `npx`/`npy` residuals — but only
  under the float model of §4.

## 3. Test protocol (`harness/tests/test_obs.py`) and parity

Per OBS record: decode the real payload with an independent transcription of `BP:32-176` (asserts full consumption;
cross-checked against `FullKnight/python/binary_protocol.py` imported read-only by path, 10/10 sampled payloads
agree on every field); fill the view; pack with the DLL; compare.

View sources: FRAME of the OBS frame → `knightPos` (`hero.pos_*`), `rb2d_velocity`, `pd.health/MPCharge/has_*`,
knight bounds via `obs_box_bounds_size` from `hero.cols`; STEP events → the ActionDecoder commit machine
(`PC:281-362`, transcribed in the test; `committed` matched the recorded flag 540/540) → `shim_*`; `action_committed`
from the STEP event; `step_game_time` = float32 Σ `FRAME.dt` (`TE:619`); `hits_taken` = `pd.health` drop over the step
(§5.3); `hp_healed` from the health delta (`TE:699-701`).  Copied from the payload (not modelled here): combat rows +
kind/clip strings, the nine `Can*` flags, fsm snapshots, diag, reset-phase telemetry, `step_real_time`.  For the reset
payload (no FRAME before SCENE_READY's OBS) the six continuous globals are copied and `knightPos` is taken from the
following frozen-frame pose record.

| check (2026-08-31, build-o) | r2_move.a | r2_rand1.a |
|---|---|---|
| payloads (reset + steps) | 301 | 241 |
| length equal | 301/301 | 241/241 |
| bytes identical after the obs-wire.md §5 mask | 301/301 | 241/241 |
| bytes identical **unmasked** | 301/301 | 241/241 |
| envelope (type, n_combat, n_terrain) | 301/301 | 241/241 |
| terrain rows bit-exact (own geometry) | 19 264/19 264 | 15 424/15 424 |
| terrain floats bit-exact | 154 112/154 112 | 123 392/123 392 |
| global-state floats | 9 933/9 933 | 7 953/7 953 |
| of which sim-derived (idx 0–12, 22–32) | 7 224/7 224 | 5 784/5 784 |
| step scalars: damage_landed / hits_taken / step_game_time / hp_healed / done / action_committed | 300/300 each | 240/240 each |
| info, diag bytes, kinds, parents, terrain_debug, fsm, reset_branch | all | all |
| `obs_terrain_rows` export (reset pose) | 64/64 rows, inventory 4+11+4+4+4+7+7+5+6+5+7 | — |
| request parser vs `pack_reset/pack_action/pack_init/pause/resume` | ok | — |

`hero_damage_events_eq_hits` (informational): 296/300, 220/240 — see §5.3.

## 4. Finding: the mod's float arithmetic runs on Mono's double evaluation stack

Per-op float32 (the `docs/float-parity.md` contract) reproduces only 42 of the 64 terrain rows of the reset payload;
every mismatch is in `npx`/`npy`/`dist` (`HO:508-514`), e.g. Bounds Cage bottom edge `npx = −2.638e-05` on the
wire where float32 gives `0`, a value that is not a multiple of the float32 ULP of the operands.  A model search
(scratch script, 31 payloads × 64 rows) over evaluation models of `HO:485-514`:

| model | rows exact |
|---|---|
| every operation rounded to float32 | 1 390 / 1 984 |
| compound expressions rounded once, `t` numerator rounded to float32 before the division | 1 860 / 1 984 |
| **every expression evaluated in double, rounded to float32 only at stores to float locals / float call arguments** | **1 984 / 1 984** |

The winning model is ECMA-335 III.1.1.1 (`F` evaluation-stack type) as implemented by Unity's Mono JIT without the
`float32` optimisation [ENGINE]: `denom = dxs*dxs + dys*dys`, `t = (−ax*dxs + −ay*dys)/denom` (double numerator **and
double division**), `npx = ax + t*dxs`, and the `npx*npx + npy*npy` argument of `Mathf.Sqrt` are each computed in
double and rounded once; every `float` local (`ax`…`t`, `npx`, `npy`) and the `Mathf.Sqrt(float)` argument is
rounded.  FMA contraction alone cannot explain it (the double division is required).  `obs.c:emit_segment` implements
this model and is bit-exact on every row of both traces.  `SE:74` (`1f − left/total`) is written the same way and
matches all 232 committed steps of r2_rand1.a (the two models agree on those operands; the double form is the
measured semantic).  Single-op expressions are unaffected (double rounding of one +,−,×,/ is innocuous), so the
existing per-op float code elsewhere is only wrong where a C# statement contains **two or more** float operations
before a store.  **Implication for workers H/F/P and `docs/float-parity.md`**: "no double intermediates unless the
decomp does" is the wrong contract for ported C# — compound expressions in `HeroController`, PlayMaker actions and
`HealthManager` must be written with double intermediates and one `(float)` cast per C# store (or the equivalent
`fma`/double sequence), and the corpora that passed bit-exactly must be re-read for compound expressions that
happened to coincide.  See Q-pobs-4 for the evidence that would confirm the rule outside `HitboxObserver`.

## 5. Findings

5.1 (§4) Mono double evaluation stack — the wire is not reproducible with per-op float32.
5.2 `knight_w`/`knight_h` are not the constants 0.5 / 1.28125: `Collider2D.bounds.size` is an engine AABB of the
    transformed box vertices, `fl(p + (o + h/2)) − fl(p + (o − h/2))`; r2_rand1.a step 177 carries
    `knight_w = 0.4999990463` (`pos_x` such that the two rounded corners are `2^-20` short of 0.5).  The AABB formula
    reproduces 540/540 payloads; `obs_box_bounds_size` implements it (Q-pobs-5).  The core must fill `knightW/H`
    from it every payload, not from the collider size.
5.3 The recorder's `HERO_DAMAGE` event hooks `HeroController.TakeDamage` **entry** (`TR:141-163`), i.e. every contact
    call including those rejected by the i-frame gates (`analysis/specs/damage-path.md` §1), while `hits_taken` counts
    only applied damage (`ModHooks.AfterTakeDamageHook`, `TE:1161-1170`).  In r2_move.a steps 144-147 the event sum
    is 2/2/2/1 against `hits_taken` 1/0/0/0 (one hit, then contacts during invulnerability).  The harness therefore
    derives `hits_taken` from the `PlayerData.health` drop (exact while nothing heals); a post-gate event
    (`AfterTakeDamageHook`) in the recorder would make it a direct check.  Not a wire issue.
5.4 Scaled terrain transforms: the naive `fl((local+offset)·lossyScale) + position` mismatches the dumped `world`
    points by 1 ULP on 9/122 (GG_False_Knight), 3/66 (GG_Gruz_Mother), 2/91 (GG_Mega_Moss_Charger) points, including
    axes with `lossyScale = 1.0` (nested scaled parents) → the engine's matrix path is not the naive formula.  The packer
    traps on `rot_deg != 0 || sx != 1 || sy != 1` (Q-pobs-1).  No rotated terrain collider exists in the four dumps.
5.5 No layer-8 non-trigger `CircleCollider2D` in the four dumps; the 12-gon path (`HO:463-480`) is transcribed but
    trapped (Q-pobs-2).  No multi-path terrain polygon in the four dumps (Q-pobs-3).
5.6 `−0.0f` on the wire (obs-wire.md §4.2) is produced by the C `-hdx` negation and preserved by the bit-copy writer.
5.7 The commit machine transcription (`PC:281-362`, `LockedStepsFor` with `0.00848f`) reproduces every `action_committed`
    flag and every `gs[22..32]` value of both traces (89/30/177/59 locked steps under fpw = 2).
5.8 `step_game_time` = float32 running sum of `FRAME.dt` matches 540/540 (`0x3D23D70A` under R2).

## 6. View fields the core must fill (`obs_view`)

`hero` (`obs_hero_view`):
- `knightPos_x/y` — `HeroController.instance.transform.position` (`HO:711`), sampled at the capture point (obs-wire.md §1.6).
- `knightW/H` — `obs_box_bounds_size(pos, hero collider offset/size)` (§5.2), last active non-trigger hero collider (`HO:746-750`).
- `rb2d_velocity_x/y` — `HeroController.rb2d.velocity` (`SE:33-36`).
- `pd_health`, `pd_MPCharge` — `PlayerData` ints (`SE:37-38`).
- `pd_hasDash, pd_canWallJump, pd_hasDoubleJump, pd_hasSuperDash, pd_hasDreamNail, pd_hasAcidArmour, pd_hasNailArt` (`SE:41-47`; `PD:` constants, all true).
- `CanJump, CanDoubleJump, CanWallJump, CanDash, CanAttack, CanCast, CanNailCharge, CanDreamNail, CanSuperDash` —
  the nine predicates **called** at the capture point in `SE:50-58` order (`CanJump` clears `ledgeBufferSteps`, hero-motion.md §4).
- `shim_CState` (0/1/2), `shim_LockedAction`, `shim_LockedStepsLeft`, `shim_LockedStepsTotal` — `InputDeviceShim` after this step's `ApplyAction` (`PC:42-49`).

`combat` / `n_combat` — one `obs_combat_row` per active Enemy then Attack collider in scene order (`HO:19`, obs-wire.md
§3.8); fill the 14 floats with `obs_combat_fill(row, src, &hero)` where `obs_combat_src` carries: `bounds_center/size`
(world AABB; for unrotated boxes use `obs_box_bounds_size`), `isTrigger`, `bucket_is_enemy`, `has_hm` + `is_boss_hm` +
`hm_IsInvincible` + `hm_hp` + `hm_max_hp` (`ObserveMaxHp` cache: max(first seen, current), per HM, reader lifetime),
`has_prev_rel` + `prev_rel_x/y` (previous payload's `relX/relY` for the same collider iff it was emitted on the previous
non-done payload; cleared on fake reset), `has_clip` + `anim_CurrentFrame` + `clip_frames_Length`; plus `kind`
(`Strip(name)` of the nearest `tk2dSpriteAnimator` owner ≤ 7 ancestors, else own name, `"unknown"` if empty) and
`clipKey` (`Strip(animOwner) + "|" + clip.name`, `"none"` without animator/clip).

`scene` — `hk_scene_GG_Hornet_1()`; `terrain_active` — NULL, or per-static `isActiveAndEnabled` if an FSM ever toggles
terrain; `eval_mode` — 0.

`fsm_snapshots` / `n_fsm` — `"<src>|<owner>|<fsm>|<state>"` strings in `FO:67-131` order (obs-wire.md §3.7).

`step` (`obs_step_view`): `damage_landed` (accumulate with `obs_damage_landed_accumulate`, `TE:1183`), `hits_taken`
(int, applied knight damage after the i-frame gates), `step_game_time` (float32 Σ dt), `step_real_time` (any; masked),
`hp_healed` (`max(0, Δhealth)` if not done), `done`, `action_committed`, `info` (`""`, `"win"`, `"loss"`,
`"fake_reset_boss"`, `"fake_reset_knight"`), `diag_*` (masked; kind-cache size = colliders ever classified).

`reset` (`obs_reset_view`): `reset_branch` (0 workshop / 1 natural_end / 2 unknown, `TE:388-415`; the fake path is 2),
`reset_phase_ms/frames[7]` (masked).

## 7. Questions

### Q-pobs-1 — `Transform.TransformPoint` for scaled/rotated terrain transforms
Identity transforms are pinned (§2).  For `lossyScale ≠ 1` the naive per-axis formula misses by 1 ULP on 14 of 279
dumped points across the three non-Hornet scenes (§5.4); rotation is unobserved.  The packer traps.  **Evidence that
would close it:** the exact float sequence of Unity's `TransformPoint` (local-to-world matrix build from the
hierarchy: which factors are multiplied first, whether the child matrix is composed or the world TRS re-derived) —
testable against `SCENE#*.world` of the scaled colliders in `GG_False_Knight`, `GG_Gruz_Mother`, `GG_Mega_Moss_Charger`
and, once P6 runs on those scenes, their OBS rows.

### Q-pobs-2 — `Mathf.Cos/Sin` libm parity for circle terrain (12-gon, `HO:463-480`)
`Mathf.Cos` = `(float)Math.Cos((double)ang)` [ENGINE]; Mono's `Math.Cos` calls the C runtime, mingw's `cos` may differ
in the last double bit, which changes the float result in rare cases.  No layer-8 non-trigger circle exists in the four
dumps, so the path is transcribed and trapped.  **Evidence that would close it:** a scene with a circle terrain
collider (dump `world` points are not enough — they come from `TransformPoint`, not from the 12-gon) and its OBS rows.

### Q-pobs-3 — multi-path terrain polygons
`HO:434-449` walks every path; the compiled table carries path 0 only (`gen_scene.py`, `unsupported = 2` flag).  None
in the four dumps.  **Evidence that would close it:** none needed unless a scene has one — then extend `gen_scene.py`
(orchestrator-owned) to emit all paths.

### Q-pobs-4 — Scope of the Mono double-evaluation-stack rule (§4) outside `HitboxObserver`
Measured on `HO:485-514` (34 688 rows) and consistent with `SE:74`.  Whether Unity's Mono applies the same to
`HeroController`/PlayMaker/`HealthManager` methods (it should — same JIT, same IL) is what the other ports depend on.
**Evidence that would close it:** one compound float statement in the hero decomp whose per-op-float32 and
double-stack results differ on a traced frame (e.g. `HC` velocity/timer arithmetic of the form `a + b*c` or `x*y/z`),
checked against the trace; or the Unity Mono build flags (`mono_set_defaults` / `MONO_FLOAT32`) for 2020.2.2f1.

### Q-pobs-5 — `Collider2D.bounds.size` for the knight (§5.2)
The transformed-vertex AABB formula reproduces 540/540 payloads for an unrotated, offset box under scale ±1; not
verified for combat boxes (`Hornet Boss 1` body: the same formula should hold), scaled circles (`Sphere Ball`) or
rotated polygons (`Hit ADash`) — obs-wire.md Q-obs-4.  **Evidence that would close it:** recompute `w, h, rel_x, rel_y`
of every combat row of both traces from ENTITY pose + collider geometry with the formula (needs the per-frame collider
size/scale that worker F models) and compare.

### Q-pobs-6 — `hits_taken` cannot be checked from the recorder's `HERO_DAMAGE` (§5.3)
The harness uses the health delta, which is exact only while nothing heals (no focus in the corpus).  **Evidence that
would close it:** a recorder event at `ModHooks.AfterTakeDamageHook` (or reading `_hitsTakenInStep`), or a corpus with
focus healing to exercise `hp_healed` and the delta ambiguity.

## 8. Sim-vs-real trace parity tool (`harness/obs_parity.py`, 2026-08-31)

- `harness/obs_codec.py` now holds the BP:32-176 decoder + the §5 mask (moved out of `test_obs.py`, which imports it).
- `harness/obs_parity.py real.hktrace sim.hktrace` pairs OBS records by `(which, step)` (occurrence order for
  repeats; frames are not compared), decodes both, and reports per section — envelope / terrain / global_state /
  combat (+ order-only count, Q-obs-1) / scalars (step_real_time and reset ms/frames masked) / fsm / info /
  terrain_debug (eval-mode text skipped) / bytes (§5 mask, diag block masked entirely) — the number of matching
  payloads and the first mismatch (step, field, real vs sim).  A sim with fewer OBS records is compared over the
  common prefix (`STOP(paired/real)`).  `--corpus-dir analysis/traces/p0 --only r2_` prints one line per
  `<name>.a.hktrace` / `<name>.sim.hktrace` pair; `--verbose` adds every section's first mismatch.  Exit 1 on any
  mismatch.  Self-test `harness/tests/test_obs_parity.py`: (real, real) 301/301 in every section; one flipped
  terrain float → exactly one `terrain` (+`bytes`) mismatch at the right step/field; one flipped `gs[24]` → one
  `global_state` mismatch; a flipped `step_real_time` → no mismatch; a sim truncated after step 50 → prefix 51/51;
  swapped combat rows → flagged order-only.
- Current full-stack picture (sim traces of commit fe1fa27, before worker F's combat rows / fsm snapshots):

| corpus | paired | terrain | global_state | scalars | note |
|---|---|---|---|---|---|
| r2_idle | 121/121 | 121 | 121 | 120 | only `reset_branch` differs |
| r2_move | 301/301 | 235 | 144 | 299 | first divergence step 144 (`vel_x` 0 vs 8.3 = hero DH); terrain rows follow knightPos |
| r2_walk | 241/241 | 141 | 140 | 237 | |
| r2_attack | 241/241 | 132 | 131 | 237 | |
| r2_dash | 241/241 | 88 | 88 | 237 | |
| r2_jump | 241/241 | 26 | 25 | 236 | |
| r2_rand1/2/3 | 103–104/241 | 98–104 | 97–104 | 101–103 | sim stops early (STOP) |

  `envelope`, `combat`, `fsm`, `bytes` are 0/N everywhere because the sim emits `n_combat = 0` and no fsm
  snapshots yet (worker F); `info` and `terrain_debug` are N/N.

5.9 **Finding for sim/core/sim.c**: `reset_branch` is packed as `OBS_RESET_BRANCH_UNKNOWN` (2) but every real
    reset in the corpus carries 0 (`BranchWorkshop`: the full path starts in `GG_Workshop`, `TE:388-415`); the fake
    fast path is the one that reports 2 (obs-wire.md §6.4).  One-line fix in `build_obs` (reset branch = 0 for a
    scene load from the workshop, 2 for a fake reset).
