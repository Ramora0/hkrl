# Observation wire format (P6 — observation parity)

Scope: everything the Python trainer receives from the mod over the WebSocket, byte by byte, and every
rule that decides the value of every byte: message envelope, the 33-float global state, combat rows,
terrain rows, string channels, telemetry trailers, reward signals, done/reset semantics. The sim's
`hksim_obs` (`sim/core/hksim.h:76-77`) must emit these bytes; the trainer (`FullKnight/python`)
runs unmodified. Target scene for the verification numbers: `GG_Hornet_1`, regime R2 (`STATE.md`).

Reference implementation: the **oracle fork** `oracle/` (a copy of the training mod). Verified
2026-08-31 with `diff -w` after normalising the `HKOracle`→`FullKnight` namespace: `Net/BinaryProtocol.cs`,
`Net/Protocol.cs`, `Game/StateExtractor.cs`, `Game/HitboxObserver.cs`, `Game/FsmObserver.cs` are
**byte-identical** to `C:\Users\Lee\coding\CSharp\HK\FullKnight\` (read-only); `Game/ProxyController.cs`
differs only by the oracle's added `KeyNames`/`KeyBits` (`PC:20-28`, not on the wire);
`Environment/TrainingEnv.cs` differs only by the `Oracle.Hooks.Raise*` calls and the
`HK_ORACLE_CAPTURE_DT` override of `kStepDeltaTime` (`TE:125-133` vs FullKnight's `const 0.00848f`),
neither of which changes a wire byte. See Q-obs-6 for the drift policy.

Citation keys:
- `BP:<n>` = `oracle/Net/BinaryProtocol.cs:<n>`; `PR:` = `oracle/Net/Protocol.cs`; `WS:` = `oracle/Net/WebsocketEnv.cs`
- `TE:` = `oracle/Environment/TrainingEnv.cs`; `SE:` = `oracle/Game/StateExtractor.cs`; `HO:` = `oracle/Game/HitboxObserver.cs`
- `PC:` = `oracle/Game/ProxyController.cs`; `FO:` = `oracle/Game/FsmObserver.cs`; `TR:` = `oracle/Oracle/TraceRecorder.cs`; `HK:` = `oracle/Oracle/Hooks.cs`
- `FullKnight/python/<file>:<n>` = `C:\Users\Lee\coding\CSharp\HK\FullKnight\python\<file>:<n>` (trainer side, read-only)
- `D:<file>:<n>` = `analysis/decomp/Assembly-CSharp/<file>:<n>`; `PM:<file>:<n>` = `analysis/decomp/PlayMaker/<file>:<n>`
- `SCENE#<i>` = `analysis/dumps/GG_Hornet_1/scene.json` → `colliders[<i>]`; `FSM#<path>::<fsm>` = `analysis/fsm/GG_Hornet_1.json`
- `PD:<name>` = `analysis/dumps/GG_Hornet_1/playerdata.json` → `fields[name=<name>].value`
- `T:<trace>` = `analysis/traces/p0/<trace>`; decoded with `harness/hktrace.py` + `FullKnight/python/binary_protocol.py`
  (two scratch decoder scripts in the session scratchpad, not committed)

Rules: every value below is a transcription of the cited line. Trace numbers in §0 and §9 verify;
they define nothing. Engine facts (Unity/Mono/.NET behaviour with no decomp) are tagged **[ENGINE]** and
collected in §8.

---

## 0. Verification summary (2026-08-31)

Decoded every `OBS` record (`docs/trace-format.md` §0x09; payload = `BinaryProtocol.Pack(msg)`,
`TR:409`) of `T:r2_move.a.hktrace` and `T:r2_rand1.a.hktrace` with (a) an independent transcription of
`BP:32-176` and (b) the trainer's own `FullKnight/python/binary_protocol.py:unpack_reset/unpack_step`:

| check | r2_move.a | r2_rand1.a |
|---|---|---|
| OBS records (reset + step) | 1 + 300 | 1 + 240 |
| independent parser consumes exactly `len` bytes | 301/301 | 241/241 |
| FullKnight decoder agrees on every field (arrays, strings, scalars, trailers, side channels) | 301/301 | 241/241 |
| step `OBS.frame == max FRAME.frame` of that step | 300/300 | 240/240 |
| combat `vel = rel(t) − rel(t−1)` for rows matched by unique kind | 324/324 | 254/254 |
| `gs[0:2] == FRAME.hero.rb_vel`, `gs[2] == pd.health`, `gs[3] == pd.MPCharge`, `gs[4:6] == (0.5, 1.28125)` | all sampled steps | all sampled steps |
| terrain rows | 64 every step; order + geometry identical 300/300 | 64 every step |
| terrain rows ↔ `scene.json` layer-8 colliders | 64/64 mapped (§4.6) | — |
| byte totals | reset 3185, step 3179 (§1.5 arithmetic) | reset 3185, step 3179 |

`(n_combat, n_terrain)` histogram — move: `(1,64)×257 (2,64)×40 (3,64)×4`; rand1:
`(1,64)×193 (2,64)×42 (3,64)×2 (4,64)×4`. Kind strings seen: `Hornet Boss 1`, `Sphere Ball`, `Needle`,
`Slash`, `AltSlash`, `UpSlash`, `DownSlash`, `Cyclone Slash`. Hornet row: `rel = ENT.pos − hero.pos +
(0.148401, −0.968784)` = the body collider offset (`analysis/specs/boss-hornet.md` §1 table, `SCENE#1237`); `w,h` =
`(1.393576, 1.156178)` at spawn and one size per FSM state thereafter (`analysis/specs/tk2d-animator.md` §4.2).

---

## 1. Transport and envelope

### 1.1 Connection
- The mod is the WebSocket **client**; the trainer is the server. URL `ws://localhost:8765`
  (`oracle/HKOracle.cs:11-13`, env override `FK_SERVER_URL`; FullKnight identical `FullKnight/FullKnight.cs:13`);
  server `FullKnight/python/config.py:20-21` (`server_host="localhost"`, `server_port=8765`), `FullKnight/python/vec_env.py:42-44`.
- Outbound frames are **binary** (`WS:40-41` `base.Send(byte[])`). Inbound: binary → `BinaryProtocol.Unpack`,
  text → JSON `Message` (`WS:27-30`); the trainer only sends binary (`FullKnight/python/env.py:46,56,84,111,115,119`).
- Strict request/reply: one reply per request, matched by receive order, no ids (`WS:93-99`). Queued
  `reset`s are coalesced to the newest (`WS:100-112`).
- Sequence per env: `init` → (`reset` → `action`* [→ `pause`/`resume` around trainer updates])* → `close`
  (`FullKnight/python/env.py:44-119`; `FullKnight/python/vec_env.py:62` calls `init()` on connect, `:303-308` `pause_all/resume_all`).

### 1.2 Requests (Python → C#), `BP:179-205`, little-endian, no padding (`FullKnight/python/binary_protocol.py:34-47`)

| msg | bytes | cite |
|---|---|---|
| `init` | `u8 0` | `BP:9`, `FullKnight/python/binary_protocol.py:25` |
| `reset` | `u8 1`, `i32 frames_per_wait`, `i32 time_scale`, `u8 eval`, `u8 force_full`, `u16 len`, `len × u8` UTF-8 `level` | `BP:189-196`, `FullKnight/python/binary_protocol.py:27-38` |
| `action` | `u8 3`, `4 × i32 action_vec` = `[move, dir, action, jump]` | `BP:197-201`, `FullKnight/python/binary_protocol.py:40-41`, vocabulary `PC:302-309` |
| `pause` / `resume` / `close` | `u8 4` / `u8 5` / `u8 6` | `BP:13-15`, `FullKnight/python/binary_protocol.py:43-47`, `FullKnight/python/env.py:119` |

`time_scale` is unpacked (`BP:191`) but **never read** by `TrainingEnv` (no use of `data.time_scale`;
`Time.timeScale` is pinned to 1/0 at `TE:373,596,627,492`). `eval` → `_evalMode` (`TE:291`) whose only
wire effect is the terrain-debug strings (§4.5). `force_full` → `TE:232-239` (§6.4).

