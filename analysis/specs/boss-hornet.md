# Hornet (GG_Hornet_1) — P1 discovery spec

Scope: everything the simulator needs to run *Hornet Boss 1* in scene `GG_Hornet_1`.
Authority order per PLAN.md §2: the FSM dump and the decompiled action classes **define** behaviour;
traces only **verify** it. Nothing here is from memory. Anything not present in a dump/decomp line is
written `UNKNOWN` with a `Q` entry in §7.

Citation shorthand used below:
- `FSM#<path>/<fsm>/<state>` → `analysis/fsm/GG_Hornet_1.json`, object at `<path>`, FSM `<fsm>`, state `<state>`.
- `BOSSES#<key>` → `analysis/dumps/GG_Hornet_1/bosses.json`, `healthManagers[0].<key>`.
- `SCENE#<path>` → `analysis/dumps/GG_Hornet_1/scene.json`, the `colliders[]` row whose `path` is
  `<path>`. Each row carries `type/layer/layerName/isTrigger/enabled/activeInHierarchy/offset` +
  `size|radius|points`, world-space `world[]` geometry, the attached `rigidbody`, and a
  `components[]` list with non-Unity component field values.
- `PHYS#<key>` → `analysis/dumps/GG_Hornet_1/physics.json`.
- `RNGP#<n>` → `analysis/dumps/GG_Hornet_1/rng_probe.json`, `draws[n]`.
- `ACT/X.cs:n` → `analysis/decomp/Assembly-CSharp/HutongGames.PlayMaker.Actions/X.cs:n`.
- `HK/X.cs:n` → `analysis/decomp/Assembly-CSharp/X.cs:n`.
- `PM/X.cs:n` → `analysis/decomp/PlayMaker/HutongGames.PlayMaker/X.cs:n` (and
  `analysis/decomp/PlayMaker/PlayMakerFSM.cs`, `.../PlayMakerFixedUpdate.cs`).
- `TR:<file>@<idx>` → `analysis/traces/p0/<file>`, FRAME-record index (not `Time.frameCount`).

### Reading the FSM dump correctly (applies to every value below)

1. A field object `{"__fsm":"FsmFloat", "useVariable":true, "name":null, "value":X}` is PlayMaker
   **None** — the action ignores it and `X` is meaningless serialisation residue. `useVariable:true`
   with a non-null `name` is a variable binding. `useVariable:false` is a literal.
   This is load-bearing: e.g. `FSM#…/Control/Hard Land` `SetVelocity2d` has `x` = None and `y` = 0,
   so it zeroes **only y** (`ACT/SetVelocity2d.cs:64–80`), which is why the trace shows
   `vel=(-22.101, 0.000)` on that state's first frame (`TR:r2_rand1.a.hktrace@354`).
2. `variables[]` values are **live values read at dump time** (`oracle/Oracle/FsmDumper.cs:160`
   → `Variables(f.Variables)` → `VarList` reads `NamedVariable` at `FsmDumper.cs:265–300`), captured at
   `Hooks.SceneReady` after the process had already run **29672** frames
   (`analysis/dumps/GG_Hornet_1/meta.json` `frameCountAtDump`, `timestampUtc` 2026-08-31T02:49:29Z).
   For most floats these equal the serialised defaults; for the detector bools (`A Dash Range` etc.)
   and for `Box Size */Box Off *` they can be episode residue. Treated as "initial value" only where
   §2.3 says so. The same caveat applies to `SCENE#` component fields — e.g. the body's
   `DamageHero.damageDealt` reads 0 there because `Control` was in `GG Fall`, i.e. after
   `GG Intro 1` set it to 0 and before `GG Reset` sets it to 1.
3. Every FSM in Hornet's hierarchy, plus `Needle/Control` and `Needle Tink/Setup and Follow`, has
   `restartOnEnable: true` (FSM dump, top-level flag on each `fsms[]` entry — 18/18 checked).
   `Fsm.OnEnable` (`PM/Fsm.cs:1839–1855`) therefore resets `ActiveState = GetState(startState)` and
   re-runs `Start()` on **every** `SetActive(true)`. This is load-bearing for the needle (§1.2).

---

## 1. Hierarchy, components and colliders

`Boss Holder/Hornet Boss 1` is the only `HealthManager` in the scene (`BOSSES#count` = 1). The table
below is the complete `GetComponentsInChildren` walk from `BOSSES#components`, joined with
`BOSSES#colliders`. `activeSelf` is the state at `SceneReady`.

| Path (under `Boss Holder/`) | layer | activeSelf | Collider (type, size/params, offset, isTrigger) | Components |
|---|---|---|---|---|
| `Hornet Boss 1` | 11 Enemies | true | `BoxCollider2D` size (1.393576, 1.156178) off (0.148401, −0.968784), **not** trigger | Transform, MeshFilter, MeshRenderer, tk2dSprite, **tk2dSpriteAnimator**, **Rigidbody2D**, BoxCollider2D, **PlayMakerFSM ×2** (`Control`, `Stun Control`), SpriteFlash, SetZ, AudioSource, **PlayMakerFixedUpdate**, **DamageHero**, ExtraDamageable, **HealthManager**, EnemyDeathEffectsUninfected, EnemyHitEffectsUninfected, EnemyDreamnailReaction, **Recoil**, DeactivateIfPlayerdataTrue, **ConstrainPosition**, PlayMakerUnity2DProxy |
| `Hornet Boss 1/Hit GDash` | 22 Enemy Attack | **false** | `PolygonCollider2D`, 1 path, 3 pts `(−0.3343,−0.6715) (−0.3788,−0.9065) (−2.1490,−0.8611)`, trigger | Transform, PolygonCollider2D, NonBouncer, **DamageHero** |
| `Hornet Boss 1/Hit ADash` | 22 Enemy Attack | **false** | `PolygonCollider2D`, same 3 pts as Hit GDash, trigger | Transform, PolygonCollider2D, NonBouncer, **DamageHero** |
| `Hornet Boss 1/Sphere Ball` | 22 Enemy Attack | **false** | `CircleCollider2D` r = 2.53, off (−0.0625, −0.28125), trigger | Transform, MeshFilter, MeshRenderer, tk2dSprite, tk2dSpriteAnimator, NonBouncer, CircleCollider2D, PlayMakerFSM (`Grow`), **DamageHero** |
| `Hornet Boss 1/A Dash Range` | 13 Hero Detector | true | `PolygonCollider2D`, 1 path, 8 pts (see below), trigger | Transform, PolygonCollider2D, PlayMakerFSM (`FSM`), PlayMakerUnity2DProxy |
| `Hornet Boss 1/Sphere Range` | 13 Hero Detector | true | `CircleCollider2D` r = 3.43, off (−0.0625, −0.28125), trigger | Transform, CircleCollider2D, PlayMakerFSM, PlayMakerUnity2DProxy |
| `Hornet Boss 1/A Sphere Range` | 13 Hero Detector | true | `BoxCollider2D` size (13.05, 35.90872), off (0, 0.7218628), trigger | Transform, PlayMakerFSM, BoxCollider2D, PlayMakerUnity2DProxy |
| `Hornet Boss 1/Evade Range` | 13 Hero Detector | true (collider **disabled**) | `BoxCollider2D` size (7.620083, 6.234100), off (0, 1.8175373), trigger, `enabled:false` at dump | Transform, **PlayMakerFSM ×2** (`FSM`, `Fluctuate`), BoxCollider2D, PlayMakerUnity2DProxy |
| `Hornet Boss 1/Run Away Check` | 13 Hero Detector | true | `BoxCollider2D` size (9.49, 4.455041), off (0, 0.928009), trigger | Transform, PlayMakerFSM, BoxCollider2D, PlayMakerUnity2DProxy |
| `Hornet Boss 1/Refight Range` | 13 Hero Detector | true | `BoxCollider2D` size (43.848213, 3.661576), off (−1.077723, 0.080992), trigger | Transform, BoxCollider2D, PlayMakerFSM, PlayMakerUnity2DProxy |
| `Hornet Boss 1/Evade Check` | **14 Terrain Detector** | true | `BoxCollider2D` size (3.512352, 1.0), off (1.256176, 0), trigger | Transform, PlayMakerFSM, BoxCollider2D, **PlayMakerFixedUpdate**, PlayMakerUnity2DProxy |
| `Hornet Boss 1/A Dash Effect` | 11 | false | — | tk2dSprite(+Animator), PlayMakerFSM (`FSM`) |
| `Hornet Boss 1/G Dash Effect` | 11 | false | — | tk2dSprite(+Animator), PlayMakerFSM |
| `Hornet Boss 1/Throw Effect` | 11 | false | — | tk2dSprite(+Animator), PlayMakerFSM |
| `Hornet Boss 1/Flash Effect` | 11 | false | — | tk2dSprite(+Animator), PlayMakerFSM |
| `Hornet Boss 1/Dust HardLand` | 0 | true | — | ParticleSystem, ParticleSystemRenderer, SetParticleScale |
| `Hornet Boss 1/Corpse Hornet GG(Clone)` | 11 | false | `BoxCollider2D` size (2.07, 3.359375), off (−0.023438, 0.132813), not trigger | tk2dSprite(+Animator), BoxCollider2D, Rigidbody2D, PlayMakerFSM (`Control`), AudioSource, NonBouncer, PlayMakerFixedUpdate |
| `…/Corpse Hornet GG(Clone)/{Thread, Grass, Grass Escape, Start Pt, Leave Anim}` | 13/11/11/16/11 | mixed | — | death-only cosmetics |

All rows: `BOSSES#components` and `BOSSES#colliders`; every collider row is independently confirmed
by `SCENE#<path>`, which additionally gives world-space corner lists (`world[]`) and the non-Unity
component fields used in §1.1, §3 and §5.

`A Dash Range` polygon (local, `BOSSES#colliders[3].paths[0]`): `(0.0271,0.2417) (25.4355,18.4062)
(26.4515,−21.5487) (7.3552,−21.3394) (0.0080,−0.1862) (−6.9610,−21.2192) (−25.1917,−21.3371)
(−24.8680,19.8310)` — a bow-tie that excludes a wedge directly below Hornet. In world space at the
dump pose (`SCENE#Boss Holder/Hornet Boss 1/A Dash Range.world[0]`): `(31.1471,44.8365)
(56.5555,63.0010) (57.5715,23.0460) (38.4752,23.2553) (31.1280,44.4086) (24.1590,23.3755)
(5.9283,23.2576) (6.2520,64.4258)` — i.e. far larger than the arena (`x ∈ [15.07, 37.96]`, §3), so
the only geometry that matters is the excluded wedge under her.

### 1.1 Damage-dealing hitboxes ("damages_hero")

There is **no FSM named `damages_hero` anywhere in the scene** — the string search over all 962 FSMs
in `analysis/fsm/GG_Hornet_1.json` returns zero. Contact damage on Hornet is component-driven:
`DamageHero` (`HK/DamageHero.cs:3–29`, fields `damageDealt` default 1 at `:5`, `hazardType` default 1
at `:7`) is read by `HeroBox` and applied via `heroCtrl.TakeDamage(damagingObject, collisionSide,
damageDealt, hazardType)` (`HK/HeroBox.cs:61`, `:91`). Four objects carry `DamageHero`:
`Hornet Boss 1` (body), `Hit GDash`, `Hit ADash`, `Sphere Ball`. The thrown `Needle` also damages
(see §1.2).

`scene.json` carries the live `DamageHero` fields for all five:

| Object | `damageDealt` | `hazardType` | `shadowDashHazard` | `resetOnEnable` | cite |
|---|---|---|---|---|---|
| `Boss Holder/Hornet Boss 1` (body) | **0 at dump** (see below) | 1 | false | false | `SCENE#Boss Holder/Hornet Boss 1` |
| `…/Hit GDash` | 1 | 1 | false | false | `SCENE#Boss Holder/Hornet Boss 1/Hit GDash` |
| `…/Hit ADash` | 1 | 1 | false | false | `SCENE#Boss Holder/Hornet Boss 1/Hit ADash` |
| `…/Sphere Ball` | 1 | 1 | false | false | `SCENE#Boss Holder/Hornet Boss 1/Sphere Ball` |
| `Needle` | 1 | 1 | false | false | `SCENE#Needle` |

The body's 0 is a dump-time snapshot, not the fight value: the Control FSM writes it at runtime —
`SetDamageHeroAmount damageDealt=0` in `FSM#…/Control/GG Intro 1` and `=1` in
`FSM#…/Control/GG Reset` (`HK/SetDamageHeroAmount.cs:24–27`), and the dump was taken with `Control`
in `GG Fall`, between the two. `resetOnEnable=false` on all five means `DamageHero.OnEnable`
(`HK/DamageHero.cs:15–28`) never restores an initial value, so the FSM write persists across the
`ActivateGameObject` toggles.

Trace corroboration: `HERO_DAMAGE{source:'Hornet Boss 1'|'Hit ADash'|'Needle'|'Sphere Ball',
amount:1, hazard_type:1}` in `TR:r2_rand1.a.hktrace` (F24994, F25224, F25228) and
`TR:r2_rand2.a.hktrace` (F25169). `Hit GDash` is unobserved in the R2 corpus.

### 1.2 Pooled / detached projectiles

`Needle` and `Needle Tink` are **not** under `Boss Holder/Hornet Boss 1` at `SceneReady` — both the
FSM dump (`FSM#Needle/Control`, `FSM#Needle Tink/Setup and Follow`) and `scene.json`
(`SCENE#Needle`, `SCENE#Needle Tink`) list them at scene root. They start as `Hornet Boss 1/Needle`
and `…/Needle/Needle Tink`; two independent code paths de-parent them, and both are in
`FSM#Needle Tink/Setup and Follow`:

- `Setup` — `GetOwner→$Self`; `GetParent(Owner)→$Needle`; `GetParent($Needle)→$Hornet`;
  **`SetParent(gameObject=Owner, parent=None)`** → de-parents **Needle Tink** itself;
  `SetFsmGameObject($Hornet, "Control", "Needle Tink", $Self)` — **this, not the `Pause`-state
  `FindChild` chain, is what Control's `$Needle Tink` ends up holding**;
  `GetFsmGameObject($Needle, "Control", "Parent")→$Hornet`.
- `Deparent` — `ActivateGameObject($Needle, activate=false)` (deactivates the needle);
  `SetMeshRenderer($Needle, true)`; **`SetParent(gameObject=$Needle, parent=None)`** → de-parents
  the **Needle**.

`ACT/SetParent.cs:30–46` assigns `transform.parent` (`:35`) and, only if the corresponding flags are
set, zeroes `localPosition`/`localRotation` (`:36–43`). Both flags are literal `false` in both
states (FSM dump), so neither is applied. Whether the assignment preserves the world pose is **not
stated by any source in `analysis/` → see Q-hornet-3**; do not port either assumption.

`FSM#…/Control/Pause` still runs `FindChild(Owner,"Needle")→$Needle` and
`FindChild($Needle,"Needle Tink")→$Needle Tink`; the second is only valid before `Setup` runs, and is
then overwritten by `Setup`'s `SetFsmGameObject`. (`FSM#…/Control/Init`'s duplicate `FindChild` for
`$Needle Tink` is disabled.)

Consequence for the sim: the needle is a *scene-root sibling* that Hornet drives by
`SetPosition`/`SetVelocityAsAngle`/`ActivateGameObject` on a cached GameObject reference. It is not
pooled (`ActivateGameObject`, not `Spawn`).

| Object | FSM | What it is |
|---|---|---|
| `Needle` | `Control` (6 states), `restartOnEnable:true` | the thrown needle; `PolygonCollider2D`, layer **22 Enemy Attack**, trigger, 1 path of 3 points; `DamageHero` 1/1; `NonBouncer`; **Rigidbody2D Dynamic, gravityScale 0, Continuous** (`SCENE#Needle.rigidbody`; corrected per REVIEW-p1 R2-1 — moved by `SetVelocity2d`/iTween, not by gravity). Child `Needle/Thread` has **no Collider2D** (absent from `scene.json`'s 1251 rows) — cosmetic |
| `Needle Tink` | `Setup and Follow` (4 states) | `BoxCollider2D` size (3.2466042, 0.251131058), offset (−0.070205, −1.907e-06), layer **17 Attack**, trigger, `enabled:false` at dump (`SCENE#Needle Tink`). De-parented to root by its own `Setup`; `Follow` copies `Needle`'s world pos+rot every frame (`FSM#Needle Tink/Setup and Follow/Follow`, all four actions `everyFrame=true`). It has **no `DamageHero`** — layer 17 `Attack` collides with 17 `Attack` and 11 `Enemies` (`PHYS#layerCollisionMatrix.ignoreLayerCollision[17]`), i.e. it is the surface the knight's nail clinks off, not a damage source |
| `Stun Effect` | `Stun Effect` (pooled clone) | `SpawnObjectFromGlobalPool` target in `FSM#…/Control/Stun Start` (`ACT/SpawnObjectFromGlobalPool.cs:69` → `ObjectPoolExtensions.Spawn`); no Collider2D (absent from `scene.json`); cosmetic |
| `Slash Effect Ghost1/2`, `Corpse Splat`, geo prefabs | — | death only (`FSM#…/Corpse Hornet GG(Clone)/Control/Blow`, `BOSSES#fields`) |

`Needle`'s dump-time world polygon is `[(30.7402,44.4960) (30.7470,44.7279) (33.2165,44.5965)]` and
`Needle Tink`'s world box is `[(33.0935,44.7203) (29.8469,44.7203) (29.8469,44.4692) (33.0935,44.4692)]`
(`SCENE#Needle.world`, `SCENE#Needle Tink.world`) — a ~2.5-unit-long sliver and a 3.25 × 0.25 bar,
both horizontal, consistent with the horizontal-only throw of §2.4.

`FSM#Needle/Control`:

| State | Transitions | Actions (decisive fields) |
|---|---|---|
| `Init` | FINISHED→`Out` | `FindChild(Owner,"Thread")→$Thread`; `GetPosition(Owner, space=World)→$Return Vector`. (`GetParent` disabled.) |
| `Out` | FINISHED→`Decel` | `Wait 0.3`; `SetRotation zAngle=0 World`; `FaceAngleV2 angleOffset=180 worldSpace=true everyFrame=true` |
| `Decel` | FINISHED→`Return` | `DecelerateV2 deceleration=0.8`; `GetSpeed2d→$Speed everyFrame`; `FloatCompare($Speed, 0, tolerance=0.5) equal/lessThan→FINISHED everyFrame` |
| `Return` | FINISHED→`Notify` | `iTweenMoveTo vectorPosition=$Return Vector, speed=30, easeType=easeInSine, space=Self, finishEvent=FINISHED`; `DecelerateV2 0.8`; `ActivateGameObject($Thread, true)` |
| `Notify` | — | `SendEventByName BroadcastAll "NEEDLE RETURN"` (this is what releases `Control/Thrown`) |
| `Destroy` | — | `DestroySelf` (global transition on `HORNET KILLED`) |

Global transition: `HORNET KILLED → Destroy`.

**`$Return Vector` is re-captured on every throw, not once at scene start.** `Needle/Control` has
`restartOnEnable: true` (FSM dump), so `Fsm.OnEnable` resets it to `startState` = `Init` and re-runs
`Start()` on every enable (`PM/Fsm.cs:1847–1854`). `FSM#…/Control/Throw` runs
`SetPosition($Needle, $Self Pos)` (action index 6) **before** `ActivateGameObject($Needle, true)`
(index 7), so the `GetPosition(Owner, space=World)→$Return Vector` in `Init` reads the throw origin
(Hornet's position with y − 0.5), and the `Return` state's `iTweenMoveTo` flies the needle back to
where it left Hornet's hand. The dump corroborates the restart: `Needle/Control` shows
`started:true, finished:true, activeState:""` — i.e. it had run to `Notify`/`Destroy` and been
stopped, not left mid-flight. The dumped `$Return Vector` value (31.4000015, 28.59476, 0.006) is the
last-throw origin, not a constant. This depends on `SetActive(true)` invoking `OnEnable`
synchronously, before the FSM's next `Update` — see **Q-hornet-4**.

---

## 2. Control FSM

`FSM#Boss Holder/Hornet Boss 1/Control` — 77 states, `startState` = `Pause`, one global transition
`STUN → Stun Start`. Second FSM on the same object: `Stun Control` (§2.6).

### 2.1 Variables (values as read at `SceneReady`; see the caveat at the top)

Floats — arena/tuning constants:

| Name | Value | Role |
|---|---|---|
| `Gravity` | 1.5 | `rb2d.gravityScale` in the grounded/airborne states |
| `Floor Y` | 27.55 | A-Dash floor test + `Land Y` snap |
| `Roof Y` | 40.54 | A-Dash roof test + `Hit Roof` snap |
| `Wall X Left` | 15.13 | A-Dash left-wall test + `Wall L` snap |
| `Wall X Right` | 37.90 | A-Dash right-wall test + `Wall R` snap |
| `Left X` | 16.06 | jump-target sample lower bound |
| `Right X` | 36.53 | jump-target sample upper bound |
| `Throw X L` | 22.51 | "far enough left" gate for Throw |
| `Throw X R` | 30.16 | "far enough right" gate for Throw |
| `Sphere Y` | 33.80 | air-sphere height gate |
| `Min Dstab Height` | 33.31 | **unused** — no action in the FSM reads it |
| `Run Speed` | −8.0 | ×`X Scale` |
| `G Dash Speed` | −25.0 | ×`X Scale` |
| `A Dash Speed` | 30.0 | magnitude for `SetVelocityAsAngle` |
| `Evade Speed` | 22.0 | ×`X Scale` |
| `Stun Air Speed` | 10.0 | ×`X Scale` |
| `Throw Speed` | 38.0 | needle launch speed |
| `Idle Wait Min` / `Max` | 0.5 / 0.75 | pre-escalation |
| `Run Wait Min` / `Max` | 0.5 / 1.0 | pre-escalation |
| `Air Dash Height` | 31.5 | **unused** |
| `Jump X` / `Jump Y` | scratch | written by `Aim Jump`/`Jump` |
| `Angle`, `Self X/Y`, `X Scale`, `Y Velocity`, `Return X Scale`, `*Crt`, `Air Dash Pause` | scratch | |

Ints: `Ct A Sphere, Ct Airdash, Ct G Dash, Ct G Sphere, Ct Idle, Ct Miss, Ct Run, Ct Throw` (all 0),
`Ms A Sphere, Ms Airdash, Ms G Dash, Ms Throw` (all 0), `HP` (0), `Hornet State` (0).

Bools: `A Dash Range` (true at dump — runtime), `A Sphere Range, Above Air Dash Height,
Below Sphere Y, Escalated, Evade Check, Evade Range, Falling, Hero Is Right, Over Min Height,
Over Throw R, Refight Range, Run Away Check, Sphere Range, Under Throw L, Will Sphere` (false).

Vector2 collider presets (the *authoritative* box sizes; the dumped `BoxCollider2D.size` is residue):

| Name | size | matching offset | value |
|---|---|---|---|
| `Box Size Idle` | (0.8946984, 2.564674) | `Box Off Idle` | (0.1200523, −0.2645378) |
| `Box Size Antic` | (1.229008, 1.380672) | `Box Off Antic` | (1.081188, −0.8565407) |
| `Box Size GDash` | (1.563310, 1.506035) | `Box Off Gdash` | (0.05040932, −0.7938576) |
| `Box Size ADash` | (1.460247, 1.025071) | `Box Off Adash` | (0.1019421, 0.0) |
| `Box Size Throw` | (0.9817337, 2.564674) | `Box Off Throw` | (0.9967937, −0.2645378) |
| `Box Size Throwing` | (1.393576, 1.156178) | `Box Off Throwing` | (0.1484013, −0.9687843) |

GameObject refs: `Self`, `Hero Obj`(=`Knight`), `Needle`, `Needle Tink`, `Sphere Ball`,
`Flash Effect`, `A Dash Effect`, `G Dash Effect`, `Throw Effect`, `Hit ADash`, `Hit GDash`,
`Dust HardLand`, `Area Title`, `Hornet Saver`.

### 2.2 Intro / gating chain

`Pause` → `Init` → `Inert` → (`GG BOSS`) → `GG Intro 1` → `GG Fall` → `GG Land` → `GG Reset` →
`GG Music` → `Flourish` → `Idle`.

| State | Actions (decisive) | Exit |
|---|---|---|
| `Pause` | `NextFrameEvent`; `FindChild(Owner,"Needle")→$Needle`; `FindChild($Needle,"Needle Tink")→$Needle Tink` | FINISHED (next Update, `ACT/NextFrameEvent.cs:19–23`) |
| `Init` | `GetOwner→$Self`; `SetGameObject($Hero Obj = $Hero)`; `SetGravity2dScale(Owner, $Gravity=1.5)` | FINISHED |
| `Inert` | `GGCheckIfBossScene → GG BOSS` (`HK/GGCheckIfBossScene.cs:16–27`, branches on `BossSceneController.IsBossScene`); `GetPlayerDataInt("hornetGreenpath")→$Hornet State`; `IntCompare($Hornet State, 4) equal/greaterThan→REFIGHT` | GG BOSS in the GG scene |
| `GG Intro 1` | `SetInvincible(Owner, Invincible=true)`; `SetDamageHeroAmount(Owner, 0)`; `SetIsKinematic2d(true)`; `Translate(Owner, y=+16, World)`; `Wait 2.0` | FINISHED |
| `GG Fall` | `SetIsKinematic2d(false)`; `Tk2dPlayAnimation "Fall"`; `CheckCollisionSide`+`Enter` bottom→LAND; `SetCollider(Owner,true)`; `SetMeshRenderer(Owner,true)` | LAND |
| `GG Land` | `Tk2dPlayAnimationWithEvents "Land"`→FINISHED; `SetVelocity2d x=0 y=0`; `SetGravity2dScale $Gravity`; box←Idle; `SetRotation z=0 World`; `SetScale y=1` | FINISHED |
| `GG Reset` | `SetInvincible(Owner, Invincible=false, InvincibleFromDirection=0)`; `SetDamageHeroAmount(Owner, 1)` | FINISHED |
| `GG Music` | `SendEventByName BroadcastAll "GG MUSIC"` (2 audio actions disabled) | FINISHED |
| `Flourish` | `Tk2dPlayAnimationWithEvents "Flourish"`→FINISHED; `AudioPlayerOneShotSingle`; area-title writes | FINISHED |

**The intro brackets Hornet's invincibility.** `GG Intro 1`'s `SetInvincible` has
`Invincible = {useVariable:false, value:true}` and `GG Reset`'s has
`Invincible = {useVariable:false, value:false}` — both **literals**, not None (dump rule 1 above).
`HK/SetInvincible.cs:29–32` writes `component.IsInvincible = Invincible.Value` whenever
`!Invincible.IsNone`, so `GG Intro 1` sets `HealthManager.IsInvincible = true` and `GG Reset` sets it
back to `false`; both also write `InvincibleFromDirection = 0` (`:33–36`).
`BOSSES#isInvincible` / `BOSSES#fields.invincible` / `SCENE#Boss Holder/Hornet Boss 1`
`HealthManager.invincible` all read `true` **only because the dump was taken with `Control` in
`GG Fall`**, i.e. between the two writes. `preventInvincibleEffect` = true.

Trace verification (`entity.invincible` = `HealthManager.IsInvincible`, docs/trace-format.md ENTITY
block): `r2_rand1.a` **True on 42 FRAMEs, False on 438**; first `False` at `Time.frameCount` 24768 =
`TR:r2_rand1.a.hktrace@42`, the first `Flourish` record — the frame after
`GG Land → GG Reset → GG Music → Flourish` all fire at F24766. Identical split in `r2_rand2.a`
(42/438, first False at F24863@42) and `r2_move.a` (42/558, first False at F24827@42). There is no
missing "clearing agent"; `GG Reset` is it.

`Refight` path (non-GG): `Refight Ready` (waits on `$Refight Range` `everyFrame`) → `Refight Wake` →
`Music` → `Flourish`. `Wake` path: `Set Scale (x=−1)` → `Wake` → `Music`. Both dead in `GG_Hornet_1`
because `Inert` takes `GG BOSS` first (action order, `PM/FsmState.cs:286–317`).

### 2.3 The decision core

```
Idle ──RUN──► Flip? ──► Run Away? ──► Run Antic ──► Run
 │                                                   │
 └──FINISHED──────────────► G Sphere? ◄──────────────┘
                              │  SPHERE G ──► Sphere Antic G ► Sphere ► Sphere Recover ► Escalation
                              │  FINISHED ──► Can Throw?
                                                │ CAN THROW  ──► Move Choice A (4-way)
                                                │ CANT THROW ──► Move Choice B (3-way)
Move Choice A/B ──► AIRDASH  ► Set ADash    ► Jump Antic ► Aim Jump ► Jump ► In Air ► ADash Antic ► Fire ► A Dash
                 ├─ SPHERE A ► Set Sphere A ► Jump Antic ► Aim Jump ► (Aim Sphere Jump) ► Jump ► In Air ► Do Sphere? ► Sphere Antic A ► Sphere A ► …
                 ├─ G DASH   ► GDash Antic ► G Dash ► GDash Recover1 ► GDash Recover2 ► Escalation
                 └─ THROW    ► Throw Antic ► Lock? ► [Lock L/R/UL/UR] ► Throw ► Thrown ► Throw Recover ► Escalation
Escalation ──► Idle       (every attack chain funnels back through Escalation)
```

`Idle` (`FSM#…/Control/Idle`), action order — and PlayMaker aborts the remaining `OnEnter` calls the
moment an action queues a state switch (`PM/FsmState.cs:307–310`, `PM/Fsm.cs:2339–2343`):

1. `FaceObject($Self, $Hero Obj)` — `ACT/FaceObject.cs:76–133`, sets `localScale.x = ±|scale.x|`.
2. `SetFloatValue($Air Dash Pause = 999)`.
3. `SetBoxCollider2DSizeVector(Owner, $Box Size Idle, $Box Off Idle)`.
4. `Tk2dPlayAnimation "Idle"`.
5. `SetVelocity2d(Owner, x=0)` — **y is None**, y untouched.
6. `BoolTest($Evade Range) isTrue→EVADE` → `Evade Antic`. **If true, actions 7–9 never run.**
7. `SendRandomEvent events=[null, RUN] weights=[0.5,0.5] delay=0` → 50 % `RUN` (→`Flip?`),
   50 % a null event, which `PM/Fsm.cs:2025` drops. **1 RNG draw** (`ACT/SendRandomEvent.cs:28`).
8. `SendRandomEventV2 events=[IDLE, RUN] weights=[0.5,0.5] trackingInts=[$Ct Idle,$Ct Run]
   eventMax=[2,2]` — rejection loop, ≥1 RNG draw (`ACT/SendRandomEventV2.cs:26–45`).
   `IDLE` has **no transition on this state**, so drawing `IDLE` is a no-op that still bumps `Ct Idle`.
9. `WaitRandom($Idle Wait Min, $Idle Wait Max) finishEvent=FINISHED` — **1 RNG draw**
   (`ACT/WaitRandom.cs:34`), then ticks in Update (`:55, :57`).

`Run` (`FSM#…/Control/Run`): `BoolTest($Evade Range)→EVADE`; `GetScale→$X Scale`;
`FloatOperator($Run Speed × $X Scale)→$Run Speed Crt`; `Tk2dPlayAnimation "Run"`;
`SetVelocity2d x=$Run Speed Crt` (y None); `WaitRandom($Run Wait Min,$Run Wait Max)→FINISHED`;
`CheckCollisionSide right/leftHitEvent=FINISHED` with `otherLayer=false` ⇒ layer 8 Terrain
(`ACT/CheckCollisionSide.cs:131–138`). So Run ends on the timer **or** on touching a wall.

`Flip?`: `SendRandomEvent [null, FINISHED] [0.5,0.5]` then `FlipScale(Owner, horizontally)`. If
`FINISHED` is drawn the state switch is queued and `FlipScale` is **skipped**; if null is drawn
`FlipScale` runs and the state finishes normally. Both reach `Run Away?`.

`Run Away?`: `BoolTest($Run Away Check) isFalse→FINISHED`; `FaceObject`; `FlipScale`. When the hero
is **inside** `Run Away Check` (9.49 × 4.455 box) she faces the hero and then flips → runs away.

`G Sphere?`: `AudioStop`; `BoolTest($Sphere Range) isFalse→FINISHED`;
`SendRandomEventV2 events=[SPHERE G, FINISHED] weights=[0.2, 0.8] trackingInts=[$Ct G Sphere,$Ct Miss]
eventMax=[1, 5]`. So the ground sphere is only offered when the hero is inside the r = 3.43 `Sphere Range`
circle, is at most 1-in-a-row, and is **forced** once 5 consecutive non-spheres have been drawn
(the `FINISHED` branch gets rejected at `Ct Miss == 5`).

`Can Throw?` — pure geometry, no RNG:
`GetPosition(Owner)→$Self X`; `FloatTestToBool($Self X > $Throw X R=30.16)→$Over Throw R`;
`FloatTestToBool($Self X < $Throw X L=22.51)→$Under Throw L`;
`CheckTargetDirection(Owner,$Hero Obj) rightBool=$Hero Is Right`;
`BoolTestMulti([$Over Throw R,$Hero Is Right],[true,false])→CAN THROW`;
`BoolTestMulti([$Under Throw L,$Hero Is Right],[true,true])→CAN THROW else CANT THROW`.
I.e. **CAN THROW ⟺ (x > 30.16 ∧ hero is to the left) ∨ (x < 22.51 ∧ hero is to the right)** — she must
be on the far side of the arena from the hero. `CheckTargetDirection` uses strict comparisons and
sets all four bools every call (`ACT/CheckTargetDirection.cs:73/82/91/100`).

`Move Choice A` — `SendRandomEventV3` (the disabled `SendEvent THROW` above it is a designer override,
`enabled:false`):

| i | event | weight | trackingInt | eventMax | trackingIntMissed | missedMax |
|---|---|---|---|---|---|---|
| 0 | AIRDASH | 0.25 | `Ct Airdash` | 2 | `Ms Airdash` | 5 |
| 1 | SPHERE A | 0.25 | `Ct A Sphere` | 1 | `Ms A Sphere` | 7 |
| 2 | G DASH | 0.25 | `Ct G Dash` | 2 | `Ms G Dash` | 5 |
| 3 | THROW | 0.25 | `Ct Throw` | 1 | `Ms Throw` | 3 |

`Move Choice B` — `SendRandomEventV3`, weights `[0.33, 0.33, 0.34]` (sum 1.00), events
`[AIRDASH, SPHERE A, G DASH]`, `eventMax [2,1,2]`, `missedMax [5,7,5]`, same `Ct*`/`Ms*` variables as
A's first three. Because V3 zeroes/increments only **its own** arrays
(`ACT/SendRandomEventV3.cs:54–58, 66–70`), `Ct Throw`/`Ms Throw` are untouched by B — so a long run of
`CANT THROW` freezes `Ms Throw`, and Throw is not starvation-forced while she is mid-arena.

`Dmg Response` (entered from `Idle`/`Run` on `TOOK DAMAGE`, which `HealthManager` raises at
`HK/HealthManager.cs:443`): `AudioStop`;
`SendRandomEvent [EVADE, JUMP, ATTACK, IDLE] weights [0.3, 0.15, 0.15, 0.4]`.
`Dmg Idle`: `WaitRandom 0.25–0.4`; `CheckCollisionSide right/left→FINISHED`.
`After Evade`: `SendRandomEvent [ATTACK, IDLE] [0.5, 0.5]` → `G Sphere?` / `Escalation`.

`Escalation` (funnel state, `FSM#…/Control/Escalation`):
`BoolTest($Escalated) isTrue→FINISHED`;
`GetFsmInt(Owner, fsmName="health_manager_enemy", "HP")→$HP`;
`IntCompare($HP, 90) greaterThan→FINISHED`;
then `$Escalated=true`, `$Run Wait Min=0.35`, `$Run Wait Max=0.75`, `$Idle Wait Min=0.1`,
`$Idle Wait Max=0.4`.

**There is no PlayMakerFSM named `health_manager_enemy` on `Boss Holder/Hornet Boss 1`** — the object
has exactly two, `Control` and `Stun Control` (`BOSSES#components[0].components`, and the FSM dump
lists only those two at that path). `ActionHelpers.GetGameObjectFsm` falls through to
`go.GetComponent<PlayMakerFSM>()` when the named FSM is missing
(`HK/HutongGames.PlayMaker/ActionHelpers.cs:56–71`: name loop `:60–67`, `Debug.LogWarning` `:68`,
fallback `return go.GetComponent<PlayMakerFSM>()` `:70`), i.e. it resolves to whichever
`PlayMakerFSM` `GetComponent` returns. **Which one that is does not matter here**: neither `Control`
nor `Stun Control` declares an int named `HP`, and `FsmVariables.GetFsmInt` logs the miss and returns
a **fresh `new FsmInt(name)`** — a throw-away with value 0 that is not the action's `storeValue`
variable (`PM/FsmVariables.cs:1304–1308`). `$HP` therefore stays at its initial 0, `0 > 90` is false, and
**escalation fires unconditionally the first time any attack chain completes** — the HP gate is dead
code in this build. Trace corroboration (verification only): `$Escalated` flips false→true at
`TR:r2_rand1.a.hktrace@255` while `$HP` stays 0 and `entity.hp` is 900, and `$Idle Wait Min/Max`
become 0.1/0.4, `$Run Wait Min/Max` 0.35/0.75 at the same index.

### 2.4 Attack chains — state by state

**Ground dash (G Dash).**

| State | Actions |
|---|---|
| `GDash Antic` | box←Antic; `Tk2dPlayAnimationWithEvents "G Dash Antic"`→FINISHED; `FaceObject`; `SetVelocity2d x=0`; `AudioPlayerOneShot` (2 clips) |
| `G Dash` | `Tk2dPlayAnimation "G Dash"`; `AudioPlaySimple`; `ActivateGameObject($G Dash Effect,true)`; camera shake; box←GDash; **`ActivateGameObject($Hit GDash, true)`**; `GetScale→$X Scale`; `FloatOperator(−25 × $X Scale)→$G Dash Speed Crt`; `SetVelocity2d x=$G Dash Speed Crt`; `Wait 0.35`→FINISHED; `CheckCollisionSideEnter` + `CheckCollisionSide` right/left→FINISHED (`otherLayer=false` ⇒ layer 8) **and `ignoreTriggers=true`** |
| `GDash Recover1` | `Tk2dPlayAnimationWithEvents "G Dash Recover1"`→FINISHED; box←Antic; **`ActivateGameObject($Hit GDash, false)`**; `DecelerateXY decelerationX=0.77` (Y is None → untouched) |
| `GDash Recover2` | `Tk2dPlayAnimationWithEvents "G Dash Recover2"`→FINISHED; box←Idle; `DecelerateXY decelerationX=0.75` |

**Air dash (A Dash).** `Set ADash` → `Jump Antic` → `Aim Jump` → `Jump` → `In Air` → `ADash Antic` →
`Fire` → `Firing L`/`Firing R` → `A Dash` → wall/floor/roof.

| State | Actions |
|---|---|
| `Set ADash` | `RandomFloat(0.15, 0.4)→$Air Dash Pause` (**1 RNG draw**); `SetBoolValue($Will Sphere = false)` |
| `Jump Antic` | box←Idle; `SetVelocity2d x=0 y=0`; `Tk2dPlayAnimationWithEvents "Jump Antic"`→FINISHED; `FaceObject` |
| `Aim Jump` | `RandomFloat($Left X=16.06, $Right X=36.53)→$Jump X` (**1 RNG draw**); `GetPosition→$Self X`; `FloatSubtract($Jump X −= $Self X)`; `FloatInRange($Jump X ∈ [−2.5, 2.5]) trueEvent=REDO`; `FloatMultiply($Jump X ×= 1.0)`. `REDO`→`Re Aim`(empty)→`Aim Jump`, i.e. **resample until the horizontal displacement exceeds 2.5** — each retry costs another draw. The `BoolTest($Will Sphere)→SPHERE A` action is **disabled**, so `Aim Sphere Jump` is unreachable from here. |
| `Jump` | `AudioPlaySimple`; `AudioPlayerOneShot` (3 clips); `Tk2dPlayAnimation "Jump"`; `NextFrameEvent→FINISHED`; `RandomFloat(41, 41)→$Jump Y` (**1 RNG draw**, degenerate); `SetVelocity2d x=$Jump X y=$Jump Y` |
| `In Air` | `CheckCollisionSide`+`Enter` bottom→LAND (layer 8); `GetPosition→$Self Y everyFrame`; `GetVelocity2d→$Y Velocity everyFrame`; `Wait($Air Dash Pause)→AIRDASH`; `FloatTestToBool($Y Velocity < 0)→$Falling everyFrame`; `FloatTestToBool($Self Y < $Sphere Y=33.8)→$Below Sphere Y everyFrame`; `BoolTestMulti([$Below Sphere Y,$Falling,$Will Sphere] all true)→SPHERE A everyFrame` |
| `ADash Antic` | `BoolTest($A Dash Range) isFalse→CANCEL`(→`In Air`); `GetAngleToTarget2D(Owner,$Hero Obj, offsetY=−0.5)→$Angle` (normalised to [0,360), `ACT/GetAngleToTarget2D.cs:73`); `Tk2dPlayAnimationWithEvents "A Dash Antic"`→FINISHED; `FaceObject`; `DecelerateV2 deceleration=0`(→ velocity 0); `SetGravity2dScale 0`; `AudioPlayerOneShot` |
| `Fire` | `SetBoxColliderTrigger(Owner, true)`; `SetVelocityAsAngle(Owner, $Angle, $A Dash Speed=30)`; box←ADash; **`ActivateGameObject($Hit ADash, true)`**; `SetScale x=1 y=1`; `FaceAngle(Owner, angleOffset=180)`; `GetRotation→$Angle World`; `FloatInRange($Angle ∈ [90,270])→FLIP Y` (`FireAtTarget` is disabled) |
| `Firing R` / `Firing L` | `SetScale y=−1` + `$Return X Scale=−1` / `$Return X Scale=+1` |
| `A Dash` | `ActivateGameObject($A Dash Effect,true)`; camera shake; `GetPosition→$Self X,$Self Y everyFrame`; `SetFloatValue($Air Dash Pause=999)`; four `FloatCompare everyFrame` (tolerance 0): `$Self Y ≤ $Floor Y`→LAND, `$Self Y ≥ $Roof Y`→ROOF, `$Self X ≤ $Wall X Left`→WALL L, `$Self X ≥ $Wall X Right`→WALL R; `Tk2dPlayAnimation "A Dash"` |
| `Land Y` | `SetPosition y=$Floor Y World`; `SetScale x=$Return X Scale`; `ActivateGameObject($Hit ADash,false)` |
| `Hard Land` | `PlayParticleEmitter($Dust HardLand, emit=0)`; `Tk2dPlayAnimationWithEvents "Hard Land"`→FINISHED; `DecelerateXY decelerationX=0.8`(Y None); `SetVelocity2d y=0` (**x None**); `SetBoxColliderTrigger false`; `SetGravity2dScale $Gravity`; box←Idle; `SetRotation z=0`; `SetScale y=1` |
| `Hit Roof` | `SetScale x=$Return X Scale y=1`; `ActivateGameObject($Hit ADash,false)`; `SetPosition y=$Roof Y`; `SetVelocity2d x=0 y=0`; trigger off; **`SetGravity2dScale 2`**; box←Idle; `SetRotation z=0` → `In Air` |
| `Wall L` | `SetRotation z=0`; `SetVelocity2d 0,0`; `SetScale x=1 y=1`; `SetPosition x=$Wall X Left`; `ActivateGameObject($Hit ADash,false)`; trigger off; box←Idle; `Tk2dPlayAnimationWithEvents "Wall Impact"`→FINISHED |
| `Jump R` | `SetVelocity2d x=10 y=20`; `Tk2dPlayAnimation "Jump"`; `SetGravity2dScale 2`; `SetScale x=−1` → `In Air` |
| `Wall R` / `Jump L` | mirror (`x=$Wall X Right`, scale x=−1; then `x=−10 y=20`, scale x=+1) |

Note both wall/roof recoveries set `gravityScale = 2`, not `$Gravity = 1.5`.

**Sphere (ground).** `Sphere Antic G` (`SetVelocity2d 0,0`; box←Idle; `Tk2dPlayAnimationWithEvents
"Sphere Antic G"`; `FaceObject`) → `Sphere` (`ActivateGameObject($Sphere Ball, true)`;
`ActivateGameObject($Flash Effect, true)`; `Tk2dPlayAnimation "Sphere Attack"`; **`Wait 1.0`**) →
`Sphere Recover` (`Tk2dPlayAnimationWithEvents "Sphere Recover G"`;
`ActivateGameObject($Sphere Ball, false)`) → `Escalation`.

**Sphere (air).** `Set Sphere A` (`$Will Sphere=true`; `$Air Dash Pause=999`) → `Jump Antic` →
`Aim Jump` → `Jump` → `In Air` → (`BoolTestMulti` fires `SPHERE A` once she is falling **and** below
y = 33.8) → `Do Sphere?` (`$Will Sphere=false`; `BoolTest($A Sphere Range) isTrue→FINISHED
isFalse→CANCEL`) → `Sphere Antic A` (`$Will Sphere=false`; anim; **`SetGravity2dScale 0`**;
`DecelerateV2 0.78`; `FaceObject`) → `Sphere A` (`ActivateGameObject($Sphere Ball,true)`;
`$Flash Effect` on; `Tk2dPlayAnimation "Sphere Attack"`; **`Wait 1.0`**; `DecelerateV2 0.78`) →
`Sphere Recover A` → `Sphere A End` (`Tk2dPlayAnimation "Fall"`; `SetGravity2dScale $Gravity`) →
`In Air`.

`Aim Sphere Jump` (`RandomFloat(Left X, Right X)`, subtract `$Self X`, **×1.25**) is only reachable
from `Aim Jump`'s `SPHERE A` transition, whose only producer (`BoolTest($Will Sphere)`) is disabled →
**dead state in this build**.

**Throw (needle).**

| State | Actions |
|---|---|
| `Throw Antic` | `GetAngleToTarget2D(Owner, $Hero Obj, offsets 0,0)→$Angle`; `SendEventByName(Owner, "STUN CONTROL STOP")`; box←Throw; `FaceObject`; `SetVelocity2d x=0`; `Tk2dPlayAnimationWithEvents "Throw Antic"`→FINISHED; `AudioPlayerOneShot` |
| `Lock?` | four `FloatInRange($Angle)` — [0,90]→LOCK R, [90,180]→LOCK L, [180,270]→LOCK UL, [270,360]→LOCK UR. Bounds are **inclusive both ends** (`ACT/FloatInRange.cs:56`), and the actions run in that order with the first match switching state, so exact 90/180/270 resolve to the earlier bucket |
| `Lock L`/`Lock UL` | `$Angle = 180` |
| `Lock R`/`Lock UR` | `$Angle = 0` |
| `Throw` | `GetPosition(Owner)→$Self Pos`; `ActivateGameObject($Throw Effect,true)`; box←Throwing; `Vector3AddXYZ($Self Pos.y −= 0.5)`; `SetPosition($Needle, $Self Pos, World)`; `ActivateGameObject($Needle, true)`; `Tk2dPlayAnimation "Throw"`; `SetVelocityAsAngle($Needle, $Angle, $Throw Speed=38)`; `NextFrameEvent→FINISHED`; `SetRecoilSpeed(Owner, 0)`; `SetCollider($Needle Tink, true)` |
| `Thrown` | **no actions**; waits for `NEEDLE RETURN` broadcast from `FSM#Needle/Control/Notify` |
| `Throw Recover` | `ActivateGameObject($Needle,false)`; `Tk2dPlayAnimationWithEvents "Throw Recover"`→FINISHED; `SetRecoilSpeed(Owner, 15)`; `SetCollider($Needle Tink,false)`; `SendEventByName(Owner,"STUN CONTROL START")` |

The `Lock?` quantisation means the needle is thrown **purely horizontally**, at angle 0 or 180 — the
`GetAngleToTarget2D` result is only used to pick the side. Trace: `Throw Antic → Lock? → Lock UL →
Throw` at `TR:r2_rand1.a.hktrace` F24970.

`Throw Antic` disables stun accumulation for the whole throw (`STUN CONTROL STOP` →
`FSM#…/Stun Control/Stop`) and `Throw Recover` re-enables it — Hornet **cannot be stunned mid-throw**.
Trace: `Stun Control: 'Idle'→'Stop'` at F24913 and `'Stop'→'Reset Counter'→'Idle'` at F25061
(`TR:r2_rand1.a.hktrace`).

**Evade / parry.** Hornet 1 has **no Counter/parry state**. The `Counter *` and `Barb *` clips exist
in the shared `Hornet Boss Anim` library (`BOSSES#animator.library.clips` ids 44–58) but **no state in
`FSM#…/Control` references them** — they belong to the Hornet 2 moveset. The evade is her only
defensive move:

| State | Actions |
|---|---|
| `Evade Antic` | `AudioStop`; `BoolTest($Evade Check) isTrue→CANCEL`(→`G Sphere?`); `SetBoolValue($Evade Range=false)`; `SendEventByName(Owner, sendToChildren=true, "EVADED")`; `SetVelocity2d x=0 y=0`; `FaceObject`; `Tk2dPlayAnimationWithEvents "Evade Antic"`→FINISHED |
| `Evade` | `Tk2dPlayAnimation "Evade"`; `GetScale→$X Scale`; `FloatOperator(22.0 × $X Scale)→$Evade Speed Crt`; `SetVelocity2d x=$Evade Speed Crt`; **`Wait 0.25`**; `CheckCollisionSideEnter right/left→FINISHED`; 2 audio actions |
| `Evade Land` | `Tk2dPlayAnimationWithEvents "Land"`→FINISHED; `SetVelocity2d x=0 y=0` |
| `After Evade` | `SendRandomEvent [ATTACK, IDLE] [0.5,0.5]` |

`$Evade Check` is the **terrain** probe (layer 14, box 3.512 × 1.0 offset +1.256 in front) — she
cancels the evade if she would back into a wall.

### 2.5 The range detectors (5 identical trigger FSMs + 1 fluctuator)

`Evade Range`, `A Dash Range`, `Sphere Range`, `A Sphere Range`, `Run Away Check`, `Refight Range`
all run the same 4-state FSM (`FSM#Boss Holder/Hornet Boss 1/<name>/FSM`):
`Init` (`GetOwner`, `GetParent→$Parent`) → `Detect`
(`Trigger2dEvent OnTriggerEnter2D→ENTER`, `Trigger2dEvent OnTriggerExit2D→EXIT`) →
`Enter`/`Exit` (`SetFsmBool($Parent, "Control", $Bool Name, true/false)`) → back to `Detect`.
`$Bool Name` is the string in the table below; `$FSM Name` is always `"Control"`.

| Detector | writes Control bool | layer | shape |
|---|---|---|---|
| `Evade Range` | `Evade Range` | 13 | Box 7.620 × 6.234, off (0, 1.8175) — **collider toggled by `Fluctuate`** |
| `A Dash Range` | `A Dash Range` | 13 | 8-point polygon (§1) |
| `Sphere Range` | `Sphere Range` | 13 | Circle r = 3.43, off (−0.0625, −0.28125) |
| `A Sphere Range` | `A Sphere Range` | 13 | Box 13.05 × 35.909, off (0, 0.72186) |
| `Run Away Check` | `Run Away Check` | 13 | Box 9.49 × 4.455, off (0, 0.92801) |
| `Refight Range` | `Refight Range` | 13 | Box 43.848 × 3.662, off (−1.0777, 0.08099) |

Layer 13 `Hero Detector` collides only with layer 9 `Player`
(`PHYS#layerCollisionMatrix.ignoreLayerCollision[13]` — false only for 3, 6, 7 and 9), so these
triggers see the knight and nothing else.

`Evade Check` is different (`FSM#…/Evade Check/FSM`): layer **14 Terrain Detector** (collides with 8
Terrain and 25 Soft Terrain), uses `OnTriggerEnter2D` **and `OnTriggerStay2D`** → `ENTER`, and holds
a 2-frame latch instead of using `OnTriggerExit2D`: `SetIntValue($Frames = 0)`,
`IntAddV2($Frames += 1) everyFrame` (**runs in FixedUpdate**, `ACT/IntAddV2.cs:25,:39` — hence the
`PlayMakerFixedUpdate` component on that object), `IntCompare($Frames ≥ 2)→EXIT`. `Enter` also carries
a `NextFrameEvent`.

`Evade Range/Fluctuate` (2 states, `FSM#…/Evade Range/Fluctuate`) gates when evading is possible at
all: `Off` — `WaitRandom(2.0, 3.0)`, `SetCollider(Owner, false)`; `On` — `WaitRandom(1.0, 2.0)`,
`SetCollider(Owner, true)`; `On` also exits on the `EVADED` broadcast from `Evade Antic`.
**2 RNG draws per cycle.** At `SceneReady` the FSM is in `Off` and the collider is disabled
(`BOSSES#colliders[8].enabled = false`).

### 2.6 Stun Control FSM

`FSM#Boss Holder/Hornet Boss 1/Stun Control`, start `Init`, globals
`STUN CONTROL STOP → Stop`, `STUN CONTROL RESET → Reset`.
Variables: float `Combo Time` 2.0; ints `Combo Counter` 0, `Decrement` 0 (declared, **never read or
written by any action in the FSM**), `Hits Total` 0, `Stun Combo` **6**, `Stun Hit Max` **10**,
`Stuns Max` **5**, `Stuns Total` 0; string `Tag` ""; GameObject `Self`.
It is bound as `HealthManager.stunControlFSM` (`BOSSES#fields.stunControlFSM`) and receives
`"STUN DAMAGE"` from `HK/HealthManager.cs:508–511` on every non-fatal hit.

| State | Transitions | Actions |
|---|---|---|
| `Init` | FINISHED→`Heavy Blow` | `GetOwner→$Self` |
| `Heavy Blow` | FINISHED→`Idle` | `PlayerDataBoolTest("equippedCharm_15") isFalse→FINISHED`; `$Stun Hit Max −= 1`; `$Stun Combo −= 1` (Heavy Blow charm) |
| `Idle` | STUN DAMAGE→`Max Check` | — |
| `Max Check` | FINISHED→`In Combo`, STUN→`Stun` | `GetTag`; `IntCompare($Hits Total, $Stun Hit Max)` `<`→FINISHED, `==`/`>`→STUN |
| `In Combo` | TIME OUT→`Reset Counter`, STUN DAMAGE→`Continue Combo`, STUN→`Stun` | `$Combo Counter += 1`; `$Hits Total += 1`; `IntCompare($Combo Counter, $Stun Combo) ==`→STUN; `Wait($Combo Time = 2.0)`→TIME OUT |
| `Continue Combo` | FINISHED→`In Combo`, STUN→`Stun` | `IntCompare($Hits Total, $Stun Hit Max)` `==`/`>`→STUN |
| `Reset Counter` | FINISHED→`Idle` | `$Combo Counter = 0` |
| `Stun` | FINISHED→`Idle` | `IntCompare($Stuns Total, $Stuns Max) ==`/`>`→`MAX` (**no transition named MAX exists** → dead); `$Stuns Total += 1`; `SendEventByName(Owner, "STUN")`; `$Combo Counter = 0`; `$Hits Total = 0` |
| `Stop` | STUN CONTROL START→`Reset Counter`, STUN DAMAGE→`Unstun Increment` | — |
| `Unstun Increment` | FINISHED→`Stop` | `$Hits Total += 1` |
| `Reset` | STUN CONTROL START→`Reset Counter` | — |

**Stun rule** (no charm): stun fires when **6 hits land inside a rolling 2 s combo window**, or when
`Hits Total` reaches **10** regardless of timing. `Hits Total` is only reset by an actual stun, and it
keeps accumulating while stunning is suppressed (`Unstun Increment`). The `Stuns Max = 5` cap is
inert because `MAX` has no transition.

`"STUN"` hits `Control`'s only global transition → `Stun Start`:
`AudioPlayRandom`; box←Idle; `ActivateGameObject($Sphere Ball,false)`;
`SpawnObjectFromGlobalPool(Stun Effect at $Self)`;
`SetFsmFloat(Owner, fsmName="recoil", "Recoil per second", 15)` (**same missing-FSM situation as
`Escalation`** — no FSM named `recoil` exists on the object; `ActionHelpers.cs:70` falls back to
`Control` and `SetFsmFloat` then finds no `"Recoil per second"` float and warns, `ACT/SetFsmFloat.cs:74–81`
→ no-op);
`SetBoxColliderTrigger false`; `SetGravity2dScale $Gravity`; `SetRotation z=0`; `SetScale y=1`;
`FaceObject`; `GetScale→$X Scale`; `FloatOperator(10.0 × $X Scale)→$Stun Air Speed Crt`;
`Tk2dPlayAnimation "Stun Air"`; **`SetVelocity2d x=$Stun Air Speed Crt y=20`**;
`ActivateGameObject` off for `$Needle`, `$Hit ADash`, `$Hit GDash`; `SetCollider($Needle Tink,false)`;
`NextFrameEvent→FINISHED`.
Then `Stun Air` (`CheckCollisionSideEnter`/`CheckCollisionSide` bottom→LAND) → `Stun Land`
(`Tk2dPlayAnimation "Stun"`; `SetVelocity2d 0,0`; **`Wait 3.0`**; also exits on `TOOK DAMAGE`) →
`Stun Recover` (empty) → `Set Jump Only` → `Jump Antic`.

---

## 3. Movement model

**Velocity-driven rigidbody, never root motion.** Every position change comes from one of:
(a) `SetVelocity2d` / `SetVelocityAsAngle` writing `rb2d.velocity`, integrated by Box2D;
(b) `DecelerateXY` / `DecelerateV2` multiplying `rb2d.velocity` in `OnFixedUpdate`;
(c) `SetPosition` / `Translate` teleports (`Land Y`, `Hit Roof`, `Wall L/R`, `GG Intro 1`);
(d) gravity via `rb2d.gravityScale`.
No `Tk2dPlayAnimation` variant moves the transform.

Rigidbody (`BOSSES#rb2d`): `bodyType` Dynamic, `mass` 1, `drag` 0, `angularDrag` 0.05,
`gravityScale` 1.5, `constraints` FreezeRotation, `simulated` true,
`collisionDetectionMode` **Continuous**, `interpolation` **None** (pinned by regime R2, STATE.md),
`sleepMode` StartAwake, `useAutoMass` false, `centerOfMass` (0.1484013, −0.9687844),
7 attached colliders.

World gravity (`PHYS#Physics2D.gravity`) = (0, −60). Effective acceleration at
`gravityScale = 1.5` is **−90 units/s²**, i.e. **−1.8 per 0.02 s fixed step**. Integration is
semi-implicit Euler (`v += a·dt` then `x += v·dt`). Verification (`TR:r2_rand1.a.hktrace@289`,
first `In Air` frame): `Jump` set `y = 41`; the sample reads `vel_y = 39.20 = 41 − 1.8` and
`pos_y = 28.5619 + 0.7840`, and `39.20 × 0.02 = 0.7840`. Exact.

`Physics2D` settings that the sim must match (`PHYS#Physics2D`): `velocityIterations` 8,
`positionIterations` 3, `queriesHitTriggers` **true**, `queriesStartInColliders` false,
`autoSyncTransforms` true, `simulationMode` FixedUpdate, `defaultContactOffset` 0.01,
`baumgarteScale` 0.2, `maxTranslationSpeed` 100, `velocityThreshold` 1.0.

Per-state velocity law:

| State | rb2d.velocity write | gravityScale |
|---|---|---|
| `Idle`, `GDash Antic`, `Throw Antic` | `x = 0` only (y untouched) | unchanged |
| `Jump Antic`, `Land`, `Evade Antic`, `Evade Land`, `GG Land`, `Sphere Antic G`, `Hit Roof`, `Wall L/R`, `Stun Land` | `x = 0, y = 0` | `Land`/`GG Land` set 1.5; `Hit Roof` sets **2** |
| `Run` | `x = −8 × scale.x` | — |
| `Evade` | `x = +22 × scale.x` | — |
| `G Dash` | `x = −25 × scale.x` | — |
| `Jump` | `x = $Jump X, y = 41` | — |
| `Jump R` / `Jump L` | `x = ±10, y = 20` | 2 |
| `Fire` | `SetVelocityAsAngle(θ = $Angle, speed = 30)` → `(30cosθ, 30sinθ)` (`ACT/SetVelocityAsAngle.cs:73–74`, degrees, CCW from +X) | 0 (set in `ADash Antic`) |
| `Stun Start` | `x = 10 × scale.x, y = 20` | 1.5 |
| `GDash Recover1/2` | `DecelerateXY x ×= 0.77 / 0.75` per fixed step, plus a `|v| < 0.001 → 0` snap (`ACT/DecelerateXY.cs:70`) | — |
| `Hard Land` | `DecelerateXY x ×= 0.8`, then `SetVelocity2d y = 0` (x is None) | 1.5 |
| `ADash Antic` | `DecelerateV2 ×0` → instant stop | 0 |
| `Sphere Antic A`, `Sphere A` | `DecelerateV2 ×0.78` per fixed step, **both axes, no deadzone** (`ACT/DecelerateV2.cs:42–82`) | 0 |
| `Sphere A End` | — | 1.5 |

Verification of the decelerate law (`TR:r2_move.a.hktrace@421→@422`): last `In Air` velocity
(16.26, −23.80); first `Sphere Antic A` sample (12.68, −19.97).
`16.26 × 0.78 = 12.6828` ✔; the y value needs one gravity step first:
`(−23.80 − 1.80) × 0.78 = −19.968` ✔ — i.e. a `FixedUpdate` gravity integration lands between the
state's `SetGravity2dScale 0` and its first `DecelerateV2` tick.

**Walls and floor.** Two independent mechanisms:
- *Grounded/airborne states* use `CheckCollisionSide` / `CheckCollisionSideEnter`. The layer they
  test is **not** taken from `otherLayerNumber`: `ACT/CheckCollisionSide.cs:131–138` is
  `if (!otherLayer) CheckTouching(8); else CheckTouching(otherLayerNumber);` and `otherLayer` is a
  plain `public bool` (`:39`), false in all 7 Hornet instances (dump) — so **`otherLayer = false` ⇒
  hardcoded layer 8 Terrain**, and the dumped `otherLayerNumber = 0` is never read. Same shape in
  `ACT/CheckCollisionSideEnter.cs:87–94`. These are **raycasts**,
  not contact normals: 3 rays per side from the collider `bounds` corners/midpoint, length **0.08**,
  mask `1 << layer` (`ACT/CheckCollisionSide.cs:165–184`). `CheckCollisionSide.OnUpdate` only
  *re-checks* while some side bool is already set (`:127–140`); first contact arrives through the
  `PlayMakerUnity2DProxy` `OnCollisionStay2D` delegate (`:82–87`, proxy dispatch at
  `HK/PlayMakerUnity2DProxy.cs:167–170`). `CheckCollisionSideEnter` subscribes `OnCollisionEnter2D`
  only and has an empty `OnUpdate`. In `G Dash` both carry `ignoreTriggers = true`.
- *A Dash* ignores physics entirely and uses the four scalar bounds
  `Floor Y 27.55 / Roof Y 40.54 / Wall X Left 15.13 / Wall X Right 37.90` via `FloatCompare everyFrame`
  on `transform.position`, then teleports onto the bound.

Hornet's body is layer 11 `Enemies`, which collides with 8 Terrain and 25 Soft Terrain but **not**
with 9 Player (`PHYS#layerCollisionMatrix.ignoreLayerCollision[11]`) — she passes through the knight.
Her `Enemy Attack` children (layer 22) collide with 20 `Hero Box` and 8 Terrain.

`ConstrainPosition` (`HK/ConstrainPosition.cs:17–51`) clamps `transform.position` in `Update` to
`[xMin,xMax] × [yMin,yMax]` when `constrainX`/`constrainY`, writing the transform back only if a
clamp fired (`:47–50`). Live values (`SCENE#Boss Holder/Hornet Boss 1`, `components[]`
`ConstrainPosition`): **`constrainX = true, xMin = 15.07, xMax = 37.96`; `constrainY = false`**
(`yMin = 0`, `yMax = 99` unused). This is the hard arena clamp and it is **tighter than the A-Dash
scalar bounds** (`Wall X Left 15.13`, `Wall X Right 37.90`) by 0.06 on each side, so in normal play
the FSM's `WALL L`/`WALL R` tests trip first and the clamp only catches overshoot within a single
step. The sim must run it every `Update`, after the FSM and after the physics write-back.

**Facing.** `transform.localScale.x = ±|scale.x|` written by `FaceObject`
(`ACT/FaceObject.cs:76–133`: target to the right ⇒ `+xScale` if `spriteFacesRight` else `−xScale`;
ties `ax == bx` fall to the "left" branch) and flipped by `FlipScale`
(`ACT/FlipScale.cs:67`, `localScale.x = −localScale.x`). `X Scale` is read back with
`GetScale space=World` (⇒ `transform.lossyScale`, `ACT/GetScale.cs:58`) and multiplied into every
horizontal speed, so `Run Speed = −8` with `scale.x = −1` gives `+8`. Verified: `TR:r2_rand1@114`
`sx = −1, vel_x = +8.00`; `TR:r2_move@114` `sx = +1, vel_x = −8.00`.

**Arena bounds used by the FSM** (all from `FSM#…/Control` variables): jump target sampled uniformly
in `[Left X 16.06, Right X 36.53]`; A-Dash clamps at `x ∈ [15.13, 37.90]`, `y ∈ [27.55, 40.54]`;
throw allowed only outside `[22.51, 30.16]` and pointing inward; air sphere requires `y < 33.80`.
Observed ground level in all three traces is `y = 28.5619` (Hornet transform), `Floor Y` = 27.55 is
the A-Dash snap target (a lower value — `Land Y` snaps the transform to 27.55 and the physics then
resolves her up onto the floor: `TR:r2_rand1@354` `pos_y = 27.5500` → `@355` `28.2958` → settles at
`28.5619`).

---

## 4. Timing

`dt = 0.02 s` under regime R2 (`Time.captureDeltaTime = Time.fixedDeltaTime = 0.02`,
`PHYS#Time`, and every FRAME record in the R2 traces has `dt = 0.02000` with `time` advancing
exactly 0.02 per record). **One FRAME record = one 0.02 s game step**; `Time.frameCount` gaps in the
traces are `timeScale = 0` pause frames and carry no game time.

### 4.1 Frame-phase model (derived from the decomp, confirmed by the traces)

- PlayMaker actions' `OnUpdate` runs in Unity `Update` (`PlayMakerFSM.cs:369–375`); `OnFixedUpdate`
  only via the `PlayMakerFixedUpdate` proxy (`PlayMakerFixedUpdate.cs:6–16`, added at
  `PlayMakerFSM.cs:286–293`).
- tk2d advances clips in **LateUpdate** (`HK/tk2dSpriteAnimator.cs:586–589`), and fires
  `AnimationCompleted` from there (`:512–514`, `:577–584`).
- The oracle's FRAME capture is the mod coroutine's `yield return null` resume, which Unity schedules
  **after every `MonoBehaviour.Update` and before `LateUpdate`** (analysis/specs/frame-order.md §3, lines 188–193).
  So it observes post-`Update`, pre-`LateUpdate` state.

⇒ an **animation-complete transition takes effect after that frame's capture**, a **`Wait`-driven
transition before it**. Both are visible in the trace: `Evade Antic → Evade` is stamped F25448 while
the F25448 capture still reads `Evade Antic`; `Evade → Evade Land` is stamped F25467 and the F25467
capture already reads `Evade Land` (`TR:r2_rand2.a.hktrace`).

### 4.2 Expected record counts

Let `D = frameCount / fps` (clip duration) and `dt = 0.02`.

- **Animation-driven state** (`Tk2dPlayAnimationWithEvents … animationCompleteEvent`):
  `N = floor(D/dt) + 1` records. (The clip samples `clipTime = 0, dt, 2dt, …`; completion needs
  `clipTime` to pass `D`, and float accumulation of `0.02f` makes exact multiples undershoot.)
- **`Wait`-driven state entered from an animation-complete** (the common case):
  `N = k − 1` where `k = min{ k : k·0.02f ≥ T }` in float32. For `T` an exact multiple of `0.02`,
  float undershoot gives `k = T/0.02 + 1`, hence `N = T/0.02`.
- **Physics-driven states** (`In Air`, `A Dash`, `GG Fall`, `Stun Air`, `Thrown`) have no closed form.

**These two closed forms are a measurement convention for checking a sim against the R2 traces, not
the thing to port.** The `−1` on `Wait` states is an artefact of the entering path (a state entered
from an `Update`-phase event gets its first `OnUpdate` tick one frame earlier than one entered from a
`LateUpdate` animation-complete). Port the mechanism instead: a float32 accumulator
`timer += Time.deltaTime` compared with `>=` (`ACT/Wait.cs:49–53`), and tk2d's own float32 clip clock
(analysis/specs/tk2d-animator.md §1.6, §3.3). The two agree: the first `k` with `Σ_{i≤k} 0.02f ≥ T` is
13 (T=0.25), 18 (0.35), 51 (1.0), 151 (3.0), matching every observed length in §4.4.

### 4.3 Clip table (`BOSSES#animator.library`, `Hornet Boss Anim`, 61 clips)

Clips referenced by `FSM#…/Control` only:

| Clip | fps | frames | wrap | D (s) | D/dt | predicted N |
|---|---|---|---|---|---|---|
| `Idle` | 12 | 6 | Loop | 0.5000 | 25.00 | — (loops) |
| `Run` | 12 | 8 | Loop | 0.6667 | 33.33 | — (loops) |
| `Fall` | 12 | 4 | Loop (loopStart 4) | 0.3333 | 16.67 | — |
| `Jump` | 12 | 9 | LoopSection (5) | 0.7500 | 37.50 | — |
| `Evade` | 12 | 3 | Loop | 0.2500 | 12.50 | — |
| `A Dash` | 20 | 2 | Loop | 0.1000 | 5.00 | — |
| `G Dash` | 20 | 2 | LoopSection (0) | 0.1000 | 5.00 | — |
| `Sphere Attack` | 15 | 9 | LoopSection (5) | 0.6000 | 30.00 | — |
| `Throw` | 12 | 6 | LoopSection (3) | 0.5000 | 25.00 | — |
| `Stun` | 12 | 6 | LoopSection (2) | 0.5000 | 25.00 | — |
| `Jump Antic` | 12 | 4 | Once | 0.3333 | 16.67 | **17** |
| `Land` | 12 | 3 | Once | 0.2500 | 12.50 | **13** |
| `Evade Antic` | 12 | 3 | Once | 0.2500 | 12.50 | **13** |
| `Hard Land` | 12 | 6 | Once | 0.5000 | 25.00 | **26** |
| `Flourish` | 12 | 14 | Once | 1.1667 | 58.33 | **59** |
| `Wall Impact` | 12 | 4 | Once | 0.3333 | 16.67 | **17** |
| `G Dash Antic` | 18 | 10 | Once | 0.5556 | 27.78 | **28** |
| `G Dash Recover1` | 18 | 4 | Once | 0.2222 | 11.11 | **12** |
| `G Dash Recover2` | 18 | 2 | Once | 0.1111 | 5.56 | **6** |
| `A Dash Antic` | 20 | 9 | Once | 0.4500 | 22.50 | **23** |
| `Throw Antic` | 18 | 14 | Once | 0.7778 | 38.89 | **39** |
| `Throw Recover` | 18 | 6 | Once | 0.3333 | 16.67 | **17** |
| `Sphere Antic G` | 12 | 10 | Once | 0.8333 | 41.67 | **42** |
| `Sphere Antic A` | 12 | 7 | Once | 0.5833 | 29.17 | **30** |
| `Sphere Recover G` | 12 | 2 | Once | 0.1667 | 8.33 | **9** |
| `Sphere Recover A` | 12 | 3 | Once | 0.2500 | 12.50 | **13** |
| `Stun Air` | 18 | 6 | Once | 0.3333 | 16.67 | — (exits on LAND) |

No frame in any Hornet clip has `triggerEvent = true` (checked across all 61 clips), so
`animationTriggerEvent` never fires and `Tk2dWatchAnimationEvents`' double-fire hazard is moot here.

Clips present in the library but **not referenced by `FSM#…/Control`**: `A Dash Recover`,
`G Dash Recover`, `Needle`, `Needle Thread`, `Wounded`, `Death Air`, `*Q` (quick-variant) clips 38–43,
`Counter *` 44–50, `Barb *` 51–58, `Hornet Flash`, `Sphere Ball`, and the four effect clips
(`Air Dash Effect`, `G Dash Effect`, `Flash`, `Throw Effect`) which belong to the child effect objects.

### 4.4 Attack timing summary + trace verification

Predicted from §4.2/§4.3 and the FSM `Wait` values; observed = record counts in the R2 traces.

| Attack | State | Driver | Predicted N | Observed N | Trace |
|---|---|---|---|---|---|
| **Throw** | `Throw Antic` | anim 0.7778 | 39 | **39** ✔ | `r2_rand1@139–177` |
| | `Lock?`+`Lock UL`+`Throw` | instantaneous / `NextFrameEvent` | 0–1 each | 0 visible ✔ | F24970–24972 |
| | `Thrown` | needle flight (`Out 0.3` + `Decel` + `Return` iTween) | — | 60 | `@178–237` |
| | `Throw Recover` | anim 0.3333 | 17 | **17** ✔ | `@238–254` |
| **G Dash** | `GDash Antic` | anim 0.5556 | 28 | **28** ✔ | `r2_move@279–306`, `r2_rand2@357–384` |
| | `G Dash` | `Wait 0.35` → N = 17, or wall | 17 | **17** (`r2_rand2@385–401`, timer) / **14** (`r2_move@307–320`, wall hit at x ≈ 16.2) ✔ | |
| | `GDash Recover1` | anim 0.2222 | 12 | **12** ✔ | both |
| | `GDash Recover2` | anim 0.1111 | 6 | **6** ✔ | both |
| **Sphere A** | `Sphere Antic A` | anim 0.5833 | 30 | **30** ✔ | `r2_move@422–451`, `r2_rand2@206–235` |
| | `Sphere A` | `Wait 1.0` → N = 50 | 50 | **50** ✔ | both |
| | `Sphere Recover A` | anim 0.25 | 13 | **13** ✔ | both |
| **Evade** | `Evade Antic` | anim 0.25 | 13 | **13** ✔ | `r2_rand2@420–432` |
| | `Evade` | `Wait 0.25` → N = 12 | 12 | **12** ✔ | `@433–444` |
| | `Evade Land` | anim 0.25 (`Land`) | 13 | **13** ✔ | `@445–457` |
| **A Dash** | `Jump Antic` | anim 0.3333 | 17 | **17** ✔ | `r2_rand1@272–288` |
| | `In Air` | physics + `Wait($Air Dash Pause)` | — | 10 | `@289–298` |
| | `ADash Antic` | anim 0.45 | 23 | **23** ✔ | `@299–321` |
| | `A Dash` | until a bound is crossed | — | 32 | `@322–353` |
| | `Hard Land` | anim 0.5 | 26 | **26** ✔ | `@354–379` |
| **shared** | `Land` | anim 0.25 | 13 | **13** ✔ | `r2_move@222–234`, `@528–540` |
| | `Run Antic` (plays `Evade Antic`) | anim 0.25 | 13 | **13** ✔ | all three |
| | `Flourish` | anim 1.1667 | 59 | **59** ✔ | all three |
| | `GG Land` | anim 0.25 (`Land`) | 13 | **13** ✔ | all three |

**Mismatches: none.** 20/20 predicted state lengths match the traces exactly. The two rules in §4.2
(the `+1` on animation-driven states, the `−1` on `Wait` states entered from an animation-complete)
are *required* to get this — a naive `round(D/dt)` misses `Hard Land` (25 vs 26), `Evade` (13 vs 12)
and `G Dash` (18 vs 17).

`Run`/`Idle` are `WaitRandom`-driven and therefore not predictable without reproducing the RNG stream;
observed values are consistent with the escalated ranges (`Run` 47/31/39/19/9 records = 0.94/0.62/
0.78/0.38/0.18 s vs `[0.35, 0.75]` post-escalation and `[0.5, 1.0]` pre-escalation; `Idle` 17 records
= 0.34 s vs `[0.1, 0.4]` post-escalation).

---

## 5. Damage and health

**Hornet's HP.** `BOSSES#hp` = 900, `BOSSES#fields.hp` = 900,
`hpScale` = `{level1: 0, level2: 1250, level3: 1250}`. `HealthManager.Start` applies
`hp = hpScale.GetScaledHP(hp)` (`HK/HealthManager.cs:313`); `GetScaledHP` returns the original HP when
the selected level's entry is `<= 0` (`HK/HealthManager.cs:20–45`). Attuned (`BossLevel = 0`) ⇒ 900;
Ascended/Radiant ⇒ 1250. `enemyType` = 1, `invulnerableTime` = 0, `damageOverride` = false,
`ignoreAcid`/`ignoreHazards`/`ignoreWater` false, `hasSpecialDeath` false.

**Taking damage** (`HK/HealthManager.cs:332–347`, `:430–517`):
- `Hit()` is a no-op while `isDead`, while `evasionByHitRemaining > 0`, or when `DamageDealt <= 0`.
- `evasionByHitRemaining` counts down by `Time.deltaTime` in `Update` (`:329`) and is set to
  **0.20 s** after every non-fatal hit (`NonFatalHit`, `:534`) and **0.15 s** after a blocked hit
  (`:427`). This is Hornet's i-frame window; `hasAlternateHitAnimation` is false so the 0.2 s branch
  always applies.
- `IsBlockingByDirection` returns false when `invincible` is false (`:711`); with `invincible` true and
  `invincibleFromDirection = 0` all directions block (`:715`).
- On damage: `HIT` / `HIT LANDED` / `TOOK DAMAGE` events are broadcast (`:441–443`) — `TOOK DAMAGE` is
  what drives `Control`'s `Idle`/`Run`/`Stun Land` transitions; `recoil.RecoilByDirection(dir, magnitude)`
  (`:450`); `hp = max(hp − round(DamageDealt × Multiplier), −50)` (`:499–504`); if `hp > 0`,
  `NonFatalHit` + `stunControlFSM.SendEvent("STUN DAMAGE")` (`:508–511`), else `Die`.

**`is_invincible` as the trainer sees it.** The trace's `invincible` column is
`HealthManager.IsInvincible` (`docs/trace-format.md` ENTITY block), i.e. the `invincible` **field**
(`HK/HealthManager.cs:226–235`) — **not** `evasionByHitRemaining`. It is `true` only during the
intro: `r2_rand1.a` 42 True / 438 False, `r2_rand2.a` 42/438, `r2_move.a` 42/558, in every case
flipping to `false` at FRAME-record index 42, the first `Flourish` record (§2.2). So for the whole
fight the observation column reads `false`, and the 0.2 s post-hit window is **not** reflected in it.

**Invincibility windows in the FSM.** `SetInvincible` is used exactly twice, and both writes are
literals: `GG Intro 1` sets `IsInvincible = true`, `GG Reset` sets `IsInvincible = false`
(`HK/SetInvincible.cs:29–32`; see §2.2 for the dump fields and the trace split). Neither `Evade` nor
any other combat state touches invincibility — **the evade is a movement dodge, not an i-frame
window** — and Hornet 1 has no `Counter` state at all (§2.4). The only damage gate during the fight
is `evasionByHitRemaining`.

**Recoil.** Live field values on this instance (`SCENE#Boss Holder/Hornet Boss 1`, `components[]`
`Recoil`), which differ from the `Reset()` editor defaults at `HK/Recoil.cs:77–84` and must be used
in preference to them:

| field | live value | `Reset()` default |
|---|---|---|
| `recoilSpeedBase` | 15.0 | 15 (`:82`) |
| `recoilDuration` | **0.15** | 0.5 (`:81`) |
| `freezeInPlace` | false | false (`:79`) |
| `stopVelocityXWhenRecoilingUp` | **false** | true (`:80`) |
| `preventRecoilUp` | **true** | false (`:83`) |
| `skipFreezingByController` | false | — |

`RecoilByDirection` only acts when `state == Ready` (`:114`); with `preventRecoilUp = true` an
up-direction hit (`attackDirection == 1`) does nothing at all (`:122`). Otherwise it sets
`recoilSpeed = recoilSpeedBase × attackMagnitude` (`:129`), builds a `Sweep(bodyCollider, dir, 3)`
(`:130`), sets `recoilTimeRemaining = recoilDuration` (`:132`) and fires
`RECOIL HORIZONTAL` / `HIT LEFT|RIGHT|UP|DOWN` (`:133–149`). `FSM#…/Control/Throw` sets
`SetRecoilSpeed(Owner, 0)` and `Throw Recover` restores `15` (`HK/SetRecoilSpeed.cs:18–29`) — Hornet
does not get pushed back while the needle is out. (`FSM#…/Control/Stun Start`'s
`SetFsmFloat(fsmName="recoil", "Recoil per second", 15)` is a no-op, §2.6.)

**Damage to the knight.** `DamageHero.damageDealt` / `hazardType` → `HeroBox` →
`HeroController.TakeDamage` (`HK/HeroBox.cs:61, :91`). Live values from `scene.json` (§1.1): every
Hornet hitbox deals **1** with `hazardType 1`; the body's `damageDealt` is driven by the FSM (0
during `GG Intro 1`…`GG Reset`, 1 thereafter). Trace corroboration: body, `Hit ADash`, `Sphere Ball`
and `Needle` all reported `amount:1, hazard_type:1`; `Hit GDash` unobserved in the R2 corpus.

Attack-hitbox activation windows (from the FSM, definitional):

| Hitbox | activated in | deactivated in |
|---|---|---|
| `Hit GDash` | `G Dash` | `GDash Recover1`, `Stun Start` |
| `Hit ADash` | `Fire` | `Land Y`, `Hit Roof`, `Wall L`, `Wall R`, `Stun Start` |
| `Sphere Ball` | `Sphere`, `Sphere A` | `Sphere Recover`, `Sphere Recover A`, `Stun Start` |
| `Needle` | `Throw` | `Throw Recover`, `Stun Start` |
| body `BoxCollider2D` | always (`SetBoxColliderTrigger true` only during `Fire`…`Land Y`/`Wall`/`Roof`) | — |

`Sphere Ball` additionally runs `FSM#…/Sphere Ball/Grow`: a single state, `SetScale (0.8, 0.8)` then
`iTweenScaleTo (1.5, 1.5, 1) time 0.25 easeOutSine` — **the sphere's hitbox radius grows over
0.25 s** (collider radius 2.53 at scale 1 ⇒ 2.024 → 3.795).

---

## 6. P4 dependency list (scope statement)

### 6.1 FsmStateAction types Hornet needs (44 distinct, across `Control`, `Stun Control`, the 7 detector FSMs, `Sphere Ball/Grow`, `Needle/Control`, `Needle Tink/Setup and Follow`, and the 4 effect FSMs)

*Control-flow / RNG* — `SendRandomEvent`, `SendRandomEventV2`, `SendRandomEventV3`, `RandomFloat`,
`Wait`, `WaitRandom`, `NextFrameEvent`, `SendEvent`, `SendEventByName`.

*Motion / physics* — `SetVelocity2d`, `SetVelocityAsAngle`, `DecelerateXY`, `DecelerateV2`,
`SetGravity2dScale`, `SetIsKinematic2d`, `GetVelocity2d`, `GetSpeed2d`, `CheckCollisionSide`,
`CheckCollisionSideEnter`, `Trigger2dEvent`.

*Transform* — `SetPosition`, `GetPosition`, `SetScale`, `GetScale`, `SetRotation`, `GetRotation`,
`Translate`, `FaceObject`, `FlipScale`, `FaceAngle`, `FaceAngleV2`, `GetAngleToTarget2D`,
`CheckTargetDirection`, `Vector3AddXYZ`.

*Colliders / objects* — `SetBoxCollider2DSizeVector`, `SetBoxColliderTrigger`, `SetCollider`,
`ActivateGameObject`, `ActivateAllChildren`, `SetMeshRenderer`, `SpawnObjectFromGlobalPool`,
`FindChild`, `FindGameObject`, `GetOwner`, `GetParent`, `SetParent`, `DestroySelf`.

*Animation* — `Tk2dPlayAnimation`, `Tk2dPlayAnimationWithEvents`, `Tk2dPlayFrame`,
`Tk2dWatchAnimationEvents`.

*Logic / math* — `BoolTest`, `BoolTestMulti`, `FloatCompare`, `FloatInRange`, `FloatTestToBool`,
`FloatOperator`, `FloatSubtract`, `FloatMultiply`, `IntCompare`, `IntAdd`, `IntAddV2`, `IntOperator`,
`SetFloatValue`, `SetBoolValue`, `SetIntValue`, `SetGameObject`.

*Cross-FSM / playerdata* — `SetFsmBool`, `SetFsmFloat`, `SetFsmString`, `GetFsmInt`,
`SetFsmGameObject`, `GetFsmGameObject`, `GetPlayerDataInt`, `PlayerDataBoolTest`.

*HK-custom* — `SetRecoilSpeed`, `SetInvincible`, `SetDamageHeroAmount`, `GGCheckIfBossScene`, `GetTag`.

*Cosmetic (must exist as no-ops, but they consume RNG — see 6.4)* — `AudioPlaySimple`,
`AudioPlayerOneShot`, `AudioPlayerOneShotSingle`, `AudioPlayRandom`, `AudioPlayInState`, `AudioStop`,
`TransitionToAudioSnapshot`, `ApplyMusicCue`, `PlayParticleEmitter`, `SpawnRandomObjectsV2` (death),
`DestroyObject` (death), `iTweenMoveTo`, `iTweenScaleTo`.

*Required by the CameraShake FSM Hornet drives (§6.4.1b), even though nothing it does is visible to
the agent* — `ShakePositionV2` (RNG), `GotoPreviousState`, `FloatAdd`, `FloatCompare`,
`SetFloatValue`, `BoolTest`, `SetPosition`, `PlayVibration` (no-op, no RNG).

Actions present but **disabled** (`enabled:false`) that the sim must *not* execute:
`FindChild` in `Init`, `BoolTest` in `Aim Jump`, `SetBoxCollider2DSizeVector` in `ADash Antic`,
`FireAtTarget` in `Fire`, `SendEventByName` in `Throw`, `SendEvent` in `Move Choice A`/`B`,
`TransitionToAudioSnapshot`+`ApplyMusicCue` in `GG Music`.

### 6.2 PlayMaker engine semantics

- `Fsm.Update` = Unity `Update` (`PlayMakerFSM.cs:369–375`); `OnFixedUpdate` requires the
  `PlayMakerFixedUpdate` proxy, opted in via `Fsm.HandleFixedUpdate` set in an action's
  `Awake`/`OnPreprocess`, which sets `preprocessed = false` (`PM/Fsm.cs:1127`) so
  `PlayMakerFSM.Init` re-runs `AddEventHandlerComponents` (`PlayMakerFSM.cs:176–181, 286–293`).
- State entry runs every action's `OnEnter` in array order and **aborts the remainder as soon as
  a state switch is queued** (`PM/FsmState.cs:286–317`, esp. `:307–310`).
- `OnUpdate` iterates `ActiveActions` (actions that did **not** `Finish()` in `OnEnter`) and does
  **not** break on a pending switch (`PM/FsmState.cs:342–355`).
- `FINISHED` is emitted only from `CheckAllActionsFinished` when `ActiveActions` empties
  (`PM/FsmState.cs:609–620`).
- Events are synchronous and recursive; a self-sent event only sets `switchToState`, drained by
  `UpdateStateChanges()` at the end of the tick hook (`PM/Fsm.cs:2023–2080, 2314–2345, 2406–2418`).
  Foreign-FSM events switch immediately (`:2340–2343`) — this is how the detector FSMs' `SetFsmBool`
  and `HealthManager`'s `SendEvent` interleave with `Control`.
- `MaxLoopCount` 1000, trip disables the component (`PM/Fsm.cs:595–605, 2385–2390`).
- **`FsmVar.IsNone`** semantics (see the top of this file) — required for correct
  `SetVelocity2d` / `DecelerateXY` / `SetPosition` / `SetRotation` behaviour.
- **`RestartOnEnable`** (`PM/Fsm.cs:1839–1855`): on every `OnEnable` the FSM resets to `startState`
  and re-runs `Start()`. True on all 18 FSMs in Hornet's reachable set — required for the needle
  (§1.2) and for every `ActivateGameObject`-toggled effect child.

### 6.3 HK components

`HealthManager` (hp, `evasionByHitRemaining` i-frames, `IsBlockingByDirection`, `TOOK DAMAGE`/`HIT`
broadcast, `stunControlFSM.SendEvent("STUN DAMAGE")`, `hpScale`),
`DamageHero` (`damageDealt`, `hazardType`) + `HeroBox` (`HeroController.TakeDamage`),
`Recoil` (`SetRecoilSpeed`, `RecoilByDirection`, `Sweep`),
`ConstrainPosition` (`Update` clamp),
`PlayMakerUnity2DProxy` (collision/trigger delegate fan-out, `HK/PlayMakerUnity2DProxy.cs:150–170`),
`FSMUtility.SendEventToGameObject`, `ActionHelpers.GetGameObjectFsm` **including its
missing-FSM fallback** (`ActionHelpers.cs:70` — required to reproduce the dead `Escalation` HP gate),
`ObjectPool.Spawn` (only for the cosmetic `Stun Effect`).
Not needed for behaviour: `SpriteFlash`, `SetZ`, `EnemyHitEffectsUninfected`,
`EnemyDeathEffectsUninfected`, `EnemyDreamnailReaction`, `ExtraDamageable`,
`DeactivateIfPlayerdataTrue`, `NonBouncer`, `SetParticleScale`.

### 6.4 Unity features

- **Rigidbody2D + Box2D**: dynamic body, `gravityScale`, `velocity`, `isKinematic`, FreezeRotation,
  Continuous collision, `interpolation = None`. Fixed step 0.02, gravity (0, −60),
  8 velocity / 3 position iterations.
- **Collider2D**: `BoxCollider2D` (with runtime `size`/`offset`/`isTrigger`/`enabled` mutation),
  `CircleCollider2D`, `PolygonCollider2D` (needed for `Hit ADash`, `Hit GDash`, `A Dash Range`).
- **Physics2D.Raycast** with a single-layer mask and `isTrigger` filtering — required by
  `CheckCollisionSide`/`Enter` (3 rays × 4 sides, length 0.08).
- **Trigger callbacks** `OnTriggerEnter2D` / `Stay2D` / `Exit2D` and **collision callbacks**
  `OnCollisionEnter2D` / `Stay2D`, routed through `PlayMakerUnity2DProxy`.
- **Layer collision matrix** (`PHYS#layerCollisionMatrix`): 8 Terrain, 9 Player, 11 Enemies,
  13 Hero Detector, 14 Terrain Detector, 20 Hero Box, 22 Enemy Attack, 25 Soft Terrain.
- **tk2dSpriteAnimator**: `Play(clipName)`, per-clip `fps`/`frameCount`/`wrapMode`/`loopStart`,
  `clipTime` advanced in **LateUpdate** by `Time.deltaTime`, `AnimationCompleted` delegate for
  `WrapMode.Once`, `Playing` flag. Frame events are unused by Hornet (§4.3).
- **Transform**: `localScale`, `position` vs `localPosition`, `lossyScale` (read by `GetScale
  space=World`), `eulerAngles`, and reparenting via `transform.parent` (`ACT/SetParent.cs:35`).
  Whether that assignment preserves the world pose is **UNKNOWN from `analysis/`** — see
  **Q-hornet-3**; the Needle and Needle Tink de-parent operations depend on it.
- **`UnityEngine.Random` global stream** — see §6.4.1.
- **Object pooling** for `Stun Effect` only (cosmetic).
- **iTween** for `Needle/Control/Return` (`easeInSine`, speed 30) and `Sphere Ball/Grow`
  (`easeOutSine`, 0.25 s scale tween). The sphere tween changes a **live hitbox radius**, so it is
  behaviourally in scope.

### 6.4.1 RNG draw census (P4 seeded-branch gate)

One global `UnityEngine.Random` stream feeds everything. `analysis/dumps/GG_Hornet_1/rng_probe.json`
characterises it: 282 probed ops, **281 advance the 4-word state by exactly one step**; the only
non-advancing op is the degenerate *integer* `Range(3,3)` (`RNGP#2`, `before == after`). A degenerate
*float* range still advances — `Range(1f,1f)` at `RNGP#1` steps the state — so
`FSM#…/Control/Jump`'s `RandomFloat(41, 41)` **does** consume a draw.

**(a) Direct FSM draws** — reachable per Hornet decision, in execution order:

| site | draws | cite |
|---|---|---|
| `WaitRandom` | 1 | `ACT/WaitRandom.cs:34` |
| `RandomFloat` | 1 | `ACT/RandomFloat.cs:28` |
| `SendRandomEvent` | 1 | `ACT/SendRandomEvent.cs:28` |
| `SendRandomEventV2` | ≥1 (rejection loop) | `ACT/SendRandomEventV2.cs:31` |
| `SendRandomEventV3` | ≥1 (rejection loop) | `ACT/SendRandomEventV3.cs:41` |
| `AudioPlayerOneShot` | 2 (weighted index + pitch) | `ACT/AudioPlayerOneShot.cs:85, :91` |
| `AudioPlayRandom` | 2 | `ACT/AudioPlayRandom.cs:47, :53` |
| `AudioPlayerOneShotSingle` | 1 (pitch) | `ACT/AudioPlayerOneShotSingle.cs:81` |

Audio one-shots fire in `GDash Antic`, `Jump`, `ADash Antic`, `Sphere Antic G`, `Sphere Antic A`,
`Throw Antic`, `Evade`, `Flourish`, `GG Fall`, `Stun Start`.

**(b) Camera-shake draws — the dominant term, and off Hornet's own object.**
`FSM#…/Control/{G Dash, A Dash, Sphere, Sphere A}` each run a `SendEventByName` with
`eventTarget.target = GameObject`, `gameObject = $CameraParent`, `sendEvent = "EnemyKillShake"`.
(The same action in `FSM#…/Control/Throw` is `enabled:false`, and
`FSM#…/Corpse Hornet GG(Clone)/Control/Blow` sends one on death.) The receiver is
`FSM#_GameCameras/CameraParent/CameraShake` (24 states), whose global transition
`EnemyKillShake → To Kill Shake` gates on priority and then enters `ShakingKill`. Of the rows below,
**Hornet triggers only `ShakingKill`**; `ShakingAverage` is the knight-hit shake (c) and the rest are
listed because the same FSM instance and the same `$Priority` variable arbitrate between them:

| CameraShake state | Priority | `ShakePositionV2` Extents | Duration | UpdateShaking calls | **draws** |
|---|---|---|---|---|---|
| `ShakingKill` (`EnemyKillShake`) | 6 | `$EnemyKillShake` (0.105, 0.105, 0) | 0.5 | 25 | **75** |
| `ShakingAverage` (`AverageShake`) | 7 | `$AverageShake` (0.15, 0.15, 0) | 1.0 | 51 | **153** |
| `ShakingSmall` (`SmallShake`) | 3 | `$SmallShake` (0.08, 0.08, 0) | 0.5 | 25 | 75 |
| `ShakingBig` (`BigShake`) | 10 | `$BigShake` (0.5, 0.5, 0) | 1.0 | 51 | 153 |
| `Rumbling*` | 0 | various | looping | until the bool clears | 3/frame |

`ShakePositionV2` draws `Random.Range(-1f, 1f)` **three times per `UpdateShaking`**
(`ACT/ShakePositionV2.cs:96`, one per Vector3 component), and `UpdateShaking` runs once in `OnEnter`
(`:62`) and once per `OnUpdate` (`:68`). The `FpsLimit` throttle at `:82–89` is disabled because the
FSM passes `$FPS Limit` = **0** (dump), so `FpsLimit.Value > 0f` is false and every tick shakes.
`timer += Time.deltaTime` at `:81` happens **before** the draws, and the stop test
`timer > Duration` at `:98` happens **after** them, so the terminating call still draws. Call count =
the first `k` with `Σ_{i≤k} 0.02f > D` in float32: **k = 25 for D = 0.5** (partial sum 0.50000006) and
**k = 51 for D = 1.0** (partial sum 1.0199996). Hence 75 and 153 draws.

Two gates keep this from being a flat per-attack cost:
- **Priority.** `To Kill Shake` is `FloatCompare($Priority, 6) lessThan → FINISHED` else
  `GotoPreviousState` — a second `EnemyKillShake` arriving while `ShakingKill` (Priority 6) or
  `ShakingAverage` (7) or `ShakingBig` (10) is running is **dropped, drawing nothing**. `Normal` sets
  Priority 0. Shakes do not stack or restart.
- **Frame gating.** These counts assume `CameraShake`'s `Update` runs once per unfrozen R2 frame with
  `Time.deltaTime = 0.02`. Whether PlayMaker ticks on the frozen frame of each agent step, and what
  `Time.deltaTime` is during a `GameManager.FreezeMoment` hit-stop, is **Q-hornet-1**.

**(c) Hits on the knight also draw.** `HK/HealthManager.cs:371–372` — the clink path (`Invincible`,
i.e. any nail hit while `IsInvincible` or blocked by direction) calls `GameManager.FreezeMoment(1)`
and then `GameCameras.instance.cameraShakeFSM.SendEvent("EnemyKillShake")`: another 75 draws plus a
hit-stop. Every hit **to** the knight runs `FSM#Knight/Effects/Damage Effect/Knight Damage/Gen`,
which sends `AverageShake` to `$CameraParent`'s `CameraShake` FSM (**153 draws**) and additionally
fires four `FlingObjectsFromGlobalPool` (spawnMin 2 / spawnMax 3, speed 20–35, angle 140–220 and
−40–40) whose own draw counts belong to analysis/specs/damage-path.md, not here.

**Consequence.** A single G Dash costs 75 shake draws against ~2–5 decision draws; the shake stream
dominates. **The P4 seeded-branch gate cannot pass on the FSM draws alone — the sim must tick
`_GameCameras/CameraParent/CameraShake` (or reproduce its draw count exactly).** Knight-side shake
sources not enumerated here (Nail Arts, Spell Control, Superdash, Dream Nail, Thorn Counter, Blocker
Shield, Hero Death — all found sending shake events in the FSM dump) are damage-path/hero scope.

### 6.5 Not in scope for Hornet

No `SpawnObjectFromGlobalPool`-based projectiles in combat (the needle is a persistent scene object),
no `Walker`, no nav/pathing, no `tk2dSpriteAnimator` frame events, no `Counter`/parry, no
multi-phase HP branching beyond the (dead) `Escalation` gate.

---

## 7. Open questions

Closed since the first draft, with the evidence that closed them (kept here so the consolidation
script does not re-open them): the `damageDealt`/`hazardType` of all five Hornet hitboxes, the
`Needle`/`Needle Tink` colliders, `ConstrainPosition`'s bounds and `Recoil`'s live fields — all now
in `analysis/dumps/GG_Hornet_1/scene.json` (§1.1, §1.2, §3, §5). The `UnityEngine.Random` step model
— `analysis/dumps/GG_Hornet_1/rng_probe.json` (§6.4.1). Whether `GG Reset` clears
`HealthManager.IsInvincible` — it does; literal `false` in the dump, `HK/SetInvincible.cs:29–32`,
trace 42/438 (§2.2). The `Stun Land` `Wait 3.0` length — 150 records (first `k` with
`Σ 0.02f ≥ 3.0` is 151; §4.2). tk2d `Loop`/`LoopSection` semantics — analysis/specs/tk2d-animator.md §1.3.

### Q-hornet-1 — Does the CameraShake FSM tick on frozen frames and during `FreezeMoment`?

§6.4.1b's draw counts (75 per `EnemyKillShake`, 153 per `AverageShake`) assume
`FSM#_GameCameras/CameraParent/CameraShake` gets exactly one `Update` per unfrozen R2 frame with
`Time.deltaTime = 0.02f`, so that `ShakePositionV2`'s `timer` (`ACT/ShakePositionV2.cs:81`) reaches
`> Duration` on call 25 (D = 0.5) or 51 (D = 1.0). Two things could change the count and hence the
whole RNG stream: (a) whether PlayMaker updates at all on the one `timeScale = 0` frame per agent
step (analysis/specs/frame-order.md §3.2 and open-questions Q18 bear on this, but not for this FSM specifically);
and (b) `HK/HealthManager.cs:371` calls `GameManager.FreezeMoment(1)` immediately before sending the
shake on the clink path, and neither the freeze duration nor its effect on `Time.deltaTime` during
the shake has been read. Evidence needed: a trace containing a knight hit on Hornet with the FRAME
`rng[4]` column differenced across the shake window — the recorder already stores
`UnityEngine.Random.state` every frame, so the draw count is directly measurable.

### Q-hornet-2 — Which `PlayMakerFSM` does `GetComponent` return on `Hornet Boss 1`?

`FSM#…/Control/Escalation` and `FSM#…/Control/Stun Start` both name FSMs that do not exist on the
object (`health_manager_enemy`, `recoil`), so `ActionHelpers.GetGameObjectFsm` warns and falls back
to `go.GetComponent<PlayMakerFSM>()` (`ActionHelpers.cs:68–70`). The object has two `PlayMakerFSM`
components — `BOSSES#components[0].components` lists them at indices 7 and 10, and the FSM dump
enumerates `Stun Control` before `Control` — and nothing establishes which one `GetComponent`
returns. The **outcome is order-independent** for both call sites (neither FSM declares an int `HP`
or a float `Recoil per second`, and `FsmVariables.GetFsmInt` returns a throw-away `new FsmInt(name)`
on a miss, `PM/FsmVariables.cs:1304–1308`), so this does not block the port; it is recorded because
any future action that reads a variable that *does* exist on one of the two would become
order-sensitive. Evidence needed: a runtime probe logging the resolved FSM's `FsmName`.

### Q-hornet-3 — Does `transform.parent = x` preserve the world pose?

`ACT/SetParent.cs:30–46` assigns `ownerDefaultTarget.transform.parent` and, only when the
corresponding flags are set, zeroes `localPosition`/`localRotation`; both flags are literal `false`
in `FSM#Needle Tink/Setup and Follow/{Setup, Deparent}`. Nothing in `analysis/decomp/` or any dump
states what the assignment does to the world pose, so the first draft's "world position not
preserved" was unsupported and has been removed. This matters because both the `Needle` and
`Needle Tink` are de-parented from Hornet at scene start and their world positions are then read
(`Init`'s `GetPosition → $Return Vector`) and written (`Throw`'s `SetPosition`) in world space.
Evidence needed: a trace or probe recording the needle's `transform.position` immediately before and
after each `SetParent`, or an authoritative decompiled source for the setter.

### Q-hornet-4 — Is `GameObject.SetActive(true)` → `OnEnable` synchronous?

`FSM#…/Control/Throw` sets the needle's world position (action index 6) and *then* activates it
(index 7); `Needle/Control` has `restartOnEnable: true`, so `Fsm.OnEnable` restarts it at `Init`
(`PM/Fsm.cs:1847–1854`), and `Init`'s `GetPosition(Owner, space=World) → $Return Vector` reads
whatever the transform holds at that instant. If `SetActive` raises `OnEnable` synchronously inside
`ActivateGameObject.OnEnter`, `$Return Vector` is the just-written throw origin (§1.2); if the enable
is deferred to the next frame, it could instead read a stale or physics-advanced position. The same
question governs every `ActivateGameObject`-toggled effect child. analysis/specs/fsm-actions.md Q7 raises the same
point. Evidence needed: a trace of `$Return Vector` across two consecutive throws from different
positions — if it tracks the throw origin, the synchronous reading is confirmed.

### Q-hornet-5 — Script-execution order within a phase

§3's `DecelerateV2` verification shows a `FixedUpdate` gravity integration landing *between*
`Sphere Antic A`'s `SetGravity2dScale 0` and its first `DecelerateV2` tick, which constrains but does
not determine the ordering of: `PlayMakerFSM.Update` vs `HealthManager.Update` (the
`evasionByHitRemaining` countdown, `HK/HealthManager.cs:329`) vs `ConstrainPosition.Update`
(`HK/ConstrainPosition.cs:17`); `PlayMakerFixedUpdate.FixedUpdate` vs the Box2D step (Hornet's
`Control`, `Evade Check` and `Needle/Control` all have `handleFixedUpdate: true` in the dump); and
`tk2dSpriteAnimator.LateUpdate` vs `PlayMakerLateUpdate`. §4's timing rules are calibrated to the
observed order rather than derived from it. Evidence needed: the sim/core scheduler decision recorded
against analysis/specs/frame-order.md's measurements; this is the same gap as the review's G5.

### Q-hornet-6 — `Physics2D.Raycast` semantics for `CheckCollisionSide`

`ACT/CheckCollisionSide.cs:165–184` fires three rays per side from the caster's own
`col2d.bounds` corners/midpoint, length 0.08, mask `1 << (int)layer` with `layer` a `LayerMask`
implicitly converted from the literal `8`. Three things are not established by any source: whether
that implicit conversion is the identity on the int (so the mask is `1 << 8`); whether a ray
originating inside the caster's own collider can hit it (`PHYS#Physics2D.queriesStartInColliders` is
`false`, and Hornet is layer 11 while the mask is layer 8, so it cannot bite *here*, but the sim's
raycast must still match Unity for the `otherLayer = true` paths); and Unity's tie-break when two
colliders are hit at the same distance. Evidence needed: a runtime raycast probe, or the
`Physics2D`/Box2D query source.