### 1.3 Replies (C# → Python), `BP:32-176`
`Pack` writes the type byte (`BP:37`) and then, **only for `step` and `reset`** (`BP:40`), the body below.
`init`, `pause`, `resume` replies are therefore exactly **one byte**: `0x00`, `0x04`, `0x05`
(the env echoes the request message: `TE:1118`, `TE:162`, `TE:171`). `close` gets **no reply**
(`TE:141-143` sets `_terminate`; `WS:114-116` → `Dispose` `TE:1121-1138` → socket closed).

Encoding facts used by every table below: .NET `BinaryWriter` little-endian **[ENGINE]** — confirmed by
the trainer decoding with `<` formats throughout (`FullKnight/python/binary_protocol.py:55,59,63,66,112,186,191,230,242,288`)
and by §0; strings are UTF-8 (`BP:78,89,102,153,168`); `float` = IEEE-754 binary32; bools are `u8 0/1`
(`BP:69-70`). Null fields default to empty/zero (`BP:42-46,64-70,98,115-119,130-132,147,167`).

### 1.4 `step` reply body (type `2`)

`nc = n_combat`, `nt = n_terrain`, `A = 5 + 56·nc`, `B = A + 32·nt`, `C = B + 132`.

| offset | size | type | field | source of value | cite |
|---|---|---|---|---|---|
| 0 | 1 | u8 | type id = 2 | `TypeToId["step"]` | `BP:11,37` |
| 1 | 2 | u16 | `n_combat` | `combat.Count` | `BP:48` |
| 3 | 2 | u16 | `n_terrain` | `terrain.Count` | `BP:49` |
| 5 | 56·nc | f32[nc][14] | combat rows, row-major, row length = the array length (14, §3.3) | `HO:793-797` | `BP:51-53` |
| A | 32·nt | f32[nt][8] | terrain rows (8, §4.2) | `HO:516` | `BP:55-57` |
| B | 132 | f32[33] | global state (§2) | `SE:86-96` | `BP:59-60`, `SE:11` |
| C | 4 | f32 | `damage_landed` (§6.1) | `TE:704` | `BP:64` |
| C+4 | 4 | f32 | `hits_taken` — `int` cast to float | `TE:705` | `BP:65` |
| C+8 | 4 | f32 | `step_game_time` = Σ `Time.deltaTime` over the step's frames | `TE:619,628` | `BP:66` |
| C+12 | 4 | f32 | `step_real_time` = Σ `Time.unscaledDeltaTime` | `TE:620,629` | `BP:67` |
| C+16 | 4 | f32 | `hp_healed` (§6.1) | `TE:706` | `BP:68` |
| C+20 | 1 | u8 | `done` | `TE:737` / `TE:760` | `BP:69` |
| C+21 | 1 | u8 | `action_committed` (§2.4) | `TE:600` | `BP:70` |
| C+22 | var | nc × (u8 len, len × u8) | `combat_kinds[i]`; `len = min(255, utf8len)`, truncated; missing → `"unknown"` | `HO:798` | `BP:75-82` |
| … | var | nc × (u8 len, bytes) | `combat_parents[i]` = the **clip key** (§3.5); missing → `""` | `HO:799` | `BP:86-93` |
| … | var | nt × (u16 len, bytes) | `terrain_debug[i]`; cap 65535; missing → `""` | `HO:517` | `BP:98-106` |
| … | 14 | u16 u16 u16 i32 f32 | diag: `enemy_count, attack_count, terrain_count, kind_cache_size, gc_heap_mb` | `TE:716-721` | `BP:113-120`, `FullKnight/python/binary_protocol.py:135-136` |
| … | 2 + Σ(2+len) | u16 count, count × (u16 len, bytes) | `fsm_snapshots` (§3.7); count and each len capped 65535 | `TE:759` | `BP:147-157` |
| … | 1 + len | u8 len, bytes | `info` — `""` unless `done` (§6.3); cap 255 | `TE:738` | `BP:165-172` |

The trainer reads exactly this order: `FullKnight/python/binary_protocol.py:215-272` (`unpack_step`), defensive only for
*absent* trailers (older DLLs), never for reordered ones.

### 1.5 `reset` reply body (type `1`)

| offset | size | field | cite |
|---|---|---|---|
| 0 | 1 | u8 type id = 1 | `BP:10,37` |
| 1..B+132 | as §1.4 | `n_combat, n_terrain, combat rows, terrain rows, global_state` | `BP:48-60` |
| B+132 | var | `combat_kinds`, `combat_parents`, `terrain_debug` (same formats as §1.4; **no step scalars**) | `BP:62` gate, `BP:75-106` |
| … | 43 | reset trailer: `u8 reset_branch`, then 7 × (`f32 ms`, `u16 frames`) in `ResetPhase.Keys` order `[pre_unload, transition_out, settle, load_boss_scene, recreate_reader, init_boss_refs, obs_final]`; branch `0=workshop 1=natural_end 2=unknown` | `BP:128-138`, `PR:95-104`, `FullKnight/python/binary_protocol.py:142-149` |
| … | var | `fsm_snapshots` block (same as §1.4) | `BP:147-157` |

No diag block, no `info` (`BP:113,165` are step-only). Reader: `FullKnight/python/binary_protocol.py:274-303`.

Byte arithmetic check (reset, `T:r2_move.a` record at frame 24762): `1+4 + 56·1 + 32·64 + 132 = 2241`;
kinds `1+13` (`Hornet Boss 1`), parents `1+18` (`Hornet Boss 1|Fall`) → 2274; terrain_debug
`63×(2+10) + 1×(2+11)` (`|seg_idx=0`…`|seg_idx=10`) → 3043; trailer 43 → 3086; fsm `2 + 3 entries
(33+31+33)` → **3185 = len**. Step 1: `2241 + 22 + 14 + 19 + 769 + 14 (diag) + 99 + 1 (info "")` =
**3179 = len**.

### 1.6 Where and when the payload is built (capture point)
- Step: `TE:596` `timeScale=1` → `ApplyAction` `TE:598` → `frames_per_wait × (yield return null)` `TE:614-624`
  → `timeScale=0` `TE:627` → reward fields `TE:699-709` → obs `TE:749-759` → `RaiseObs` `TE:762` → send `TE:763`.
  The obs is sampled in the coroutine resume of the step's last live frame, **after that frame's
  `Update`s and before its `LateUpdate`s** (`analysis/specs/frame-order.md:224`, `:402-417`; `analysis/specs/hero-motion.md` §2.2).
  `OBS.frame` equals that frame (`TR:414`; §0: 540/540).
- Reset (full path): obs built at `TE:480-490` **before** `Time.timeScale = 0` (`TE:492`), in the resume
  after the boss-wake loop's last `yield return null` (`TE:440-446`). Fake path: `TE:336-345`, with
  `timeScale` still 0 from the previous step's `TE:627`. The frozen frames between a reply and the
  next request are wall-clock dependent in ws mode (Q-obs-11).
- The recorder writes the identical bytes (`TR:409` calls `Pack` on the same `Message` the env then sends,
  `TE:355-356, 516-517, 586-587, 743-744, 762-763`).

---

## 2. Global state — 33 floats (`SE:11`, `SE:86-96`; trainer indices `FullKnight/python/observation.py:18-57`)

`GetGlobalState(knightW, knightH, shim)` is called once per payload with the Knight-bucket bounds (§3.1)
and the input shim (`TE:337-338, 481-482, 750-751`). Reads are live at the capture point of §1.6.

| idx | name | expression | source field / method | units, notes |
|---|---|---|---|---|
| 0 | `vel_x` | `rb.velocity.x`, `rb = HeroController.rb2d` (reflection), 0 if null | `SE:33-35`; `D:HeroController.cs:435` | world units/s; equals `FRAME.hero.rb_vel_x` (§0) |
| 1 | `vel_y` | `rb.velocity.y` | `SE:36` | |
| 2 | `hp` | `PlayerData.instance.health` | `SE:37`; `D:PlayerData.cs:43` | masks (int); `PD:health = 9` at SceneReady |
| 3 | `soul` | `PlayerData.instance.MPCharge` | `SE:38`; `D:PlayerData.cs:73` | 0..99 int; `PD:MPCharge = 0` |
| 4 | `knight_w` | `bounds.size.x` of the last non-trigger `Collider2D` on the hero GameObject | `SE:28`, `HO:746-750`, `HO:115-117` | `0.5` (`SCENE#1215` size 0.5, `edgeRadius 0.0025` **not** included — observed 0.5 exactly) |
| 5 | `knight_h` | `bounds.size.y` | same | `1.28125` (`SCENE#1215`) |
| 6 | `has_dash` | `pd.hasDash ? 1 : 0` | `SE:41`; `D:PlayerData.cs:184` | `PD:hasDash = true` |
| 7 | `has_wall_jump` | `pd.canWallJump` | `SE:42`; `:144` | true |
| 8 | `has_double_jump` | `pd.hasDoubleJump` | `SE:43`; `:194` | true |
| 9 | `has_super_dash` | `pd.hasSuperDash` | `SE:44`; `:188` | true |
| 10 | `has_dream_nail` | `pd.hasDreamNail` | `SE:45`; `:168` | true |
| 11 | `has_acid_armour` | `pd.hasAcidArmour` | `SE:46`; `:192` | true |
| 12 | `has_nail_art` | `pd.GetBool("hasNailArt")` → `ModHooks.GetPlayerBool` (mod-hookable, no hook installed by this mod) | `SE:47`; `D:PlayerData.cs:4573-4576`, field `:158` | true |
| 13 | `can_jump` | `CanJump()` via `ReflectionHelper.CallMethod` (private) | `SE:50`, `SE:99-109`; `D:HeroController.cs:4717`; `D:Modding/ReflectionHelper.cs:594` | exception → 0 |
| 14 | `can_double_jump` | `CanDoubleJump()` | `SE:51`; `:4735` | |
| 15 | `can_wall_jump` | `CanWallJump()` | `SE:52`; `:4811` | |
| 16 | `can_dash` | `CanDash()` | `SE:53`; `:4762` | |
| 17 | `can_attack` | `CanAttack()` | `SE:54`; `:4771` | |
| 18 | `can_cast` | `CanCast()` (public) | `SE:55`; `:2973` | |
| 19 | `can_nail_charge` | `CanNailCharge()` | `SE:56`; `:4780` | |
| 20 | `can_dream_nail` | `hc.CanDreamNail()` direct | `SE:57`; `:3038` | |
| 21 | `can_super_dash` | `hc.CanSuperDash()` direct | `SE:58`; `:3029` | |
| 22 | `commit_locked` | `shim.CState == Locked` | `SE:69`; `PC:41-42` | §2.4 |
| 23 | `commit_releasing` | `shim.CState == Releasing` | `SE:70` | |
| 24 | `commit_progress` | `LockedStepsTotal > 0 ? 1 − max(LockedStepsLeft,0)/LockedStepsTotal : (releasing ? 1 : 0)`, `Clamp01` | `SE:71-81` | float division |
| 25–32 | `commit_action_0..7` | one-hot of `shim.LockedAction` if in `0..7`, else all 0 | `SE:82-83, 94-95` | index = `action[2]` vocabulary `PC:306-308` |

The exact conjunctions of the nine `Can*` predicates are transcribed in `analysis/specs/hero-motion.md` §4 (table at
`:812-822`); `CanJump` has the side effect `ledgeBufferSteps = 0` (`analysis/specs/hero-motion.md:814`), which the
observation call itself triggers once per payload — port the read as the call, not as a pure read.
Only `idx 0..5` are normalised by the trainer; `6..32` pass raw (`SE:22-26`, `FullKnight/python/config.py:81-82`).

### 2.4 Hard-commit block semantics (`PC:281-362`, `SE:65-84`)
`LockedStepsFor(a, fpw) = max(1, ceil(HoldGameSeconds[a] / (fpw × 0.00848f)))` with
`HoldGameSeconds = {1: 1.5, 3: 0.5, 5: 3.0, 6: 1.0}` (`PC:281-300`; the constant `0.00848f` at `PC:295` is
**not** the R2 dt — `open-questions.md` Q20 / `analysis/specs/hero-motion.md` Q-hero-6). Under R2 (`fpw = 2`,
`stepGameSeconds = 0.01696f`): nail_charge 89, focus 30, dream_nail 177, super_dash 59.

| step | `ApplyAction` branch | state after | obs `[22,23,24,25..32]` | `action_committed` |
|---|---|---|---|---|
| free pick of `a[2] ∈ {1,3,5,6}` | `PC:348-362` | `LockedAction=a`, `Left=T−1`, `Total=T`, `CState=Locked` (or `Releasing` if `T=1`) | `[1,0,1/T,onehot(a)]` | 0 |
| locked | `PC:338-347`: `a[2] := LockedAction`, `Left--`; `Left ≤ 0 → Releasing` | | `[1,0,1−Left/T,…]` or, on the step that reaches 0, `[0,1,1.0,onehot(a)]` | 1 |
| release | `PC:329-337`: `a[2] := 7`, all cleared **before** the obs | Idle | `[0,0,0,0…]` | 1 |

`Releasing` is visible in exactly one payload (the step whose `Left` reached 0). Verified:
`T:r2_rand1.a` step 1 (`action [0,2,1,1]`) → `gs[22]=1, gs[24]=0.011236=1/89, gs[26]=1, committed=0`;
step 2 `committed=1`; step 240 (super dash) `gs[24]=0.42373=1−34/59, gs[31]=1`.
Reset clears the machine (`TE:328, 366`, `PC:51-57`) and applies the neutral action `[2,2,7,1]`
(`TE:329-330, 367-368`).

---

## 3. Combat rows

### 3.1 Enumeration and buckets (`HO:9-25`, `HO:100-124`, `HO:738-742`)
A `HitboxReader` MonoBehaviour is created for every gameplay scene (`HO:298-305`,
`D:GameManager.cs:2104-2112`) and again explicitly at reset (`TE:430`). Its `Start()` (`HO:66-72`)
iterates `Resources.FindObjectsOfTypeAll<Collider2D>()` — **loaded scene objects, inactive/pooled
objects and prefab assets alike** [ENGINE] — and `AddHitbox` classifies each `Box/Polygon/Edge/Circle
Collider2D` (`HO:104`) into the **first** matching bucket:

| order | bucket | predicate | cite |
|---|---|---|---|
| 1 | `Enemy` | `col.GetComponent<DamageHero>() != null` ‖ `col.gameObject.LocateMyFSM("damages_hero") != null` | `HO:107-109`; `D:FSMUtility.cs:123-148` (`GetComponents` on that GameObject only, `FsmName` equality) |
| 2 | `Terrain` | `go.layer == PhysLayers.TERRAIN (8) && !col.isTrigger` | `HO:111-113`; `D:GlobalEnums/PhysLayers.cs:9` |
| 3 | `Knight` | `go == HeroController.instance.gameObject && !col.isTrigger` | `HO:115-117` |
| 4 | `Attack` | `go.GetComponent<DamageEnemies>()` ‖ `go.LocateMyFSM("damages_enemy")` ‖ (`go.name == "Damager" && go.LocateMyFSM("Damage")`) | `HO:119-121` |

Later-created objects are added when `ModHooks.ColliderCreateHook` fires, i.e. in
`PlayMakerUnity2DProxy.Start()` (`HO:280`, `D:Modding/ModHooks.cs:89,686-693`,
`D:PlayMakerUnity2DProxy.cs:124-135`), via `GetComponentsInChildren<Collider2D>(true)` of that object
(`HO:74-80`). Membership is permanent until the collider is destroyed (`HO:709-710` `RemoveWhere(c == null)`
each payload; `PruneDestroyed` `HO:82-98` has **no call site**).