### Q-hornet-7 — iTween easing and completion

Two Hornet behaviours are iTween-driven and neither has been read from source:
`FSM#Needle/Control/Return` (`iTweenMoveTo` to `$Return Vector`, `time 1.0`, `speed 30`,
`space Self`, `easeType easeInSine`, `finishEvent FINISHED`) sets the length of `Control/Thrown` —
60 FRAME records in `r2_rand1.a` (§4.4), a number the spec reports without predicting; and
`FSM#…/Sphere Ball/Grow` (`iTweenScaleTo` (1.5, 1.5, 1), `time 0.25`, `easeOutSine`) changes a
**live hitbox radius** (2.53 × 0.8 = 2.024 → 2.53 × 1.5 = 3.795), so it is not cosmetic. Evidence
needed: the iTween implementation (easing curve, whether `speed` overrides `time`, per-frame vs
coroutine stepping, whether it draws from `UnityEngine.Random`), and a trace of the sphere collider's
world bounds during `Sphere`/`Sphere A`.

### Q-hornet-8 — `ParticleSystem`, `SendMessage` and `FindGameObjectsWithTag` ordering

`FSM#…/Control/Hard Land`'s `PlayParticleEmitter($Dust HardLand, emit=0)`,
`FSM#…/Control/Stun Start`'s `SpawnObjectFromGlobalPool`, and the death-path
`SpawnRandomObjectsV2`/`FindGameObject`/`DestroyObject` chain all depend on ordering rules
(particle-system emission scheduling, `SendMessage` receiver order, `FindGameObjectsWithTag` return
order) that no source in `analysis/` pins down. None of them is on the observation path for a live
Hornet fight, so this does not block P4; it blocks a bit-faithful death sequence and any future
pooled-projectile boss. Evidence needed: a runtime probe or decompiled source — engine documentation
is not admissible under rule 2.2.