Per payload (`HO:700-837`): buckets are visited in `SortedDictionary` key order = enum order
`Knight(0), Enemy(1), Attack(2), Terrain(3)` (`HO:9-15,19` [ENGINE: sorted by the enum's int]);
within a bucket in `HashSet` iteration order (§3.8). A collider is **skipped unless
`col.isActiveAndEnabled`** (`HO:742`: GameObject active in hierarchy **and** collider enabled). Knight-bucket
colliders only set `knightW/H` (`HO:746-751`; last one wins) and emit no row. Enemy and Attack colliders
emit one row each (`HO:759-800`).

In `GG_Hornet_1` the loaded-scene members are (ascending `SCENE#` = enumeration order, §3.8):
Enemy — `_GameManager/GlobalPool/Gas Explosion Recycle M(Clone)` ×3 (`#675-677`), `…Recycle L(Clone)` ×2
(`#788-789`) [all have `DamageHero`, layer Attack, never active in the traces], `Boss Holder/Hornet Boss 1/Sphere Ball`
(`#1222`, Circle r 2.53), `…/Hit GDash` (`#1225`, Polygon), `…/Hit ADash` (`#1226`, Polygon), `Needle`
(`#1227`, Polygon, scene root), `Boss Holder/Hornet Boss 1` body (`#1237`, Box, non-trigger). Attack —
every `damages_enemy` owner in `FSM#`: `Knight/Attacks/{Slash, AltSlash, UpSlash, DownSlash, WallSlash,
Great Slash, Dash Slash, Sharp Shadow, Cyclone Slash/Hits/Hit R, Hit L}`, `Knight/Spells/{Q Slam, Q Slam 2,
Q Mega, Scr Heads, Scr Heads 2}/Hit *`, `Knight/Spells/Q Fall Damage`, `Knight/SuperDash Damage`,
`Knight/Effects/SD Burst`, `Knight/Charm Effects/Thorn Hit/Hit *`, pooled `Fireball(Clone)`,
`Fireball2 Spiral(Clone)`, `Grubberfly Beam*`, `Spell Fluke Dung Lv*/Damager` (`SCENE#1177-1218`).
The diag counts `17 / 68 / 14` (`T:r2_move.a` step 1) exceed these because assets are counted (Q-obs-2).
`Knight/HeroBox` (`#1220`, trigger, child GO), `*/Clash Tink` (layer Tinker) and `Needle Tink` (layer
Attack, no `DamageHero`, `analysis/specs/boss-hornet.md:151`) fall in no bucket.

### 3.2 Knight-relative frame
`knightPos = HeroController.instance.transform.position` (`HO:711`) — the **transform**, not `rb2d.position`
(they coincide under R2 with interpolation off; `STATE.md`). `bounds = col.bounds` (`HO:744`) is the
world-space AABB of the collider [ENGINE; evidenced: Hornet body `rel = pos + offset`, §0; `Hit GDash`
`w = 2.1490 − 0.3343 = 1.8147` from its polygon points `analysis/specs/boss-hornet.md:56`; `Sphere Ball` `w = h = 2·2.53·scale`
= 4.49193 / 7.59 as the `Grow` FSM scales it; `Hit ADash` rotated → different AABB (1.64778 × 0.88207)].
`rel_x = bounds.center.x − knightPos.x`, `rel_y` likewise, `w = bounds.size.x`, `h = bounds.size.y` (`HO:753-756`).

### 3.3 The 14 features (`HO:793-797`; trainer indices `FullKnight/python/observation.py:60-81`)

| idx | name | expression | cite | notes |
|---|---|---|---|---|
| 0 | `rel_x` | `bounds.center.x − knightPos.x` | `HO:753` | |
| 1 | `rel_y` | `bounds.center.y − knightPos.y` | `HO:754` | |
| 2 | `w` | `bounds.size.x` | `HO:755` | |
| 3 | `h` | `bounds.size.y` | `HO:756` | |
| 4 | `vel_x` | `rel_x − prevRel.x` iff `prevRelCache[col].tick == MotionTick − 1`, else `0` | `HO:780-791`, cache `HO:54-55` | `MotionTick++` once per `GetSplitFeatures` (`HO:707`); cleared on fake reset (`TE:335`, `HO:60-64`); new reader on full reset. Not called on done steps (`TE:735-746`). Verified 578/578 (§0) |
| 5 | `vel_y` | likewise | | |
| 6 | `is_trigger` | `col.isTrigger` | `HO:757` | |
| 7 | `gives_damage` | `bucket == Enemy` | `HO:763` | capability flag, not `damageDealt` (`analysis/specs/damage-path.md` §5) |
| 8 | `takes_damage` | `hm != null`, `hm = GetParentHm(col)` = nearest `HealthManager` on self or ≤ 7 ancestors (`depth < 8`) | `HO:761,764`, `HO:197-204`, `HO:221-239` | cached per collider forever (`HO:184-193`); Q-dmg-6 |
| 9 | `is_target` | `hm != null && bossHms.Contains(hm)` | `HO:765` | `bossHms = TrainingEnv._bossHMs` (§6.4) |
| 10 | `is_invincible` | `hm != null && hm.IsInvincible` | `HO:776`; `D:HealthManager.cs:226-236` (field `:159`) | Q-dmg-7 |
| 11 | `hp_raw` | `(float)hm.hp` or 0 | `HO:766`; `D:HealthManager.cs:108` | can be −50 (`D:HealthManager.cs:504`) |
| 12 | `hp_max_raw` | `ObserveMaxHp(hm)` = `max(first-seen hp, current hp)`, cached per HM | `HO:767`, `HO:208-219` | Q-dmg-10 |
| 13 | `anim_phase` | `len = clip.frames.Length; len > 0 ? Clamp01(anim.CurrentFrame / len) : 0`; `0` if no animator/clip | `HO:769`, `HO:152-175`; `D:tk2dSpriteAnimator.cs:115,146-160`; `D:tk2dSpriteAnimationClip.cs:20` | finished `Once` clip → `1.0` (`analysis/specs/tk2d-animator.md` §5) |

Knight-owned rows (`Slash`, `Needle` after re-parenting to root) have `takes_damage = is_target = 0`,
`hp = hp_max = 0` (no HM within 8 ancestors) — `T:r2_rand1.a` steps 2, 90, 93, 105. `Sphere Ball`/`Hit *`
rows carry Hornet's HM (`takes=1, target=1, hp=900`) — `T:r2_move.a` steps 154, 227.

### 3.4 `combat_kinds[i]` — visual-entity string (`HO:134-142`, `HO:241-268`)
`Strip(name)` of the nearest transform (self or ≤ 7 ancestors) carrying a `tk2dSpriteAnimator`; fallback
`Strip(col.gameObject.name)`, or `"unknown"` if empty. `Strip` cuts at the first `"(Clone)"` and `Trim()`s
(`HO:262-268`). Cached per collider (`HO:137-141`). The animator found is cached for §3.5 (`HO:250,257`).
Seen: `Hornet Boss 1` (body, `Hit GDash`, `Hit ADash` — all resolve to the boss root), `Sphere Ball`,
`Needle`, `Slash`, `AltSlash`, `UpSlash`, `DownSlash`, `Cyclone Slash` (for `Hits/Hit L|R`).

### 3.5 `combat_parents[i]` — **the clip key**, not the HealthManager name (`HO:799`, `HO:152-175`)
`Strip(anim.gameObject.name) + "|" + clip.name` for the cached animator's `CurrentClip`; `"none"` if no
animator or no clip; memoised per clip asset (`HO:169-174`). `GetParentKind` (`HO:184-193`, the HM root
name) is computed as a side effect of `GetParentHm` but **never sent**. Seen: `Hornet Boss 1|Fall`,
`Hornet Boss 1|Throw Antic`, `Sphere Ball|Sphere Ball`, `Needle|Needle`, `Slash|SlashEffect M`,
`Cyclone Slash|Cyclone Effect`, …

### 3.6 Vocab ids
Ids exist only in the trainer: `KindVocab` (`FullKnight/python/vocab.py:18-58`) assigns the next integer on first sight,
in arrival order across all envs, `0 = "unknown"` (also for `""`/`None`, `:34-35`), `1 = "terrain"`
(reserved, never on the wire), cap `kind_vocab_size = 512` → overflow to 0 (`FullKnight/python/config.py:209`);
persisted in the checkpoint (`:63-69`). `combat_kinds` and `combat_parents` share one vocab
(`FullKnight/python/vec_env.py:345-348`). **The sim's obligation is the exact strings**; ids are stable within a run and
follow the checkpoint across runs.

### 3.7 `fsm_snapshots` (`TE:775-813`, `FO:67-131`)
Entries `"<src>|<owner>|<fsm>|<state>"` (`FO:126-131`; `state = ActiveStateName ?? "(none)"`,
`PM:PlayMakerFSM.cs:90-99` returns `""` when no active state). Order: (B) for each boss HM in `_bossHMs`
(HashSet order), every `PlayMakerFSM` in `GetComponentsInChildren<PlayMakerFSM>(true)` of the boss root
(`FO:82`; same enumeration the trace ENTITY block uses, `TR:623-624`) that `isActiveAndEnabled` (`FO:90`)
and whose `FsmName` is not in `NameBlacklist` (`FO:39-64,91`), `owner` = boss root name (`FO:81`); then
(E) every **active** Enemy-bucket collider in HashSet order, `GetComponents<PlayMakerFSM>()` of the
collider's own GameObject, same filters, `owner` = that GameObject's name (`FO:103-124`); then (A) the same
over the Attack bucket. Hornet examples: `B|Hornet Boss 1|Control|GG Fall`, `B|Hornet Boss 1|Fluctuate|Off`
(the FSM lives on `Boss Holder/Hornet Boss 1/Evade Range`, `FSM#`), `B|Hornet Boss 1|Grow|Grow` (only while
`Sphere Ball` is active), `E|Sphere Ball|Grow|Grow`, `E|Needle|Control|Out`, `E|Hornet Boss 1|Control|…`,
`A|Slash|nail_cancel_check|Idle`, `A|Hit R|nailart_damage|Set`, `A|Hit L|nailart_damage|Set`.
Consumed only by the visualiser/fsm_tracker (`PR:28-35`, `FullKnight/python/env.py:39-42`), never by training.

### 3.8 Row order within a bucket [ENGINE] — see Q-obs-1
Rows come from `HashSet<Collider2D>` enumeration (`HO:740`). Observed:
- Terrain: the 13 emitted colliders appear in exactly the `SCENE#` order (`SceneDumper.cs:38` enumerates
  with the same `Resources.FindObjectsOfTypeAll<Collider2D>()`, `:40` drops unloaded; `SCENE#` is
  strictly ascending in `instanceID`), identical across 300/300 payloads (§4.6).
- Enemy: `Sphere Ball` (#1222) < `Hit GDash` (#1225) < `Hit ADash` (#1226) < `Needle` (#1227) < body (#1237)
  in every payload where two are active (`T:r2_move.a` steps 154, 227, 242, 250; `T:r2_rand1.a` 90, 93, 105,
  162). Attack: `Hit R` (#1178) before `Hit L` (#1187) (`T:r2_rand1.a` step 93).
- Enemy rows always precede Attack rows (bucket order; e.g. `('Needle','Hornet Boss 1','Cyclone Slash','Cyclone Slash')`).
So far every observation is consistent with "ascending instanceID = FindObjectsOfTypeAll order = insertion
order". Nothing in the corpus exercises a removal (`RemoveWhere`) followed by re-insertion, which is where
a .NET `HashSet` reuses freed slots; the trainer itself is order-invariant (set encoders, `FullKnight/python/model.py`),
so only the byte-identity gate depends on this.

---

## 4. Terrain rows

### 4.1 Membership
Bucket `Terrain` (§3.1: layer 8 and non-trigger — hence `is_trigger` **is always 0** on terrain rows),
`isActiveAndEnabled` (`HO:742`), and **not** `col.usedByComposite` (`HO:808-810`; exception → treated as
false). Composite-absorbed shapes are dropped entirely; a `CompositeCollider2D` itself is not in the
shape filter `HO:104` and is never emitted — so a scene whose terrain is composite-only would emit nothing
(no such scene in the four dumps: `usedByComposite = false` for all 1251 `GG_Hornet_1` colliders, `analysis/dumps/GG_Hornet_1/scene.json` `count`).

### 4.2 Segment decomposition (`HO:416-481`)
Every terrain collider becomes an ordered list of world-space segment pairs `(a, b)`, all points via
`transform.TransformPoint(local + offset)` (rotation and scale respected):

| shape | segments | cite |
|---|---|---|
| `EdgeCollider2D` | `(p[i]+off, p[i+1]+off)` for `i = 0..n−2`, in point order | `HO:424-433` |
| `PolygonCollider2D` | per path `pi`, closed: `(p[i], p[(i+1) mod n])` for `i = 0..n−1`, paths in index order; empty paths skipped | `HO:434-449` |
| `BoxCollider2D` | `half = size/2`, corners `bl, br, tl, tr` of `offset ± half`; order **bottom (bl→br), right (br→tr), top (tr→tl), left (tl→bl)** | `HO:450-462` |
| `CircleCollider2D` | 12-gon: `p_i = offset + r·(cos θ_i, sin θ_i)`, `θ_i = 2π i/12`, `i = 0..12`, segments `(p_{i−1}, p_i)` | `HO:463-480` (`N = 12`, `Mathf.PI`) |
| anything else | nothing | `HO:481` |

Then per segment (`HO:485-518`), in float32:
```
ax = a.x − kx ; ay = a.y − ky ; bx = b.x − kx ; by = b.y − ky          // HO:487-488
mx = 0.5(ax+bx) ; my = 0.5(ay+by) ; hdx = 0.5(bx−ax) ; hdy = 0.5(by−ay) // HO:489-492
if (hdx < 0 || (hdx == 0 && hdy < 0)) { hdx = −hdx; hdy = −hdy }        // HO:496-500  (canonical hdx ≥ 0)
dxs = bx−ax ; dys = by−ay ; denom = dxs²+dys²                            // HO:503-504
t = denom > 1e-12f ? clamp((−ax·dxs + −ay·dys)/denom, 0, 1) : 0          // HO:505-511
npx = ax + t·dxs ; npy = ay + t·dys ; dist = sqrt(npx²+npy²)             // HO:512-514 (Mathf.Sqrt)
row = [mx, my, hdx, hdy, npx, npy, dist, isTrigger]                      // HO:516
```
Note `npx/npy` use the **un-canonicalised** `a→b` direction (`dxs/dys` from the original endpoints);
only `hd` is flipped. `Mathf.Sqrt` is `(float)Math.Sqrt(double)` — float32 result. `−0` appears on the wire
(e.g. `hdy = −0.` for the Bounds Cage top edge, §0) and must be preserved bit-for-bit.

### 4.3 Row order
Colliders in Terrain-bucket `HashSet` order (§3.8), segments in the shape order of §4.2, `seg_idx`
restarting at 0 per collider (`HO:484,517-518`).

### 4.4 Trainer post-processing (informational)
The trainer drops rows with `|npx| > view_w/2 = 15` or `|npy| > view_h/2 = 8.5` (`FullKnight/python/observation.py:237-257`,
`FullKnight/python/vec_env.py:319-323`, `FullKnight/python/config.py:79-80`) and pads/masks per batch (`FullKnight/python/vec_env.py:331-361`). The
sim must still emit **all** rows (e.g. the Bounds Cage at `npy ≈ −238`).

### 4.5 `terrain_debug[i]`
`baseDebug + "|seg_idx=" + i` (`HO:517`). With `eval = false` (training), `baseDebug = ""` (`HO:819-821`)
→ exactly `"|seg_idx=<i>"`, reproducible. With `eval = true` it is `BuildTerrainDebug` (`HO:522-664`):
GameObject path, layer name, `bounds` formatted `"0.00"`, plus live `Physics2D.GetIgnoreLayerCollision /
GetIgnoreCollision / Distance / IsTouching / Linecast / OverlapPointAll` results — engine queries the sim
cannot reproduce byte-for-byte; parity runs must use `eval = false` (§5).

### 4.6 GG_Hornet_1 terrain inventory (verified against `T:r2_move.a`, 64 rows every step)

| rows | collider (`SCENE#`) | shape | segs |
|---|---|---|---|
| 0–3 | `_GameManager/Bounds Cage` (#1171, the only **active** one of four) | Box 1520.17 × 78.17 at offset (150.09, −368.3) | 4 |
| 4–14 | `Roof Collider` (#1223) | Polygon, 1 path, 11 points | 11 |
| 15–26 | `Hornet Saver/Colliders` ×3 (#1229 27.6×20 @ (26.4,17); #1230 8×20 @ (42,35); #1231 8×20 @ (11,35)) | Box | 12 |
| 27–33 | `TileMap Render Data/Scenemap/Chunk 1 0` (#1245, 8 pts) | Edge | 7 |
| 34–40 | `Chunk 0 0` (#1246, 8 pts) | Edge | 7 |
| 41–45 | `Chunk 0 2` (#1247, 6 pts) | Edge | 5 |
| 46–51 | `Chunk 1 1` (#1248, 7 pts) | Edge | 6 |
| 52–56 | `Chunk 1 2` (#1249, 6 pts) | Edge | 5 |
| 57–63 | `Chunk 0 1` (#1250, 8 pts) | Edge | 7 |

All 53 box/edge segments recomputed from `scene.json` geometry (offset, size, points, transform) match the
wire rows to ≤ 1e-4 in `(mx, my, hdx, hdy, npx, npy, dist)` at the knight position `(22.540000915527344,
28.40812110900879)`; the 11 polygon rows are the `Roof Collider` path. Excluded as expected: three inactive
`Bounds Cage` boxes (#1172, #1173, #1175), the trigger `Orbit Shield(Clone)/Laser Stopper` (#189).

---

## 5. Fields the sim cannot (or should not) reproduce — mask list for the parity gate

| field | why | recommended treatment |
|---|---|---|
| `step_real_time` (`BP:67`) | Σ `Time.unscaledDeltaTime` = wall clock (`TE:620`) | mask (zero both sides before hashing) |
| diag `gc_heap_mb` (`BP:119`) | `GC.GetTotalMemory` (`TE:721`) | mask |
| diag `enemy/attack/terrain_count` (`BP:115-117`) | raw bucket sizes incl. prefab assets and pooled clones found by `FindObjectsOfTypeAll` (17/68/14 in Hornet vs 10/…/13 loaded; Q-obs-2) | mask, or pin per-scene constants |
| diag `kind_cache_size` (`BP:118`) | count of colliders ever classified (`HO:137-141`); grows 1→7 over an episode, deterministic given identical rows | mask (or model the lazy cache) |
| reset trailer `ms`, `frames` (`BP:133-137`) | wall time and scene-load frame counts (`TE:845-862`; e.g. `load_boss_scene 517 ms / 650 frames`) | mask; keep `reset_branch` (deterministic §6.4) |
| `terrain_debug` when `eval=true` | physics queries + `"0.00"` formatting (§4.5) | run parity with `eval=false` (then the strings are `"|seg_idx=i"` and are compared) |
| `fsm_snapshots` | reproducible only if every listed FSM is modelled (worker F scope) | compare as its own field; not needed for the trainer |
| `step_game_time` | Σ `Time.deltaTime` in float32 over `fpw` frames; under R2 exactly `0.02f + 0.02f = 0x3D23D70A` (`0.03999999910593033`) | reproduce |
| `hits_taken` fake-reset edge cases, RNG draws | §6.2, Q-obs-3 | reproduce under a pinned `FK_FAKE_RESET_PROB` |

Everything else — counts, all float rows, the 33 globals, kinds, parents, `damage_landed`, `hits_taken`,
`hp_healed`, `done`, `action_committed`, `info`, `reset_branch` — is deterministic game state and is the
P6 gate. Proposed harness canonicalisation: zero the byte ranges of `step_real_time`, the 14-byte diag block
and the 42 trailer bytes after `reset_branch` on both payloads, then compare bytes.

---

## 6. Reward signals, termination and reset — as coded

### 6.1 Per-step signals (`TE:699-709`), accumulated during the step's frames and zeroed after packing
- `damage_landed += hitInstance.DamageDealt / (float)(n × maxHP) × 100f` in the `On.HealthManager.TakeDamage`
  wrapper, **before** `orig` and before any fake-reset clamp, only for HMs in `_bossMaxHPs`
  (`TE:1172-1187`); `n = _bossHMs.Count`, `maxHP = hm.hp` at bind time (`TE:1447`). Units: percent of one
  boss's HP pool, equal-weighted across bosses. Uses the raw `DamageDealt`, not the applied
  `RoundToInt(DamageDealt × Multiplier)` (`D:HealthManager.cs:499-504`). Untracked HMs pass through
  (`TE:1176-1180`). On the wire as `f32`.
- `hits_taken += damage` in the `ModHooks.AfterTakeDamageHook` handler (`TE:1161-1170`, subscribed `TE:1144`),
  which HK invokes from `HeroController.TakeDamage` only after the invulnerability/i-frame gates, at
  `D:HeroController.cs:1958` (normal), `:2029` (spikes), `:2042` (acid) (`D:Modding/ModHooks.cs:1276-1296`;
  gating transcribed in `analysis/specs/damage-path.md` §1). `damage` is the amount HK will apply, i.e. after the
  `On.HeroController.TakeDamage` wrapper (`TE:1285-1308`) may have clamped it (§6.2). `_syntheticKill`
  is never true (`KillKnight` `TE:1350` has no call site). Units: masks; wire `f32`.
- `hp_healed = max(0, health_now − health_at_step_start)` **only if `!_episodeDone`** (`TE:604, 699-701`).
- Trainer reward, for context: `attack_weight·δ_atk/D − δ_def + heal_coef·hp_healed` (`FullKnight/python/ppo.py:442`).

### 6.2 Fake reset (`TE:56-73`, `FK_FAKE_RESET_PROB`)
`_fakeResetProb` is read from the env var at `Setup` (`TE:1099-1107`; `FullKnight/python/train.py:652-656` sets it from
`config.fake_reset_prob = 1.0`, `FullKnight/python/train.py:652-656`, `FullKnight/python/config.py:428`; **unset in the oracle captures**
→ 0). With `p > 0`:
- Boss side (`TE:1199-1225`): `wouldDie = hp − DamageDealt ≤ 0 && !hasSpecialDeath`; if every other tracked
  HM has `hp ≤ 0` and `UnityEngine.Random.value < p` (**one RNG draw**, only evaluated when `wouldFinish`),
  `DamageDealt := max(0, hp − 1)`, `orig`, `RestoreFightHPs()`, `_fakeResetPending = true`, source = boss.
- Knight side (`TE:1292-1305`): if `damageAmount > 0 && health − damageAmount ≤ 0 && Random.value < p`
  (one draw, only when lethal): `orig(…, max(0, health − 1), …)`, `RestoreFightHPs()`, pending, source = knight.
  With `health = 1` the clamped amount is 0 and `HeroController.TakeDamage` returns at `D:HeroController.cs:1829-1832`
  — no i-frames, no `hits_taken`.
- `RestoreFightHPs` (`TE:1323-1340`): `hc.MaxHealth()` (`D:HeroController.cs:2183-2187` → `PlayerData.MaxHealth`
  `D:PlayerData.cs:4630-4636`, health := `CurrentMaxHealth`) and `hm.hp := maxHp` for every tracked HM
  that is not `isDead`.
- Silent knight death (`health ≤ 0` without the hook, `TE:657-678`): another `Random.value < p` draw.

### 6.3 Episode end (`TE:632-687`, evaluated after the frame loop; loop breaks early on `_bossDied || health ≤ 0`, `TE:622-623`)
Priority: `_fakeResetPending` → `done, info = "fake_reset_boss" | "fake_reset_knight"` (`TE:634-651`);
else `_bossDied` → `"win"` (`TE:652-656`); else `health ≤ 0` → fake roll (`TE:662-672`) or `"loss"`
(`TE:673-677`). `_bossDied` is set only by `OnBossActualDeath` (`TE:1263-1278`) subscribed to
`HealthManager.OnDeath` of every tracked HM (`TE:1452-1454`; fired from `SendDeathEvent`
`D:HealthManager.cs:685-691`, reached only when `!hasSpecialDeath` `:567-571`) and only when all other
tracked HMs have `hp ≤ 0`. The done payload carries `done=1`, `info`, **empty** combat/terrain, 33 zero
floats, empty fsm block, the real reward fields and diag (`TE:735-746`, `TE:704-721`). A further `action`
after done returns the same shape with zero rewards/times and a zero diag block (`TE:573-589`) — the trainer
never sends one (it resets, `FullKnight/python/vec_env.py:101,206`).

### 6.4 Reset paths (`TE:175-519`)
Common: `_level = data.level ?? _level`, `_frameSkipCount = data.frames_per_wait ?? old`, `_evalMode`,
all counters zeroed, `_resetCount++` (`TE:289-302`).
- **Fake fast path** (`_lastEpisodeWasFake && level unchanged && !force_full`, and no live native transition,
  and no tracked HM `isDead`; `TE:236-287, 309-358`): no scene load; `reset_branch = 2`; all 7 phases logged
  (values ≈ 0 ms / 0 frames); `ResetCommit` + neutral action; `ClampBossLevel`; `ClearMotion`; obs; the world
  continues **mid-fight** (boss FSM state, positions, RNG untouched). `timeScale` stays 0.
- **Full path**: `ResetCommit` + neutral action (`TE:366-368`), `timeScale = 1`, `captureDeltaTime = kStepDeltaTime`
  (`TE:373-378`), branch `0` if already in `GG_Workshop`, else `1` (waiting for the natural death/dream-return
  transition unless the previous end was fake, `TE:388-415`), `SceneHooks.LoadBossScene` (`TE:424`), new reader +
  1 frame (`TE:430-431`), `InitBossRefs` = `BossSceneController.bosses ∪ BossHealthLookup.Keys` (`TE:1404-1458`;
  `D:BossSceneController.cs:42,102`) with `_bossMaxHPs[hm] = hm.hp` and `OnDeath` subscription, wake loop up
  to 600 frames until an Enemy-bucket collider is active (`TE:438-446`), `ClampBossLevel` (`BossLevel := 0`,
  `TE:1243-1256`), `_knightMaxHP`, damage hooks, obs (`TE:480-490`), `timeScale = 0`, accumulators zeroed
  (`TE:492-503`). Late binding retries every 24 steps and a ≥100-HP scan every 240 steps if no boss bound
  (`TE:531-571`) — affects `is_target`/`damage_landed` for late-registering bosses (not Hornet).

---

## 7. P6 port checklist (what `hksim_obs` and the world model must provide)

Packer (`sim/core`, orchestrator):
1. Type byte; `u16 nc, nt`; rows as float32 little-endian in the exact column orders of §3.3 / §4.2; 33
   globals; step scalars `f f f f f B B`; kinds then parents (`u8` len, UTF-8, cap 255); terrain_debug
   (`u16` len); diag `H H H i f` (step); reset trailer `B + 7×(f H)`; fsm block `H + n×(H bytes)`; info
   `B bytes` (step). One-byte replies for `init/pause/resume`; none for `close`. Request parser of §1.2
   (`time_scale` ignored, `eval` → debug strings, `force_full` → full reset).
2. `hits_taken` written as `(float)int`; `−0.0f` preserved; float32 arithmetic for every derived value
   (`0.5f·(…)`, the `t` projection, `Mathf.Sqrt`, `Clamp01((float)frame/len)`, `DamageDealt/(float)(n·max)·100f`,
   `1f − left/total`); no FMA (`docs/float-parity.md`).
3. Done payload = empty sets + 33 zeros + real reward fields (§6.3); a `step` after done = zero rewards, zero diag.

World state the packer reads (workers H/F/P):
4. Hero: `transform.position` (§3.2), `rb2d.velocity`, `PlayerData.health / MPCharge / maxHealth`, the seven
   `has_*` bools (constants from `PD:`), the nine `Can*` predicates evaluated at the capture point (with
   `CanJump`'s side effect), the hero body collider bounds (0.5 × 1.28125; `edgeRadius` excluded).
5. Input shim: `CState / LockedAction / LockedStepsLeft / LockedStepsTotal` per `PC:281-362` with
   `0.00848f` (Q20) and the neutral-action reset.
6. Every collider of §3.1 with: GameObject active-in-hierarchy + collider enabled (toggled by FSMs, e.g.
   `Hit GDash`, `Sphere Ball`, the knight's slashes), `isTrigger`, world AABB of the shape under the
   current transform (box size changes from `SetBoxCollider2DSizeVector`, `Sphere Ball` scale, rotated
   polygons), layer (terrain), `usedByComposite`, nearest `HealthManager` (≤ 8 levels) and nearest
   `tk2dSpriteAnimator` (≤ 8 levels), GameObject names for `Strip`.
7. HealthManager: `hp`, `invincible`, `isDead`, `hasSpecialDeath`, `OnDeath`; `hp_max` first-sight cache per HM
   (§3.3 idx 12) living with the reader (cleared on full reset, kept on fake reset).
8. tk2d animator per §3.3 idx 13: `CurrentClip` (name, `frames.Length`, wrapMode), `CurrentFrame` rule
   `D:tk2dSpriteAnimator.cs:146-160` (`Once`: `min((int)clipTime, n)`), sampled post-Update/pre-LateUpdate.
9. Per-collider previous `rel` + tick for `vel_x/vel_y`; tick advances once per non-done payload; cleared on
   fake reset; fresh on full reset.
10. Insertion-ordered per-bucket collider lists in the order of §3.8 (from `scene.json` instanceID order),
    with append on runtime creation (`PlayMakerUnity2DProxy.Start`) and removal on destroy; bucket order
    Knight, Enemy, Attack, Terrain.
11. Boss set `_bossHMs` = `BossSceneController.bosses ∪ BossHealthLookup` at scene-ready (Hornet: the single
    `Boss Holder/Hornet Boss 1` HM, `analysis/dumps/GG_Hornet_1/bosses.json`), `_bossMaxHPs` (900),
    `BossLevel := 0`.
12. Damage hooks with the exact ordering of §6.1/§6.2 (credit before clamp; `AfterTakeDamage` after the
    i-frame gates; RNG draw only on lethal hits) and `FK_FAKE_RESET_PROB` as a config input.
13. FSM snapshot strings per §3.7 (blacklist `FO:39-64`, enumeration order, `"(none)"` fallback).
14. Reset semantics of §6.4 incl. the fake fast path (world continues) and `reset_branch`.

GG_Hornet_1 objects that must exist for row parity: `Knight` body collider; `Hornet Boss 1` body Box +
`Hit GDash` + `Hit ADash` polygons + `Sphere Ball` circle (+ `Grow` FSM scale) + `Needle` polygon (root,
no HM) ; knight `Attacks/*` polygons and `Cyclone Slash/Hits/Hit L|R`; the 13 terrain colliders of §4.6;
`Evade Range/Fluctuate`, `Sphere Ball/Grow`, `Needle/Control`, `Attacks/*/nail_cancel_check`,
`Hits/*/nailart_damage` FSMs for the snapshot block. Pooled `Gas Explosion`, spells and charm hitboxes only
matter if they ever activate.

---

## 8. Open questions

### Q-obs-1 — Within-bucket row order is `HashSet<Collider2D>` enumeration order
The wire order of combat and terrain rows is `HashSet` iteration (`HO:740`) over sets filled by
`Resources.FindObjectsOfTypeAll<Collider2D>()` at reader `Start` (`HO:68-70`) plus later `ColliderCreateHook`
additions (`HO:74-80`), with `RemoveWhere` of destroyed colliders every payload (`HO:709-710`). Both the
enumeration order of `FindObjectsOfTypeAll` and the slot-reuse behaviour of Mono's `HashSet` after removals
are engine facts with no decomp [ENGINE]. Evidence so far: terrain order == `scene.json` order (ascending
`instanceID`) 300/300; every Enemy/Attack pair observed is in ascending `instanceID` order (§3.8); no removal +
re-insertion occurs in the corpus (all emitted colliders are scene-resident). **Evidence that would close
it:** a corpus that destroys and re-creates Enemy/Attack colliders (a spell whose effect is instantiated and
destroyed, or a multi-episode ws-mode trace) decoded for row order; plus an `instanceID`-ordered dump of the
reader's sets at SceneReady (a one-line addition to `SceneDumper`). Until then the sim should order by the
`scene.json` index and append runtime creations, and the harness should report an order-only mismatch as
its own category.

### Q-obs-2 — Prefab assets and pooled clones inflate the diag counts
`diag_enemy_count = 17`, `diag_attack_count = 68`, `diag_terrain_count = 14` (`T:r2_move.a`), but the loaded
scene holds 10 Enemy-class, 14 non-trigger layer-8 colliders and `scene.json` skipped 165 unloaded
colliders (`SceneDumper.cs:40`, `skippedNotLoaded`). `FindObjectsOfTypeAll` returns prefab assets, which the
reader classifies and counts (§3.1) but never emits (never `isActiveAndEnabled`). The sim cannot enumerate
assets it does not load. **Evidence that would close it:** either an owner decision to mask the diag block
(§5) or a dump of the reader's bucket contents including `!scene.isLoaded` entries so the counts become
per-scene constants.

### Q-obs-3 — Fake-reset path and its RNG draws are untraced
Training runs with `FK_FAKE_RESET_PROB = 1.0` (`FullKnight/python/config.py:428`, `FullKnight/python/train.py:652-656`), so almost every
episode ends by a clamped lethal hit, `RestoreFightHPs` mid-step, `info = fake_reset_*`, and the next reset
is the fast path where the world continues mid-fight (§6.2, §6.4). Every oracle capture ran with the variable
unset (`harness/tools/run_oracle.py:46` passes the parent environment) and no corpus contains a death
(`analysis/specs/damage-path.md` Q-dmg-9). The `UnityEngine.Random.value` draws at `TE:662,1215,1296` consume the shared
stream that boss FSMs read (`open-questions.md` Q15), so their exact placement matters for P4/P5 parity
as well. **Evidence that would close it:** two death corpora (knight-death, boss-death) captured with
`FK_FAKE_RESET_PROB=1` and `=0`, with the RNG chain and the `OBS`/`EPISODE_END` records inspected.

### Q-obs-4 — `Collider2D.bounds` for rotated boxes/polygons and scaled circles
`w, h, rel_x, rel_y` come from `Collider2D.bounds` (`HO:744-756`), a native world AABB [ENGINE]. Observed:
axis-aligned box = `size` at `position + offset` (Hornet body, knight, `edgeRadius` excluded); polygon =
AABB of transformed points (`Hit GDash` 1.8147 × 0.2350 from `analysis/specs/boss-hornet.md:56`; `Hit ADash` 1.64778 × 0.88207
when its owner is rotated for the diagonal dash); circle = `2·r·scale` (`Sphere Ball` 4.49193 → 7.59 under
`Grow`). Not observed: a rotated `BoxCollider2D`, a non-uniformly scaled circle (which Unity's circle cannot
represent), an `EdgeCollider2D`/`edgeRadius` in the combat buckets. **Evidence that would close it:** the
`bounds` field of `scene.json` (already dumped, `SCENE#*.bounds`) compared against AABBs recomputed from the
dumped `world` points for every rotated/scaled collider in the four scenes.

### Q-obs-5 — Which fields define "byte-identical" for the P6 gate
§5 lists fields that are wall-clock or process dependent (`step_real_time`, `gc_heap_mb`, reset phase
`ms/frames`, asset-inflated diag counts) and fields that are deterministic only if a subsystem outside the
trainer's needs is modelled (`fsm_snapshots`, eval-mode `terrain_debug`). PLAN.md §5 P6 says "byte-identical
step payloads" without a mask. **Evidence that would close it:** an owner decision adopting the §5
canonicalisation (zero the named byte ranges on both sides, run parity with `eval=false`) or requiring the
sim to emit constants for them.

### Q-obs-6 — Oracle fork vs FullKnight drift policy
Today the wire code is identical (§ preamble). FullKnight is scheduled to switch to R2 at P8 (`STATE.md`),
which touches `kStepDeltaTime` (`TE:127`), `frames_per_wait`, and possibly the commit constant `PC:295`
(Q20) — the latter changes `commit_progress` and `action_committed` on the wire. No mechanism re-verifies
the fork against the training tree. **Evidence that would close it:** a harness check that diffs the five
wire-defining files (namespace-normalised) and fails P6 when they diverge, run before every gate.

### Q-obs-7 — `hasNailArt` (and every `PlayerData.GetBool`) is mod-hookable
`gs[12]` reads `pd.GetBool("hasNailArt")` → `ModHooks.GetPlayerBool` (`D:PlayerData.cs:4573-4576`), a
Modding-API dispatch that any loaded mod may override; the other six `has_*` read fields directly. In the
oracle/training installs only this mod is loaded, and `PD:hasNailArt = true`. **Evidence that would close
it:** confirmation that `ModHooks.GetPlayerBool` returns the field unchanged when no `GetPlayerBoolHook`
subscriber exists (`D:Modding/ModHooks.cs`, the `GetPlayerBool` body) — then the sim ports it as the field.

### Q-obs-8 — `anim_phase` sampling phase vs the trace's ANIM sub-block
The combat row reads `CurrentFrame` in the coroutine resume, before that frame's `tk2dSpriteAnimator.LateUpdate`
(`analysis/specs/frame-order.md:413-417`); the trace `FRAME` ANIM block is sampled at the same point (`TR:446,506`), so
they agree, but the *sprite the player sees* is one LateUpdate ahead. `analysis/specs/tk2d-animator.md` §5 verified the
values for `Throw Antic`/`Fall`; this spec observed `Slash` 5/15, `Sphere Ball` 2/9, 4/9, `Needle` 1.0
(finished `Once`). The residual question is Q-frame-4 (the dt attributed to the last live frame's LateUpdate),
inherited, not new. **Evidence that would close it:** Q-frame-4's experiment.

### Q-obs-9 — `takes_damage`/`kind` ancestor walks stop at depth 8 and are cached forever
`ClassifyParent`/`ClassifyEntity` walk `depth < 8` (`HO:225,245`) and cache per collider until it is destroyed
(`HO:137-141,187-192`), so a collider re-parented at runtime (the `Needle` is de-parented to the scene root
by its own FSM, `analysis/specs/boss-hornet.md:150`) keeps the classification made at **first observation**, not its current
ancestry. In the corpus the needle is first observed already at root (`takes_damage = 0`); a needle first
seen while still under `Hornet Boss 1` would read `takes_damage = 1, is_target = 1, hp = 900` for the rest of
the reader's life. **Evidence that would close it:** the frame at which `Needle` becomes `isActiveAndEnabled`
relative to its `Setup`/de-parent action in `FSM#Needle::Control`, from an `EVENT FSM_TRANSITION` + `OBS`
alignment in `T:r2_rand1.a` (step 90 is the first needle row).

### Q-obs-10 — Name strings depend on Unity GameObject names incl. whitespace
`combat_kinds`/`combat_parents`/`fsm_snapshots` carry `Strip(gameObject.name)` and raw `FsmName`/state names
(`HO:262-268`, `FO:126-131`): `"(Clone)"` cut, outer whitespace trimmed, interior whitespace and case kept
(`"Hornet Boss 1"`, `"Sphere Ball"`, `"SlashEffect M"`, `"GG Fall"`). The sim's scene tables must carry the
exact names from `scene.json` / `FSM#` (paths there are `name/name/...`, so a name containing `/` would be
ambiguous). **Evidence that would close it:** a scan of the four dumps for names containing `/`, leading or
trailing whitespace, or non-ASCII (UTF-8 length ≠ char count matters for the `u8` length cap).

### Q-obs-11 — Number of frozen frames between a reply and the next request (ws mode)
Between `SendMessage` and the next `action`, frames keep rendering with `timeScale = 0` (`TE:627`); the
count is wall-clock dependent under the trainer but fixed in script mode (the reset `OBS` is at frame 24762
and step 1's live frames are 24764–24765 in `T:r2_move.a`, i.e. one frozen frame; the sim's R2 schedule
assumes exactly one per step, `analysis/specs/PORT-BRIEFS.md`). `HeroController.Update` runs on frozen frames with
`dt = 0` (`analysis/specs/hero-motion.md` §2.2) and PlayMaker is gated (`FsmPauseGate`), but `open-questions.md` Q18 notes
event dispatch is not gated. Whether N > 1 frozen frames are observationally a no-op is not established.
**Evidence that would close it:** a script-mode capture with an artificial K-frame delay before each action
(a `HK_ORACLE_*` knob) compared against the K = 1 trace for DH.

### Q-obs-12 — `hp_max_raw` after a fake reset and for multi-boss scenes
`ObserveMaxHp` lives on the reader (`HO:43,208-219`), so it survives fake resets (reader kept) and is
rebuilt on full resets. After a fake reset `RestoreFightHPs` sets `hp := _bossMaxHPs[hm]` (the bind-time
`hp`, `TE:1338`), which equals the first-seen value unless the boss refilled above it, so the column is stable
for Hornet; for phase bosses (`hasSpecialDeath`, False Knight: `project_fk_stagger_fake_reset` in the training
notes; Q-dmg-10) the first-seen value is a phase pool, not the fight total. **Evidence that would close it:**
per-boss `hp` at SceneReady vs `BossHealthLookup[hm].adjustedHP` over `analysis/dumps/*/bosses.json` (as
Q-dmg-10 asks) plus one fake-reset trace per boss.

---

## 9. Method note
`OBS` payloads were read with `harness/hktrace.py` (`Obs.payload`), parsed by a from-scratch transcription of
`BP:32-176` that asserts full consumption, then cross-checked against `FullKnight/python/binary_protocol.py` imported by path
(`unpack_reset`/`unpack_step` plus the `pop_last_*` side channels), and reconciled with the same-frame
`FRAME` record (hero pose/velocity/PlayerData/collider block; ENTITY hp/invincible/pose/anim) and with
`analysis/dumps/GG_Hornet_1/scene.json` (terrain geometry recomputed with the §4.2 rules). The FullKnight
decoder accepted every oracle payload; no mismatch was found.