### Q-hornet-9 — `tk2d` wrap modes other than `Once` (observation parity only)

Hornet uses `Loop` (`Idle`, `Run`, `Fall`, `Evade`, `A Dash`) and `LoopSection` (`Jump`, `G Dash`,
`Sphere Attack`, `Throw`, `Stun`) clips, none of which ever drives a transition — only `Once` clips
raise `animationCompleteEvent` (§4.3), so for the FSM they are cosmetic. They are **not** cosmetic
for P6: `anim_phase` and the animation-clip vocab id are observation columns the trainer consumes
(the FullKnight training repo's project instructions, section "Combat hitbox features" — outside
this repo, so not citable under rule 2.2; the authority for the wire layout is
`oracle/Net/BinaryProtocol.cs`), and a wrong `loopStart` wrap would desynchronise them. analysis/specs/tk2d-animator.md §1.3 documents the wrap modes; this entry exists only to record that §4.3's
clip table lists `wrapMode`/`loopStart` per clip but does not itself verify the looping arithmetic
against a trace. Evidence needed: replay the §4.3 table through analysis/specs/tk2d-animator.md's model and diff
`anim.clip`/`anim.frame`/`anim.clip_time` against the R2 traces for the looping clips.

### Q-hornet-10 — Dead FSM elements: confirm nothing outside Hornet drives them

`Aim Sphere Jump` is unreachable in this build (its only producer, the `BoolTest($Will Sphere)` in
`Aim Jump`, is `enabled:false`); the floats `Min Dstab Height` (33.31) and `Air Dash Height` (31.5)
and the bools `Above Air Dash Height` and `Over Min Height` are never read by any action in
`FSM#…/Control`; `Stun Control`'s `Decrement` int and `Tag` string are never referenced; and
`Stun Control/Stun`'s `IntCompare($Stuns Total, $Stuns Max)` raises an event `MAX` for which the FSM
has neither a state transition nor a global transition, making the 5-stagger cap inert. All of these
were established by searching only Hornet's own FSMs, so an external `SetFsmBool`/`SetFsmFloat`/
`SendEvent` targeting `Boss Holder/Hornet Boss 1` would invalidate them. Evidence needed: a
scene-wide sweep of `analysis/fsm/GG_Hornet_1.json` for actions whose target GameObject resolves to
Hornet — blocked today because `FsmEventTarget` serialises as `__unserialized` in the dump (only the
`target` enum survives), which is the review's G11.

## Review fixes

Applied against `analysis/specs/REVIEW-p1.md` (2026-08-31). Defect id → what changed.

| id | change |
|---|---|
| D01 | §2.2: `GG Reset`'s `SetInvincible.Invincible` is the **literal** `{useVariable:false, value:false}`, so `HK/SetInvincible.cs:29–32` writes `IsInvincible = false`. Deleted the "None → skipped" claim, the "clearing agent is outside these two FSMs" conclusion, and the old **Q5** (void). §5's "Invincibility windows" paragraph rewritten to match. |
| D02 | §2.2 and §5: replaced "invincible = true for the whole capture" with the measured split — `r2_rand1.a` 42 True / 438 False, first `False` at `Time.frameCount` 24768 = FRAME index 42 (the first `Flourish` record); `r2_rand2.a` 42/438, `r2_move.a` 42/558, all flipping at index 42. Re-measured from the traces, not quoted. |
| D03 | §1.2: `Needle/Control` has `restartOnEnable: true` (FSM dump), so `Fsm.OnEnable` (`PM/Fsm.cs:1847–1854`) re-enters `Init` on every `ActivateGameObject($Needle, true)`. `$Return Vector` is therefore the **throw origin** re-captured each throw (`Throw` writes `SetPosition($Needle)` at action index 6 before activating at index 7), not the scene-start spawn position. Dump corroboration added (`started:true, finished:true, activeState:""`). New **Q-hornet-4** for the `SetActive` → `OnEnable` synchrony this depends on. |
| D04 | §1.2 and §6.4: removed "world position not preserved" in both places. Now states only what `ACT/SetParent.cs:30–46` does (assigns `transform.parent`; `resetLocalPosition`/`resetLocalRotation` both literal `false` in the two Needle Tink states) and marks the world-pose question UNKNOWN → **Q-hornet-3**. |
| D05 | New **§6.4.1 RNG draw census**. Adds the CameraShake path: `Control/{G Dash, A Dash, Sphere, Sphere A}` → `EnemyKillShake` → `FSM#_GameCameras/CameraParent/CameraShake` global → `To Kill Shake` → `ShakingKill` → `ShakePositionV2` (Extents `$EnemyKillShake` = (0.105,0.105,0), Duration 0.5, `$FPS Limit` = 0 ⇒ throttle off), 3 draws per `UpdateShaking` (`ACT/ShakePositionV2.cs:96`), called once in `OnEnter` (`:62`) and once per `OnUpdate` (`:68`), stopping on `timer > Duration` **after** drawing (`:81`, `:98`). Computed count is **25 calls = 75 draws** per `EnemyKillShake` and **51 calls = 153 draws** per `AverageShake` (Duration 1.0) — the review's 78 assumed 26 calls; float32 `Σ 0.02f` first exceeds 0.5 at k = 25 (0.50000006), not 26. Also documents the **priority gate** (`To Kill Shake` requires `$Priority < 6`, so overlapping shakes are dropped, not stacked), the knight-hit paths (`HK/HealthManager.cs:371–372` clink → `EnemyKillShake` + `FreezeMoment(1)`; `FSM#Knight/Effects/Damage Effect/Knight Damage/Gen` → `AverageShake`), and the conclusion that the P4 seeded gate needs the sim to tick `CameraShake`. §6.1 gains the CameraShake action types. |
| D06 | §3 and §2.3: layer 8 comes from `otherLayer == false` (`ACT/CheckCollisionSide.cs:131–138`, field declared `public bool otherLayer` at `:39`), **not** from `otherLayerNumber = 0`, which is never read on any of the 7 Hornet instances. Fixed in all three places the claim appeared. |
| D07 | Header caveat 2: `frameCountAtDump` 26760 → **29672**, `timestampUtc` 2026-08-31T02:49:29Z. |
| D08 | Closed the old Q3/Q4/Q6/Q7 from `analysis/dumps/GG_Hornet_1/scene.json`, added a `SCENE#` citation key, and cited it in: §1.1 (a `DamageHero` table — `Hit GDash`/`Hit ADash`/`Sphere Ball`/`Needle` all `damageDealt 1, hazardType 1, resetOnEnable false`; body 0 at dump because `Control` was in `GG Fall` between `GG Intro 1` and `GG Reset`); §1.2 (`Needle` = `PolygonCollider2D`, layer 22, trigger, 3 points, no Rigidbody2D, world polygon given; `Needle Tink` = `BoxCollider2D` 3.2466042 × 0.251131058, offset (−0.070205, −1.907e-06), **layer 17 Attack**, trigger, `enabled:false`, **no `DamageHero`** — a clink surface, not a damage source; `Needle/Thread` and `Stun Effect` have no Collider2D at all, confirmed absent from all 1251 rows); §3 (`ConstrainPosition` live: `constrainX true, xMin 15.07, xMax 37.96, constrainY false` — the real arena clamp, 0.06 tighter on each side than the A-Dash `Wall X` bounds); §5 (Recoil, see D09). §1's collider-table footer and the `A Dash Range` world polygon now cite `SCENE#` too. |
| D09 | §5: replaced the `Recoil.cs:77–84` `Reset()` defaults with the live `SCENE#` values in a table, flagging the two that differ — `recoilDuration` **0.15** (not 0.5) and `stopVelocityXWhenRecoilingUp` **false** (not true) — plus `preventRecoilUp: true`, which makes an up-direction hit a complete no-op (`HK/Recoil.cs:122`). |
| D10 | §2.6: `Stun Control`'s variable list now includes the int `Decrement` (and the string `Tag`), with the verified note that neither is read or written by any action in the FSM. |
| D11 | §1.2: rewritten. `Setup` de-parents **Needle Tink** (`SetParent(Owner, parent=None)`) and writes Control's `$Needle Tink` via `SetFsmGameObject($Hornet, "Control", "Needle Tink", $Self)`; `Deparent` de-parents **Needle** and deactivates it. The `Pause`-state `FindChild` chain is noted as valid only until `Setup` overwrites it. |
| D12 | §2.3: keeps the question (now **Q-hornet-2**) but states the outcome is order-independent — neither FSM declares an int `HP`, and `PM/FsmVariables.cs:1304–1308` returns a fresh `new FsmInt(name)` on a miss rather than writing the action's `storeValue`. |
| D13 / C7 | No change needed — §2.6 already states the `Stuns Max` cap is inert because `MAX` has no transition. Recorded in **Q-hornet-10** so the sweep that would invalidate it is named. |
| C11 | §4.2: added an explicit note that the two closed forms are a **measurement convention for the R2 traces, not the thing to port** (the `−1` depends on the entering path), and that the mechanism to port is the float32 accumulator plus tk2d's clip clock. Old Q1 closed: `Stun Land`'s `Wait 3.0` = **150 records** (first `k` with `Σ 0.02f ≥ 3.0` is 151). |
| C13 | §4.1: the FRAME capture is described as the coroutine resume scheduled **after every `Update` and before `LateUpdate`** (analysis/specs/frame-order.md §3), replacing the loose "sits in Update". |
| old Q2 → closed | §6.4.1: `rng_probe.json` shows 281/282 probed ops advance the state by one step; the sole exception is the degenerate **integer** `Range(3,3)`. A degenerate **float** range still advances, so `Jump`'s `RandomFloat(41, 41)` does consume a draw — the spec's original claim, now cited. |
| old Q8 → closed | tk2d wrap-mode semantics are documented in analysis/specs/tk2d-animator.md §1.3; the residual (observation-parity verification of the looping clips) is retained as **Q-hornet-9**. |
| Q-format | All remaining open questions rewritten as `### Q-hornet-<n> — <title>` + one paragraph, with the closed ones listed once at the top of §7 so the consolidation script does not re-open them. Review gaps G3/G4/G5/G10/G13 that named this spec are now Q-hornet-3/4/5/7/8; G11 is named inside Q-hornet-10 as the blocker for the dead-element sweep. |
