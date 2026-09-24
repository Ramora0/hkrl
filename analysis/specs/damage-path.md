# Damage path (P1.4)

Scope: contact damage enemy→knight, nail/spell damage knight→enemy, i-frames, recoil, hit-stop,
boss HP/stagger/death, and the six observation columns that read this subsystem.
Target scene: `GG_Hornet_1`. Regime: R2 (`STATE.md`) — captureDeltaTime = fixedDeltaTime = 0.02,
frames_per_wait 2, `Time.timeScale` 0 between agent steps.

Citation keys:
- `D:<file>:<line>` = `analysis/decomp/Assembly-CSharp/<file>:<line>`
- `H:<path>` = `analysis/dumps/GG_Hornet_1/hero.json` → `heroController[name=<path>].value`
- `P:<path>` = `analysis/dumps/GG_Hornet_1/playerdata.json` → `fields[name=<path>].value`
- `B:<path>` = `analysis/dumps/GG_Hornet_1/bosses.json` → `healthManagers[0]…`
- `X:<path>` = `analysis/dumps/GG_Hornet_1/physics.json`
- `F:<goPath>::<fsm>[<state>]` = `analysis/fsm/GG_Hornet_1.json` → `fsms[path=<goPath>, fsmName=<fsm>]`
- `T:<trace>@<frame>` = `analysis/traces/p0/<trace>`
- `O:<file>:<line>` = `oracle/<file>:<line>`

Rule for the port: §1–§4 are a transcription of the cited code. Trace rows in §6 are verification
only; no rule below is inferred from a trace. Anything the sources do not state is `UNKNOWN` in §7.

---

## 0. Layers, colliders, physics context

| layer | name | source |
|---|---|---|
| 8 | Terrain | `X:layerNames[8]` |
| 9 | Player | `X:layerNames[9]` |
| 11 | Enemies | `X:layerNames[11]` |
| 16 | Tinker | `X:layerNames[16]` |
| 17 | Attack | `X:layerNames[17]` |
| 19 | Interactive Object | `X:layerNames[19]` |
| 20 | Hero Box | `X:layerNames[20]` |
| 22 | Enemy Attack | `X:layerNames[22]` |

Collision matrix (`X:layerCollisionMatrix.ignoreLayerCollision[a][b]`, `false` = the pair interacts):
- 20↔11 interact, 20↔22 interact, 20↔17 interact, 20↔12 interact, 20↔19 interact.
- 20↔8 and 20↔23 do NOT interact.
- 17↔11 interact, 17↔19 interact; 17↔22 and 17↔16 do NOT.
- 9↔11 do NOT interact (the knight body passes through enemy bodies); 9↔8 interact.

Knight colliders (`X:heroColliders`):
- `Knight` BoxCollider2D, layer 9, isTrigger false, offset (0, −0.75), size (0.5, 1.28125), edgeRadius 0.0025, material `FrictionlessSurface`.
- `Knight/HeroBox` BoxCollider2D, layer 20, **isTrigger true**, offset (0.00557327271, −0.6942673), size (0.455413818, 1.16978645).
- `Knight/Attacks/{Slash,AltSlash,UpSlash,DownSlash,WallSlash,Great Slash,Dash Slash}` PolygonCollider2D, layer 17, isTrigger true, `enabled=false` at rest.
- `Knight/Attacks/Cyclone Slash/Hits/{Hit L,Hit R}` PolygonCollider2D, layer 17, isTrigger true, `enabled=true` (the parent GameObject is inactive).
- `Knight/Attacks/<slash>/Clash Tink` PolygonCollider2D, layer 16.

Hornet colliders (`B:colliders`): body BoxCollider2D layer 11 non-trigger; `Hit GDash`, `Hit ADash`
PolygonCollider2D layer 22 trigger; `Sphere Ball` CircleCollider2D layer 22 trigger r=2.53; the rest
are layer 13/14 detectors.

Physics: `gravity=(0,−60)`, `velocityIterations=8`, `positionIterations=3`, `queriesHitTriggers=true`,
`callbacksOnDisable=true`, `autoSyncTransforms=true`, `simulationMode=FixedUpdate`,
`defaultContactOffset=0.01` (`X:Physics2D`); `Time.fixedDeltaTime=0.02` (`X:Time`).
Dump provenance: `analysis/dumps/GG_Hornet_1/meta.json` — `timestampUtc = 2026-08-31T02:49:29Z`,
`frameCountAtDump = 29672`, `oracleGit = 31d43cf9f52c048fd95eeffe90d9cf15cd5aa0a3`,
`timeScaleAtDump = 0`. This dump **is** R2: `X:Time.captureDeltaTime = 0.02 = X:Time.fixedDeltaTime`.
The only regime artefact is `X:Time.timeScale = 0`, and that is the capture point rather than the
regime — dumps run at `SceneReady`, after `Time.timeScale = 0` (`docs/trace-format.md`
§Capture points). Every `X:` / `H:` / `P:` / `B:` / `S:` value cited below was re-read against this
dump.
`S:<path>` = `analysis/dumps/GG_Hornet_1/scene.json` → `colliders[path=<path>]`. That dump carries
1251 `Collider2D`s of the loaded scenes (1220 `DontDestroyOnLoad` + 31 `GG_Hornet_1`; 165 skipped as
not-loaded) together with their GameObject's non-Unity component fields.

---

## 1. Enemy → knight

### 1.1 Detection

`HeroBox` (MonoBehaviour on `Knight/HeroBox`) is the only entry point.
`OnTriggerEnter2D` and `OnTriggerStay2D` both call `CheckForDamage(otherCollider)` unless the static
`HeroBox.inactive` is set (`D:HeroBox.cs:25-39`). `HeroBox.inactive = true` is set in the knight's
death coroutine (`D:HeroController.cs:3733`).

`CheckForDamage` (`D:HeroBox.cs:41-74`), in order:
1. If `FSMUtility.ContainsFSM(other.gameObject, "damages_hero")` (`D:FSMUtility.cs:102-121`):
   `damageAmount = FSMUtility.GetInt(fsm,"damageDealt")`, `hazardType = GetInt(fsm,"hazardType")`
   (`D:FSMUtility.cs:200-203` = `FsmVariables.FindFsmInt(name).Value`), then
   `heroCtrl.TakeDamage(other.gameObject, side, damageAmount, hazardType)` **immediately** and return.
2. Else `DamageHero component = other.gameObject.GetComponent<DamageHero>()`; if non-null and
   `!(cState.shadowDashing && component.shadowDashHazard)`, cache
   `damageDealt/hazardType/damagingObject/collisionSide`.
   - `side = other.transform.position.x > heroBox.transform.position.x ? right : left`
     (`D:HeroBox.cs:48-50, 64`). Note: the comparison is against the **HeroBox** transform, and uses
     `transform.position` of the *collider's GameObject*, not the collider bounds.
   - `IsHitTypeBuffered(hazardType)` is `hazardType == 0` (`D:HeroBox.cs:76-79`). Buffered hits are
     deferred to `LateUpdate` → `ApplyBufferedHit` (`D:HeroBox.cs:81-93`); all others call
     `heroCtrl.TakeDamage` directly from `CheckForDamage`, i.e. from whatever context Unity dispatched
     `OnTriggerEnter2D` / `OnTriggerStay2D` in. **When Unity dispatches those callbacks relative to
     `FixedUpdate`, and whether `OnTriggerStay2D` fires exactly once per physics step, is an engine
     rule no source in `analysis/` states — Q-dmg-11.** What the trace bounds: all 44 `HERO_DAMAGE`
     records in `T:r2_rand1.a` carry `phase = 1`, i.e. they were emitted after that frame's
     `RecorderBehaviour.FixedUpdate` and before its `Update` (`O:Oracle/TraceRecorder.cs:277-286`),
     and consecutive records from one source are exactly 1 `fixed_count` apart (§6.1).

`DamageHero` (`D:DamageHero.cs`) is pure data: `damageDealt` (default 1), `hazardType` (default 1),
`shadowDashHazard`, `resetOnEnable` (restores `damageDealt` to its first-seen value on enable,
`D:DamageHero.cs:15-28`). `SetDamageHeroAmount` (`D:SetDamageHeroAmount.cs:18-30`) is the FSM action
that writes `damageDealt`.

GG_Hornet_1 carriers of `DamageHero` (`S:`, five in the whole loaded scene): `Boss Holder/Hornet Boss 1`
(body), `…/Hit GDash`, `…/Hit ADash`, `…/Sphere Ball`, and the scene-root `Needle`. (`B:components`
lists only the first four, because it enumerates the boss subtree; `Needle` is a sibling at the scene
root — `F:Boss Holder/Hornet Boss 1::Control` holds it in the `` variable.) The only other
`DamageHero`s in the dump are three pooled `Gas Explosion Recycle` clones, all inactive.
No object in the scene has a `damages_hero` FSM (`analysis/fsm/GG_Hornet_1.json`, zero matches), so
path (1) is dead for this scene and every knight hit comes through path (2).

`DamageHero` field values, all from `S:…components[DamageHero]` (dump taken at `SceneReady`, i.e.
mid-intro):

| GameObject | `damageDealt` | `hazardType` | `shadowDashHazard` | `resetOnEnable` |
|---|---|---|---|---|
| `Boss Holder/Hornet Boss 1` (body) | **0** | 1 | false | false |
| `Boss Holder/Hornet Boss 1/Hit GDash` | 1 | 1 | false | false |
| `Boss Holder/Hornet Boss 1/Hit ADash` | 1 | 1 | false | false |
| `Boss Holder/Hornet Boss 1/Sphere Ball` | 1 | 1 | false | false |
| `Needle` | 1 | 1 | false | false |

The body's 0 is the intro value written by `F:Boss Holder/Hornet Boss 1::Control[GG Intro 1]` →
`SetDamageHeroAmount target=OWNER damageDealt=0`; `F:…::Control[GG Reset]` →
`SetDamageHeroAmount target=OWNER damageDealt=1` restores it when the fight starts, which is what the
trace observes (§6.1). The other four are serialized prefab values that no FSM writes (those two are
the only `SetDamageHeroAmount` instances in `analysis/fsm/GG_Hornet_1.json`).
`resetOnEnable = false` on all five ⇒ the `OnEnable` restore path (`D:DamageHero.cs:15-28`) never runs
in this scene.

### 1.2 `HeroController.TakeDamage` control flow

Signature `TakeDamage(GameObject go, CollisionSide damageSide, int damageAmount, int hazardType)`
(`D:HeroController.cs:1825`).

1. `damageAmount = ModHooks.OnTakeDamage(ref hazardType, damageAmount)` (`:1827`).
   The oracle registers nothing on this hook (`O:Environment/TrainingEnv.cs:1142-1147` hooks
   `AfterTakeDamageHook`, not `TakeDamageHook`), so it is identity for the sim.
2. `if (damageAmount <= 0) return;` (`:1829-1832`).
3. **Boss tier multiplier** (`:1833-1844`): if `BossSceneController.IsBossScene`
   (`D:BossSceneController.cs:60` = `Instance != null`), then
   `BossLevel==2 → damageAmount = 9999`; `BossLevel==1 → damageAmount *= 2`; `BossLevel==0 → unchanged`.
   The mod forces tier 0: `BossChallengeUI.LoadBoss` is overridden to `SceneHooks.ForcedTier`
   (`O:HKOracle.cs:128-147`), the `BossSceneController.BossLevel` **setter** is detoured to suppress
   every nonzero write (`O:HKOracle.cs:159-170`), and `ClampBossLevel()` resets it at each episode
   boundary (`O:Environment/TrainingEnv.cs:1243-1256`). ⇒ the sim implements the `case 0` branch only,
   but must keep the branch (Q-dmg-1).
4. `if (CanTakeDamage())` (`:1845`) — full conjunction (`D:HeroController.cs:4802-4809`):
   `damageMode != DamageMode.NO_DAMAGE && transitionState == HeroTransitionState.WAITING_TO_TRANSITION
   && !cState.invulnerable && !cState.recoiling && !playerData.isInvincible && !cState.dead
   && !cState.hazardDeath && !BossSceneController.IsTransitioning`.
   `IsTransitioning` = `Instance.isTransitioningOut` (`D:BossSceneController.cs:64-73`).
   Dump values at SceneReady: `H:damageMode = FULL_DAMAGE`, `H:transitionState = WAITING_TO_TRANSITION`,
   `P:isInvincible = false`, `P:invinciTest = false`, `H:takeNoDamage = false`.

**Taken branch** (`CanTakeDamage()` true), in source order:
- `:1847-1850` early `return` if `(damageMode==HAZARD_ONLY && hazardType==1) || (cState.shadowDashing && hazardType==1) || (parryInvulnTimer > 0f && hazardType==1)`.
  `H:parryInvulnTimer = 0`; set only by parry/quake/cyclone entry points (`:1795, :1807, :1817`) to
  `INVUL_TIME_PARRY=0.25`, `INVUL_TIME_QUAKE=0.4`, `INVUL_TIME_CYCLONE=0.25` (`H:`).
- `:1853-1918` Carefree Melody shield, gated on **`carefreeShieldEquipped && hazardType == 1`**
  (`:1853`; `H:carefreeShieldEquipped = false`) → skipped. When active: `hitsSinceShielded` is clamped
  to 7 (`:1855-1858`), then one `UnityEngine.Random.Range(1, 100)` is drawn and compared against a
  per-count threshold — 10 / 20 / 30 / 50 / 70 / 80 / 90 for `hitsSinceShielded` 1..7, `default:
  flag = false` otherwise (`:1859-1906`). On success `hitsSinceShielded = 0`,
  `carefreeShield.SetActive(true)`, `damageAmount = 0`, `spawnDamageEffect = false`, `flag = true`;
  otherwise `hitsSinceShielded++` (`:1907-1917`). RNG-relevant — Q-dmg-2.
- `:1919-1929` Baldur Shell: `equippedCharm_5 && blockerHits > 0 && hazardType == 1 && cState.focusing
  && !flag` — the **`!flag`** conjunct (`:1919`) means a carefree-shield block in the same call
  suppresses this branch. True ⇒ `proxyFSM.SendEvent("HeroCtrl-TookBlockerHit")`,
  `spawnDamageEffect = false`, `damageAmount = 0`. `P:equippedCharm_5 = false` ⇒ the `else` branch
  fires `proxyFSM.SendEvent("HeroCtrl-HeroDamaged")`.
- `:1930` `CancelAttack()` → if `cState.attacking`: `slashComponent.CancelAttack()` + `ResetAttacks()`
  (`D:HeroController.cs:4072-4079`, `:4111-4119`).
- `:1931-1939` clear `cState.wallSliding`, `cState.touchingWall`.
- `:1940-1943` `if (recoilingLeft||recoilingRight) CancelRecoilHorizontal()` (`:4088-4093`).
- `:1944-1953` `if (bouncing||shroomBouncing) { CancelBounce(); rb2d.velocity = (vx, 0) }`.
- `:1958` `damageAmount = ModHooks.AfterTakeDamage(hazardType, damageAmount)` — the oracle subscribes
  here (`O:Environment/TrainingEnv.cs:1144`, handler `:1161-1170`) and returns `damage` unchanged, so
  identity for the sim.
- `:1959-1969` `if (!takeNoDamage && !playerData.invinciTest)`:
  `playerData.TakeHealth(overcharmed ? damageAmount*2 : damageAmount)`. `P:overcharmed = false`.
- `:1970-1980` Grubsong, gated on **`equippedCharm_3 && damageAmount > 0`** (`:1970`;
  `P:equippedCharm_3 = false`) → skipped; would `AddMPCharge(GRUB_SOUL_MP = 15)`, or
  `GRUB_SOUL_MP_COMBO = 25` when `equippedCharm_35` is also set (`H:`).
- `:1985-1989` `if (cState.nailCharging || nailChargeTimer != 0) { nailCharging=false; nailChargeTimer=0; }`
  — a hit cancels a nail-art charge.
- `:1990-1993` **`if (damageAmount > 0 && OnTakenDamage != null) OnTakenDamage();`** — a fully
  blocked hit (carefree or Baldur) does not raise it. `BossSceneController.Setup` subscribes
  `SetKnightDamaged` here (`D:BossSceneController.cs:195-199`).
- `:1994-1998` `if (playerData.health == 0) { StartCoroutine(Die()); return; }`.
- `:1999-2016` dispatch on `hazardType`: `2`→`DieFromHazard(SPIKES, go.transform.rotation.z)`,
  `3`→`DieFromHazard(ACID)`, `4`→log only, `5`→`DieFromHazard(PIT)`,
  **default (incl. 1)** → `StartCoroutine(StartRecoil(damageSide, spawnDamageEffect, damageAmount))`.

**Not-taken branch** (`:2018-2058`): returns immediately unless
`cState.invulnerable && !cState.hazardDeath && !playerData.isInvincible`; then only `hazardType` 2/3/4
do anything (spike/acid still kill through i-frames). `hazardType==1` is a no-op. ⇒ under R2 with
hazardType 1, an i-framed contact costs nothing but **still calls `TakeDamage`** (visible in the trace
as a HERO_DAMAGE event with unchanged `hp_after`, §6).

### 1.3 `StartRecoil` and `Invulnerable`

`StartRecoil(CollisionSide impactSide, bool spawnDamageEffect, int damageAmount)`
(`D:HeroController.cs:3782-3833`):
1. `if (cState.recoiling) yield break;`
2. `playerData.disablePause = true`.
3. `ResetMotion()` (`:4129-4142`: CancelJump, CancelDoubleJump, CancelDash, CancelBackDash,
   CancelBounce, CancelRecoilHorizontal, CancelWallsliding, `rb2d.velocity = Vector2.zero`,
   `transition_vel = 0`, `wallLocked=false`, `nailChargeTimer=0`).
4. `AffectedByGravity(false)` (observable as `rb2d.gravityScale = 0`).
5. `recoilVector`: `impactSide==left → (RECOIL_VELOCITY, RECOIL_VELOCITY*0.5)` and FlipSprite if facing
   right; `impactSide==right → (−RECOIL_VELOCITY, RECOIL_VELOCITY*0.5)` and FlipSprite if facing left;
   otherwise `Vector2.zero` (`:3791-3810`). `H:RECOIL_VELOCITY = 15.0` ⇒ ±(15, 7.5).
6. `SetState(ActorStates.no_input)`; `cState.recoilFrozen = true` (`:3811-3812`).
7. `if (spawnDamageEffect) { damageEffectFSM.SendEvent("DAMAGE"); if (damageAmount > 1) Instantiate(takeHitDoublePrefab, …) }`.
8. `StartCoroutine(Invulnerable(equippedCharm_4 ? INVUL_TIME_STAL : INVUL_TIME))` (`:3821-3828`).
   `P:equippedCharm_4 = false` ⇒ `INVUL_TIME`.
9. `yield return takeDamageCoroutine = StartCoroutine(gm.FreezeMoment(DAMAGE_FREEZE_DOWN,
   DAMAGE_FREEZE_WAIT, DAMAGE_FREEZE_UP, 0.0001f))` (`:3829`) — **the mod replaces every
   `GameManager.FreezeMoment` overload with an empty coroutine**
   (`O:Environment/TrainingEnv.cs:1080-1088`, `_killFreezeFloat/_killFreezeBool/_killFreezeInt/_killFreezeMomentGC`),
   so `Time.timeScale` is never touched. How many frames a `yield return StartCoroutine(...)` on a
   coroutine that immediately `yield break`s costs is a Unity rule no source in `analysis/` states
   (Q-dmg-8); the trace bounds it at **exactly 1 recorded frame per hit**: `cState.recoilFrozen`
   reads 1 on 1 recorded FRAME in `T:r2_move.a` (1 landed hit), on 2 in `T:r2_rand1.a` and 2 in
   `T:r2_rand2.a` (2 landed hits each), and `cState.recoiling` is set on the next recorded frame
   after `recoilFrozen` clears (§6.1).
   Vanilla body for reference: `D:GameManager.cs:2887-2897` (ramp to `targetSpeed` over `rampDownTime`,
   hold `waitTime` measured in `Time.unscaledDeltaTime`, ramp back to 1 over `rampUpTime`).
   `GameManager.FreezeMoment(int)` presets: 0=(0.01,0.35,0.1,0), **1=(0.04,0.03,0.04,0)**,
   2=(0.25,2,0.25,0.15), 3/4/5=(0.01,0.25,0.1,0) (`D:GameManager.cs:2860-2886`).
10. `cState.recoilFrozen = false; cState.recoiling = true; playerData.disablePause = false;`

`Invulnerable(float duration)` (`D:HeroController.cs:3835-3844`):
```
cState.invulnerable = true;
yield return new WaitForSeconds(DAMAGE_FREEZE_DOWN);   // 0.001
invPulse.startInvulnerablePulse();
yield return new WaitForSeconds(duration);             // INVUL_TIME = 1.3
invPulse.stopInvulnerablePulse();
cState.invulnerable = false;
cState.recoiling  = false;
```
Whether `WaitForSeconds` accumulates scaled or unscaled time is a Unity rule no source in
`analysis/` states (Q-dmg-8). Measured instead: `Time.deltaTime == 0` on the frozen inter-step frame
(`analysis/specs/frame-order.md` §3.2, derived from Δ`Time.time` = 0.02 across a Δ`frame` = 2 FRAME
pair), and the i-frame window is a constant **67 recorded frames** in both the `.a` and the `.b` run
of the same corpus (§6.1). `Time.unscaledDeltaTime` is raw wall clock under this regime and differs
every frame (`analysis/open-questions.md` Q3), so an unscaled wait would give a run-varying frame
count; it does not. That is evidence for scaled time, not a citation for it.

**Damage constants** (values from `H:`, the runtime inspector reads). For the `public float` block
at `D:HeroController.cs:103-181` the decomp carries **no** initialiser, so there the decomp is not a
source at all (cf. `analysis/open-questions.md` Q9) and only the dump is. Two rows below are
different: `DEATH_WAIT` is `D:HeroController.cs:197 private float DEATH_WAIT = 2.85f;` and
`ATTACK_QUEUE_STEPS` is `:189 private int ATTACK_QUEUE_STEPS = 5;` — both **are** initialised in the
decomp, and the dump agrees with both.

| constant | value | decl |
|---|---|---|
| `INVUL_TIME` | 1.3 | `D:HeroController.cs:161` |
| `INVUL_TIME_STAL` | 1.75 | `:163` |
| `INVUL_TIME_PARRY` | 0.25 | `:165` |
| `INVUL_TIME_QUAKE` | 0.4 | `:167` |
| `INVUL_TIME_CYCLONE` | 0.25 | `:169` |
| `DAMAGE_FREEZE_DOWN` | 0.001 | `:155` |
| `DAMAGE_FREEZE_WAIT` | 0.25 | `:157` |
| `DAMAGE_FREEZE_UP` | 0.05 | `:159` |
| `RECOIL_VELOCITY` | 15.0 | `:153` |
| `RECOIL_DURATION` | 0.2 | `:149` |
| `RECOIL_DURATION_STAL` | 0.08 | `:151` |
| `RECOIL_HOR_VELOCITY` | 3.75 | `:129` |
| `RECOIL_HOR_VELOCITY_LONG` | 16.0 | `:131` |
| `RECOIL_HOR_STEPS` | 8.0 | `:133` |
| `RECOIL_HOR_TIME` | 0.1 | `:127` |
| `RECOIL_DOWN_VELOCITY` | 0.0 | `:135` |
| `BOUNCE_VELOCITY` | 12.0 | `:123` |
| `SHROOM_BOUNCE_VELOCITY` | 25.0 | `:125` |
| `BOUNCE_TIME` | 0.25 | `:119` |
| `DEFAULT_GRAVITY` | 0.79 | `:103` |
| `DEATH_WAIT` | 2.85 | `:197` (initialised in decomp) |

### 1.4 Recoil motion (while `cState.recoiling`)

- `FixedUpdate` (`D:HeroController.cs:952-956`): `hero_state == no_input && !cState.transitioning &&
  cState.recoiling` ⇒ `AffectedByGravity(false); rb2d.velocity = recoilVector;` — velocity is
  **re-asserted every physics step**, so the knight is not affected by gravity or input during recoil.
- `Update` (`D:HeroController.cs:5168-5189`): `hero_state == no_input && cState.recoiling` ⇒
  `if (recoilTimer < (equippedCharm_4 ? RECOIL_DURATION_STAL : RECOIL_DURATION)) recoilTimer += Time.deltaTime;`
  else `CancelDamageRecoil(); … SetState(previous | airborne); fsm_thornCounter.SendEvent("THORN COUNTER");`
- `CancelDamageRecoil` (`:4095-4102`): `cState.recoiling=false; recoilTimer=0; ResetMotion();
  AffectedByGravity(true); SetDamageMode(FULL_DAMAGE);`
- Because the comparison is `<` on a float accumulated as repeated `+= 0.02f`, the sum reaches
  `0.2f` only after 11 ticks, so recoil lasts **11 update ticks** at dt=0.02 (verified §6.1).

`cState.recoiling` is also cleared by `Invulnerable` at `:3843` and by the non-`no_input` Update branch
at `:5195-5199` (`cState.recoiling=false; AffectedByGravity(true)`), by `HeroDash` (`:3523`) and by
`orig_DoAttack` (`:5516`).

### 1.5 Horizontal recoil (`RecoilLeft`/`RecoilRight`) — nail bouncing off an invincible target

Separate mechanism from damage recoil; triggered by `HealthManager.Invincible()` (`D:HealthManager.cs:359-370`)
and by `NailSlash.orig_OnTriggerEnter2D` against layer 11/19 (`D:NailSlash.cs:189-230`).
- `RecoilLeft()` (`D:HeroController.cs:2260-2271`): guarded by `!recoilingLeft && !recoilingRight &&
  !equippedCharm_14 && !controlReqlinquished`; `CancelDash(); recoilSteps=0; recoilingLeft=true;
  recoilLarge=false; rb2d.velocity = (−RECOIL_HOR_VELOCITY, vy)`. `RecoilRight` mirrors (`:2273-2284`).
- `RecoilLeftLong`/`RecoilRightLong` (`:2300-2312`, `:2286-2298`) additionally `ResetAttacks()` and use
  `RECOIL_HOR_VELOCITY_LONG`, `recoilLarge=true`.
- `RecoilDown()` (`:2314-2321`): `CancelJump();` unconditionally, then — only if
  **`rb2d.velocity.y > RECOIL_DOWN_VELOCITY && !controlReqlinquished`** (`:2317`) —
  `velocity.y = RECOIL_DOWN_VELOCITY` (i.e. clamp to 0).
- Sustained in `FixedUpdate` (`:990-1011`): each step, while `recoilingLeft`,
  `velocity.x = (vx > −num) ? −num : vx − num` with `num = recoilLarge ? 16.0 : 3.75`; mirrored for right.
- Terminated in `FixedUpdate` (`:912-922`): `if (recoilSteps <= RECOIL_HOR_STEPS) recoilSteps++; else CancelRecoilHorizontal();`
  ⇒ 9 physics steps of horizontal recoil (`recoilSteps` 0…8 inclusive, cancel on the 10th entry).
- `P:equippedCharm_14 = false`.

### 1.6 Knight death

`Die()` coroutine (`D:HeroController.cs:3699-3744`): fires `OnDeath`, `disablePause=true`,
`rb2d.velocity=0`, `CancelRecoilHorizontal()`; in map zone `GODS_GLORY` (the GG boss scenes) it takes
the early branch (`:3717-3727`): `RelinquishControl(); StopAnimationControl(); AffectedByGravity(false);
playerData.isInvincible = true; ResetHardLandingTimer(); renderer.enabled=false;
heroDeathPrefab.SetActive(true); yield break;` — **no `cState.dead`, no `gm.PlayerDead`, no layer change**
on this path. (The non-GG path at `:3728-3743` sets `cState.dead`, `rb2d.isKinematic=true`,
`HeroBox.inactive=true`, `gameObject.layer=2`, and schedules `gm.PlayerDead(DEATH_WAIT)`.)
The env ends the episode on `PlayerData.instance.health <= 0` instead (`O:Environment/TrainingEnv.cs:622, 656-677`).

---

## 2. Knight → enemy

### 2.1 Attack input → slash activation

- Input: `ActionDecoder.ApplyAction` `action[2]==0 → shim.AttackTap()` (`O:Game/ProxyController.cs:390`),
  which checks `HeroController.CanAttack()` by reflection (`O:Game/ProxyController.cs:119-120, 178-183`)
  and forces a release-then-press.
- `LookForQueueInput()` (`D:HeroController.cs:3330-3424`), called from `orig_Update` at `:5279`:
  - `:3377-3388` `if (attack.WasPressed) { if (CanAttack()) DoAttack(); else { attackQueueSteps=0; attackQueuing=true; } }`
  - `:3419-3422` `if (attack.IsPressed && attackQueueSteps <= ATTACK_QUEUE_STEPS && CanAttack() && attackQueuing) DoAttack();`
    `H:ATTACK_QUEUE_STEPS = 5`.
- `CanAttack()` (`D:HeroController.cs:4771-4778`): `attack_cooldown <= 0 && !cState.attacking &&
  !cState.dashing && !cState.dead && !cState.hazardDeath && !cState.hazardRespawning &&
  !controlReqlinquished && hero_state ∉ {no_input, hard_landing, dash_landing}`.
- `DoAttack()` → `orig_DoAttack()` (`D:HeroController.cs:3505-3509`, `:5513-5548`):
  `ResetLook(); cState.recoiling = false;`
  `attack_cooldown = equippedCharm_32 ? ATTACK_COOLDOWN_TIME_CH : ATTACK_COOLDOWN_TIME;`
  then direction select on `vertical_input`: `> ε → Attack(upward)`, `< −ε → Attack(downward)` unless
  `hero_state ∈ {idle, running}` in which case `Attack(normal)`, else `Attack(normal)`; each followed by
  `StartCoroutine(CheckForTerrainThunk(dir))`.
- `Attack(AttackDirection)` (`D:HeroController.cs:1322-1506`):
  - `if (Time.timeSinceLevelLoad − altAttackTime > ALT_ATTACK_RESET) cState.altAttack = false;` (`:1325-1328`)
  - `cState.attacking = true;`
  - `attackDuration = equippedCharm_32 ? ATTACK_DURATION_CH : ATTACK_DURATION;` (`:1330-1337`)
  - slash selection (`:1338-1463`): `cState.wallSliding → wallSlash`; `normal` alternates
    `normalSlash`/`alternateSlash` on `cState.altAttack` (toggling it); `upward → upSlash` + `cState.upAttacking = true`;
    `downward → downSlash` + `cState.downAttacking = true`. Grubberfly's Elegy beams gated on
    `equippedCharm_35` (`P:` false).
  - slash FSM `direction` float (`:1465-1495`): wallSliding → 180 if facingRight else 0;
    normal → 0 if facingRight else 180; upward → 90; downward → 270.
  - `altAttackTime = Time.timeSinceLevelLoad;` then `if (cState.attacking) slashComponent.StartSlash();` (`:1496-1505`).
- Attack lifetime in `orig_Update` (`D:HeroController.cs:5200-5208`, evaluated **before** `LookForQueueInput`):
  `if (cState.attacking && !cState.dashing) { attack_time += Time.deltaTime; if (attack_time >= attackDuration) { ResetAttacks(); animCtrl.StopAttack(); } }`.
- Cooldown decrement `orig_Update` `:5357-5360`: `if (attack_cooldown > 0) attack_cooldown -= Time.deltaTime;`
  (also **after** `LookForQueueInput`, hence a fresh attack is already one tick down on the frame it starts).
- `ResetAttacks()` (`:4111-4119`): clears `nailCharging`, `nailChargeTimer`, `attacking`, `upAttacking`,
  `downAttacking`, `attack_time = 0`.
- `FixedUpdate` `:974-989`: inside the `!cState.backDashing && !cState.dashing` block (`:974`),
  after `Move(move_input)` (`:976`), the guard is
  **`(!cState.attacking || !(attack_time < ATTACK_RECOVERY_TIME)) && !cState.wallSliding && !wallLocked`**
  (`:977`); inside it, `move_input > 0 && !cState.facingRight` or `move_input < 0 && cState.facingRight`
  ⇒ `FlipSprite(); CancelAttack();` (`:979-988`). So a mid-swing direction change kills the swing,
  but only once `attack_time >= ATTACK_RECOVERY_TIME` (0.1), and never while wall-sliding or
  wall-locked.

Attack constants (`H:`; declared `D:HeroController.cs:107-117`):
`ATTACK_DURATION = 0.35`, `ATTACK_DURATION_CH = 0.28`, `ALT_ATTACK_RESET = 0.5`,
`ATTACK_RECOVERY_TIME = 0.1`, `ATTACK_COOLDOWN_TIME = 0.41`, `ATTACK_COOLDOWN_TIME_CH = 0.25`.
`P:equippedCharm_32 = true` (the save has Quick Slash) ⇒ **0.28 / 0.25 are the live values**.

### 2.2 `NailSlash` collider window

`NailSlash` (`D:NailSlash.cs`), one instance per slash GameObject:
- `Awake` (`:44-62`): caches `poly = GetComponent<PolygonCollider2D>()`,
  `clashTinkPoly = transform.Find("Clash Tink").GetComponent<PolygonCollider2D>()`, and sets both
  `poly.enabled = false`, `mesh.enabled = false`.
- `StartSlash()` (`:64-101`): reads `slashAngle = slashFsm.FsmVariables.FindFsmFloat("direction").Value`;
  scales by charm (`mantis && longnail → 1.4`, `mantis → 1.25`, `longnail → 1.15`, else `scale`);
  plays `animName` (+`" M"` for mantis, +`" F"` for fury); `anim.PlayFromFrame(0)`;
  **`stepCounter = 0; polyCounter = 0; poly.enabled = false; clashTinkPoly.enabled = false;
  animCompleted = false; slashing = true; mesh.enabled = true;`**
  Live flag values, `S:…components[NailSlash]`:

  | slash | `animName` | `scale` | `longnail` | `mantis` | `fury` |
  |---|---|---|---|---|---|
  | `Knight/Attacks/Slash` | `SlashEffect` | (1.601078, 1.645244, 0) | true | true | false |
  | `Knight/Attacks/AltSlash` | `SlashEffectAlt` | (1.25697, 1.422434, 0) | true | true | false |
  | `Knight/Attacks/UpSlash` | `UpSlashEffect` | (1.15, 1.4, 1) | true | true | false |
  | `Knight/Attacks/DownSlash` | `DownSlashEffect` | (1.125, 1.28002, 1) | true | true | false |
  | `Knight/Attacks/WallSlash` | `SlashEffect` | (−1.62, 1.645244, 1) | false | false | false |

  So the four non-wall slashes take the `mantis && longnail` branch (`D:NailSlash.cs:68-71`):
  `localScale = (scale.x × 1.4, scale.y × 1.4, scale.z)` and `anim.Play(animName + " M")`; WallSlash
  takes the plain branch (`:83-87`). `fury = false` everywhere, so the `animName + " F"` override
  (`:88-91`) never fires at full health. Which charm sets which flag is written outside `NailSlash`
  and is not reachable in the decomp, but the port does not need it: the live values are dumped, and
  `P:equippedCharm_13 = true` / `P:equippedCharm_18 = true` are consistent with both being on. The
  flags affect hitbox size and clip name, not damage.
- `FixedUpdate()` (`:103-127`) — the whole timing model:
```
if (slashing) {
  if (stepCounter == 1)                      { poly.enabled = true;  clashTinkPoly.enabled = true; }
  if (stepCounter >= 5 && polyCounter > 0)   { poly.enabled = false; clashTinkPoly.enabled = false; }
  if (animCompleted && polyCounter > 1)      CancelAttack();
  if (poly.enabled) polyCounter++;
  stepCounter++;
}
```
  ⇒ the collider is enabled on the **2nd** `NailSlash.FixedUpdate` after `StartSlash` and disabled on
  the 6th (`stepCounter` 5), i.e. an active window of **4 `FixedUpdate` calls**. That equals 4 physics
  steps (0.08 s at `fixedDeltaTime` 0.02) only if Unity calls `FixedUpdate` exactly once per physics
  step — an engine rule no source in `analysis/` states (Q-dmg-11). The trace is consistent with it:
  `fixed_count` (incremented in `RecorderBehaviour.FixedUpdate`, `O:Oracle/TraceRecorder.cs:277-281`)
  advances by 0 or 1 per rendered frame and never by more (`analysis/specs/frame-order.md` §3.2).
- `OnTriggerEnter2D` and `OnTriggerStay2D` (`:129-133`, `:150-153`) both run `orig_OnTriggerEnter2D`,
  which handles **bounce/recoil only** (`:183-276`): `slashAngle==0 → RecoilLeft/RecoilLeftLong`,
  `180 → RecoilRight/…`, `90 → RecoilDown`, `270 → Bounce/BounceHigh/ShroomBounce` against layers
  11 (ENEMIES) / 19 (INTERACTIVE_OBJECT) / 17 (HERO_ATTACK), gated on `NonBouncer`.
  **`NailSlash` deals no damage** — damage is the `damages_enemy` FSM on the same GameObject.
- `CancelAttack()` (`:175-181`): `slashing=false; poly.enabled=false; clashTinkPoly.enabled=false; mesh.enabled=false;`
- `Disable` is installed as `anim.AnimationCompleted` and sets `animCompleted = true` (`:155-158`).

### 2.3 The `damages_enemy` FSM (data)

`F:Knight/Attacks/Slash::damages_enemy` (template `damages_enemy`; the same template is on AltSlash,
UpSlash, DownSlash, WallSlash, Great Slash, Dash Slash, Cyclone Hit L/R, Sharp Shadow, SuperDash Damage,
all spell hit objects and the Thorn Hit objects).

States:
- `[Idle]` — `Collision2dEvent OnCollisionEnter2D → storeCollider=$Collider`,
  `Trigger2dEvent OnTriggerEnter2D → $Collider`,
  `Trigger2dEvent OnTriggerStay2D collideLayer=Enemies → $Collider`; transition `HIT → Send Event`.
  ⇒ **re-fires on every `OnTriggerStay2D` dispatch while overlapping an `Enemies`-layer collider.**
  How often that is — once per physics step is the assumption — is Q-dmg-11; §6.3 measures the
  resulting cadence as one `HIT` event per `fixed_count` increment.
- `[Send Event]` — `IntCompare($damageDealt, 0)`; `GetLayer($Collider) → $Layer`;
  `IntCompare($Layer, 20)`; `IntCompare($Layer, 9)`; `CheckSendEventLimit($Collider)`;
  `GetName($Collider) → $Name`; `CompareNames($Name, $Ignore Names)` where
  `Ignore Names = ["_Props","_Scenery","_Enemies","Chunk"]`;
  `SendEventByName($Collider, "TAKE DAMAGE")`;
  **`TakeDamage Target=$Collider AttackType=$attackType CircleDirection=$circleDirection
  DamageDealt=$damageDealt Direction=$direction IgnoreInvulnerable=$Ignore Invuln
  MagnitudeMultiplier=$magnitudeMult MoveAngle=$Move Angle MoveDirection=$moveDirection
  Multiplier=$Multiplier SpecialType=$Special Type`**; then `SendEvent(self)` → `Parent`.
- `[Parent]`, `[Grandparent]` — repeat the same sequence on `GetParent($Collider)` and its parent,
  guarded by `GameObjectIsNull`. ⇒ the FSM performs its own 3-level ancestor walk, functionally the
  same shape as `HitTaker.Hit`'s `recursionDepth = 3` (`D:HitTaker.cs:5, 14-22`).

`CheckSendEventLimit.OnEnter` (`D:CheckSendEventLimit.cs:21-36`; the file is 37 lines) looks up
`LimitSendEvents` on the FSM **owner** and fires `trueEvent` / `falseEvent` from `component.Add(...)`.
`Add(obj)` appends and returns true on first sight, false if `obj` is already in `sentList`
(`D:LimitSendEvents.cs:33-41`). `sentList` is cleared in `OnEnable` (`:12-15`) and in `Update`
(`:17-31`), but the `Update` clear is **gated**: when `monitorCollider` is non-null it returns early
unless `monitorCollider.enabled` changed since the previous frame (`:19-26`), so the list survives
across frames while the collider's enabled state is stable. The unconditional "clear when non-empty"
(`:27-30`) is reached only when `monitorCollider` is null.
Every knight slash sets `monitorCollider` to its own `PolygonCollider2D` —
`S:Knight/Attacks/Slash…components[LimitSendEvents].monitorCollider = PolygonCollider2D "Slash"`, and
likewise for `AltSlash`, `UpSlash`, `DownSlash`, `WallSlash` — so one swing can send `TAKE DAMAGE` to
a given target at most once per enable→disable cycle of that slash's collider, which is exactly the
4-step window of §2.2.

The `TakeDamage` FSM action (`D:HutongGames.PlayMaker.Actions/TakeDamage.cs:78-97`) builds
`HitInstance { Source = base.Owner, AttackType = (AttackTypes)AttackType.Value, …,
Multiplier = Multiplier.IsNone ? 1f : Multiplier.Value, IsExtraDamage = false }`, then passes it
through **`hit = ModHooks.OnHitInstanceBeforeHit(base.Fsm, hit)`** (`:95`) before
`HitTaker.Hit(Target.Value, hit)` (`:96`) and `Finish()` (`:97`). The oracle registers nothing on
`OnHitInstanceBeforeHit` — `HookDamage` subscribes only `AfterTakeDamageHook`,
`On.HealthManager.TakeDamage` and `On.HeroController.TakeDamage`
(`O:Environment/TrainingEnv.cs:1142-1147`) — so it is identity for the sim, but it is the one place
an outgoing hit can be rewritten.

`HitTaker.Hit` (`D:HitTaker.cs:7-23`): walks `target.transform` up to `recursionDepth = 3` levels,
calling `GetComponent<IHitResponder>()?.Hit(damageInstance)` at each level.

`DamageEnemies` (`D:DamageEnemies.cs`) is the **component** equivalent, not used by the knight:
`OnCollisionEnter2D → DoDamage(collision.gameObject)`; `OnTriggerEnter2D` adds the collider to
`enteredColliders` unless its layer ∈ {20, 9, 26, 31} or it is tagged `Geo` (`:49-59`);
`OnTriggerExit2D` removes it; `OnDisable` clears; `FixedUpdate` re-`DoDamage`s every retained collider
each physics step (`:74-88`); `DoDamage` requires `damageDealt > 0`, sends `"TAKE DAMAGE"` to the target
and calls `HitTaker.Hit` with a `HitInstance` built from the component fields (`:90-111`).
**No `DamageEnemies` component exists in this scene**: 0 matches across all 1251 loaded-scene
colliders in `scene.json` (and 0 in `bosses.json` / `hero.json` / `physics.json`). Caveat: `scene.json`
is enumerated from `Collider2D`s, so a `DamageEnemies` on a GameObject with no `Collider2D` would be
missed — but such a component could never fire, since every one of its entry points is a physics
callback (`D:DamageEnemies.cs:44, 49, 61`). The FSM path is the only knight→enemy damage path here.

### 2.4 Knight attack damage numbers (data)

`F:Knight/Attacks::Set Slash Damage` computes the shared nail number once and pushes it into each
`damages_enemy`:
- `[Get Damage]`: `GetNailDamage → $Nail Damage`. `D:GetNailDamage.cs:20-24`:
  `BossSequenceController.BoundNailDamage`-clamped `playerData.nailDamage` inside a boss sequence,
  else raw `playerData.nailDamage`. `P:nailDamage = 21`.
- `[Glass Attack Modifier]`: `PlayerDataBoolTrueAndFalse(trueBool=equippedCharm_25, falseBool=brokenCharm_25)`
  then `$Damage Float = $Nail Damage; ×1.5; ConvertFloatToInt rounding=Nearest → $Nail Damage`.
  `P:equippedCharm_25 = true`, `P:brokenCharm_25 = false` ⇒ `round(21×1.5) = round(31.5) = 32`.
- `[Set Beam Damage]`: `$Beam = round($Nail × 0.5)`, `IntClamp 1..90`, `SetPlayerDataInt beamDamage`.
  ⇒ `P:beamDamage = 16`.
- `[Set Damage]`: writes `$Nail Damage` into `damages_enemy.damageDealt` of
  DownSlash, AltSlash, Slash, UpSlash, WallSlash, Cyclone Hit L, Cyclone Hit R; then
  `IntOperator $Nail Damage × 2` and writes the doubled value into Dash Slash and Great Slash.
- Runtime confirmation, `F:` `damages_enemy.damageDealt` at SceneReady: Slash/AltSlash/UpSlash/DownSlash/WallSlash/Cyclone Hit L/Hit R = **32**; Great Slash / Dash Slash = **64**.

`nailart_damage` **overrides** `damageDealt` when the art object activates
(`F:Knight/Attacks/Cyclone Slash/Hits/Hit R::nailart_damage`):
- `[Init]`: `GetNailDamage → $nailDamage` (raw 21 — **Fragile Strength is not applied here**);
  `$Damage Float = $nailDamage × $Multiplier`.
- `[Fury?]`: `BoolTest($Fury)` → `$Damage Float × 1.75`.
- `[Set]`: `ConvertFloatToInt rounding=Nearest → $nailDamage`;
  `SetFsmInt(OWNER, "damages_enemy", "damageDealt", $nailDamage)`.
- `Multiplier`: Cyclone Hit L/R = **1.25** ⇒ `26.25 → 26` (trace-confirmed, §6.3);
  Great Slash = **2.5**, Dash Slash = **2.5** ⇒ `52.5 → ` **UNKNOWN**, 52 or 53 — see below and
  Q-dmg-3.

Rounding: `ConvertFloatToInt rounding=Nearest` calls `Mathf.RoundToInt(floatVariable.Value)`
(`D:HutongGames.PlayMaker.Actions/ConvertFloatToInt.cs:52-58`). **`Mathf` is not in
`analysis/decomp/`** (only Assembly-CSharp, Assembly-CSharp-firstpass and PlayMaker were decompiled),
so its behaviour at an exact `x.5` tie is UNKNOWN and must not be assumed. The two trace-confirmed
cases do not discriminate: `21×1.5 = 31.5` is a tie whose candidates are 31 and 32, and half-to-even,
half-away-from-zero and half-up all yield 32; `21×1.25 = 26.25` is not a tie at all. `21×2.5 = 52.5`
**is** a tie whose candidates are 52 (half-to-even) and 53 (half-away-from-zero / half-up), so Great
Slash and Dash Slash damage cannot be stated from the sources — Q-dmg-3.

Per-emitter `damages_enemy` variables at SceneReady (`F:`; `attackType` values are `AttackTypes`
indices, `D:AttackTypes.cs`: 0 Nail, 1 Generic, 2 Spell, 3 Acid, 4 Splatter, 5 RuinsWater, 6 SharpShadow, 7 NailBeam):

| owner | damageDealt | attackType | direction | magnitudeMult | Multiplier | Ignore Invuln |
|---|---|---|---|---|---|---|
| `Knight/Attacks/Slash` | 32 | 0 | 0 (set per swing) | 1.0 | 1.0 | false |
| `Knight/Attacks/AltSlash` | 32 | 0 | ↑ | 1.0 | 1.0 | false |
| `Knight/Attacks/UpSlash` | 32 | 0 | ↑ | 1.0 | 1.0 | false |
| `Knight/Attacks/DownSlash` | 32 | 0 | ↑ | 1.0 | 1.0 | false |
| `Knight/Attacks/WallSlash` | 32 | 0 | ↑ | 1.0 | 1.0 | false |
| `Knight/Attacks/Cyclone Slash/Hits/Hit L` | 32→26 | 0 | 180 | 2.0 | 1.0 | false |
| `Knight/Attacks/Cyclone Slash/Hits/Hit R` | 32→26 | 0 | 0 | 2.0 | 1.0 | false |
| `Knight/Attacks/Great Slash` | 64→52 or 53 (Q-dmg-3) | 0 | 0 | 1.5 | 1.0 | false |
| `Knight/Attacks/Dash Slash` | 64→52 or 53 (Q-dmg-3) | 0 | 0 | 1.5 | 1.0 | false |
| `Knight/Attacks/Sharp Shadow` | 21 | 6 | 0 | 0.0 | 1.0 | false |
| `Knight/SuperDash Damage` | 10 | 1 | 0 | 1.5 | 1.0 | false |
| `Knight/Effects/SD Burst` | 10 | 1 | 0 | 2.0 | 1.0 | false |
| `Knight/Charm Effects/Thorn Hit/Hit {L,R,U,D}` | 5 | 1 | 180/0/90/270 | 1.5 | 1.0 | false |

### 2.5 `HealthManager.Hit` → `TakeDamage`

`IHitResponder.Hit` implementation, `D:HealthManager.cs:332-347`:
```
if (!isDead && !(evasionByHitRemaining > 0f) && hitInstance.DamageDealt > 0) {
    FSMUtility.SendEventToGameObject(hitInstance.Source, "DEALT DAMAGE");
    int card = DirectionUtils.GetCardinalDirection(hitInstance.GetActualDirection(transform));
    if (IsBlockingByDirection(card, hitInstance.AttackType)) Invincible(hitInstance);
    else TakeDamage(hitInstance);
}
```
- `HitInstance.GetActualDirection(target)` (`D:HitInstance.cs:31-39`): if `Source != null && target != null
  && CircleDirection`, `atan2(target.position − Source.position) * 57.29578`; else `Direction`.
- `DirectionUtils.GetCardinalDirection(deg)` = `((round(deg/90) % 4) + 4) % 4`
  (`D:DirectionUtils.cs:13-21`), constants `Right=0, Up=1, Left=2, Down=3` (`D:DirectionUtils.cs:5-11`).
- `IsBlockingByDirection(card, attackType)` (`D:HealthManager.cs:705-764`):
  returns false if `(attackType == Spell || attackType == SharpShadow) && gameObject.CompareTag("Spell Vulnerable")`;
  false if `!invincible`; true if `invincibleFromDirection == 0`; otherwise a per-cardinal switch on
  `invincibleFromDirection` (see source; `B:invincibleFromDirection = 0` for Hornet).
- `Invincible(hitInstance)` (`:349-428`): sets `directionOfLastAttack`; sends `"BLOCKED HIT"` to self and
  `"HIT LANDED"` to the source; unless `GetComponent<DontClinkGates>()`, sends `"HIT"` to self and,
  when `!preventInvincibleEffect`: for `AttackTypes.Nail`, `card==0 → HeroController.instance.RecoilLeft()`,
  `card==2 → RecoilRight()`; `GameManager.instance.FreezeMoment(1)` (no-op'd by the mod);
  `cameraShakeFSM.SendEvent("EnemyKillShake")` — which drives `ShakePositionV2` and therefore draws
  `UnityEngine.Random`, see Q-dmg-2; spawns `blockHitPrefab` at a box-collider-derived point.
  **Always ends with `evasionByHitRemaining = 0.15f`** (`:427`).
  `B:preventInvincibleEffect = true` for Hornet ⇒ only the events and the 0.15 s evasion apply.
- `evasionByHitRemaining` is initialised to `−1f` in `Start` (`:299`) and decremented in `Update` by
  `Time.deltaTime` unconditionally (`:327-330`) — an `Update` timer, i.e. one tick per rendered frame
  (`analysis/specs/frame-order.md` §2, measured: one `HC_UPDATE_PRE`/`POST` pair per FRAME record),
  not one per physics step. It does not advance on the env's frozen inter-step frame, because
  `Time.deltaTime == 0` there is measured (`analysis/specs/frame-order.md` §3.2), not assumed.

`TakeDamage(HitInstance)` (`D:HealthManager.cs:430-517`), in order:
1. `if (hitInstance.AttackType == Acid && ignoreAcid) return;` (`B:ignoreAcid = false`).
2. `if (CheatManager.IsInstaKillEnabled) hitInstance.DamageDealt = 9999;` — off.
3. `int card = directionOfLastAttack = GetCardinalDirection(GetActualDirection(transform));`
4. Events: `"HIT"` → self, `"HIT LANDED"` → `hitInstance.Source`, `"TOOK DAMAGE"` → self,
   and `"HIT"` → `sendHitTo` if set (`B:sendHitTo = null`).
5. `if (recoil != null) recoil.RecoilByDirection(card, hitInstance.MagnitudeMultiplier);` (`:448-451`)
   — see §2.6. `B:recoil` = the `Recoil` component on `Boss Holder/Hornet Boss 1`.
6. Per-`AttackType` effects (`:452-494`):
   - `Nail` or `NailBeam`: if `AttackType == Nail && enemyType != 3 && enemyType != 6`
     ⇒ **`HeroController.instance.SoulGain()`** (`:457-460`). `B:enemyType = 1` ⇒ Hornet grants soul.
     Then spawns `strikeNailPrefab` and `slashImpactPrefab` at
     `(Source.position + transform.position)*0.5 + effectOrigin`, with
     `UnityEngine.Random.Range(340,380)` / `(70,110)` / `(250,290)` rotations — **RNG consumers**, Q-dmg-2.
   - `Generic` / `Spell` / `SharpShadow`: single prefab spawn, no RNG.
7. `if (hitEffectReceiver != null && AttackType != RuinsWater) hitEffectReceiver.RecieveHitEffect(GetActualDirection(transform));`
   (`B:hitEffectReceiver` = `EnemyHitEffectsUninfected`, which drives the `SpriteFlash` component on
   `Boss Holder/Hornet Boss 1` — `B:components[0].components` lists both). `SpriteFlash`
   (`D:SpriteFlash.cs`) only animates a material property block; it touches no HP, collider, FSM
   variable or transform (`D:SpriteFlash.cs:34, 61, 74, 86` — every write is
   `rend.SetPropertyBlock(block)`), so it is **out of scope for the sim**.
   Same for `strikeNailPrefab` / `slashImpactPrefab` / `blockHitPrefab`, whose only sim-relevant
   consequence is their `UnityEngine.Random` draws (Q-dmg-2).
8. **`int num2 = Mathf.RoundToInt(hitInstance.DamageDealt * hitInstance.Multiplier);`** (`:499`)
   `if (damageOverride) num2 = 1;` (`B:damageOverride = false`).
9. **`hp = Mathf.Max(hp − num2, −50);`** (`:504`)
10. `if (hp > 0) { NonFatalHit(hitInstance.IgnoreInvulnerable); if (stunControlFSM) stunControlFSM.SendEvent("STUN DAMAGE"); }`
    `else Die(GetActualDirection(transform), hitInstance.AttackType, hitInstance.IgnoreInvulnerable);` (`:505-516`)
11. `NonFatalHit(bool ignoreEvasion)` (`:519-536`): returns immediately if `ignoreEvasion`;
    if `hasAlternateHitAnimation` plays `alternateHitAnimation`; **else `evasionByHitRemaining = 0.2f`**.
    `B:hasAlternateHitAnimation = false` ⇒ Hornet gets the 0.2 s evasion window.

`SoulGain()` (`D:HeroController.cs:2081-2116`): `if (MPCharge < maxMP) num = 11` (+3 for `equippedCharm_20`,
+8 for `equippedCharm_21`) `else num = 6` (+2 / +6); `num = ModHooks.OnSoulGain(num)`;
`playerData.AddMPCharge(num)`. `P:equippedCharm_20 = false`, `P:equippedCharm_21 = false`,
`P:maxMP = 99` ⇒ **+11 soul per nail hit** below 99.

`ApplyExtraDamage(int)` (`:538-546`) is a separate entry (spore/dung, `IExtraDamageable`):
`hp = Max(hp − amount, 0)`; `Die(null, Generic, ignoreEvasion:true)` at ≤0. It does **not** go through
`TakeDamage`, so it is invisible to the oracle's ENEMY_DAMAGE hook. `ExtraDamageable.RecieveExtraDamage`
short-circuits on `damagedThisFrame` and on `IsInvincible` unless the object is tagged `Spell Vulnerable`
(`D:ExtraDamageable.cs:44-56`); `B:tag = "Untagged"`.

### 2.6 `Recoil` (enemy knockback)

`D:Recoil.cs`. Hornet's live serialized values,
`S:Boss Holder/Hornet Boss 1…components[Recoil]`:

| field | value |
|---|---|
| `freezeInPlace` | **false** |
| `stopVelocityXWhenRecoilingUp` | **false** |
| `preventRecoilUp` | **true** |
| `recoilSpeedBase` | **15.0** |
| `recoilDuration` | **0.15** |
| `skipFreezingByController` | false |
| `state` | `Ready` (0) |

`preventRecoilUp = true` is load-bearing: `RecoilByDirection` skips its entire body when
`attackDirection == 1` (Up) (`D:Recoil.cs:122`), so an up-directed hit on Hornet produces **no**
knockback and **no** `"HIT UP"` event. `recoilDuration = 0.15`, not the 0.5 of `Reset()`.
The `Reset()` block (`D:Recoil.cs:77-84`: `freezeInPlace=false, stopVelocityXWhenRecoilingUp=true,
recoilDuration=0.5f, recoilSpeedBase=15f, preventRecoilUp=false`) is the **editor default** and
disagrees with the live object on three of five fields — do not port it (cf.
`analysis/open-questions.md` Q9).

`RecoilByDirection(int attackDirection, float attackMagnitude)` (`:112-152`):
```
if (state != Ready) return;
if (freezeInPlace) Freeze();
else if (attackDirection != 1 /*Up*/ || !preventRecoilUp) {
    state = Recoiling;
    recoilSpeed = recoilSpeedBase * attackMagnitude;
    recoilSweep = new Sweep(bodyCollider, attackDirection, 3);
    isRecoilSweeping = true;
    recoilTimeRemaining = recoilDuration;
    // events: 2 -> "RECOIL HORIZONTAL","HIT LEFT"; 0 -> "RECOIL HORIZONTAL","HIT RIGHT";
    //         3 -> "HIT DOWN"; 1 -> "HIT UP"
    UpdatePhysics(0f);
}
```
`UpdatePhysics(dt)` (`:196-233`), called from `FixedUpdate` with `Time.fixedDeltaTime` (`:191-194`):
- `Frozen`: `body.velocity = 0`; `recoilTimeRemaining -= dt`; `<= 0 → CancelRecoil()`.
- `Recoiling`: if `isRecoilSweeping`, `recoilSweep.Check(transform.position, recoilSpeed*dt, layerMask 256, out clipped)`;
  a `true` return stops sweeping; `if (clipped > Mathf.Epsilon) transform.Translate(recoilSweep.Direction * clipped, Space.World);`
  then `recoilTimeRemaining -= dt`; `<= 0 → CancelRecoil()`.
  `SweepLayerMask = 256` = layer 8 (Terrain) (`D:Recoil.cs:47`).
`Sweep` (`D:Sweep.cs`, 57 lines) is fully in the decomp: the constructor
(`:3-15`) caches `CardinalDirection`, `Direction = (DirectionUtils.GetX(dir), GetY(dir))`,
`ColliderOffset = collider.offset ⊙ collider.transform.localScale`,
`ColliderExtents = collider.bounds.extents`, `SkinThickness` (default `0.1f`, `:17`) and `RayCount`
(`Recoil` passes 3, `D:Recoil.cs:130`). `Check(offset, distance, layerMask, out clippedDistance)`
(`:27-56`) returns `false` with `clippedDistance = 0` when `distance <= 0`; otherwise it casts
`RayCount` rays evenly spanning the collider's leading face — origin
`offset + ColliderOffset + (ColliderExtents ⊙ Direction) + (ColliderExtents ⊙ |Direction.yx|) × (2i/(RayCount−1) − 1) − Direction × SkinThickness`,
length `num + SkinThickness` where `num` starts at `distance` and is lowered to
`hit.distance − SkinThickness` on each nearer hit (`:37-53`). It sets `clippedDistance = num` and
returns `distance − num > Mathf.Epsilon`. So `isRecoilSweeping` is cleared the first step the sweep
is obstructed, and `Recoil` then keeps translating with no further terrain query.
- **Recoil moves the transform directly, not the rigidbody.**
`Freeze()` (`:166-189`): if `skipFreezingByController` fires `OnHandleFreeze` and stays `Ready`; else
`state = Frozen`, `body.velocity = 0`, sends `"FREEZE IN PLACE"` to a `Climber Control` FSM,
`recoilTimeRemaining = recoilDuration`, `UpdatePhysics(0)`.
`OnEnable → CancelRecoil()` (`:92-95`); `IsRecoiling` is `state ∈ {Recoiling, Frozen}` (`:61-71`).

---

## 3. Spells, nail arts, focus

Everything in this section is **FSM data** unless marked *code*.

### 3.1 What the save grants (`P:`)

`nailDamage = 21`, `beamDamage = 16`, `health = maxHealth = maxHealthBase = 9`, `healthBlue = 0`,
`MPCharge = 0`, `maxMP = 99`, `MPReserve = 0`, `MPReserveMax = 99`, `focusMP_amount = 33`,
`fireballLevel = 2`, `quakeLevel = 2`, `screamLevel = 2`,
`hasDash/hasWalljump/hasDoubleJump/hasSuperDash/hasDreamNail/hasAcidArmour/hasNailArt/hasCyclone/hasUpwardSlash/hasDashSlash = true`,
`equippedCharms = [13, 18, 25, 32, 36]`, `charmSlotsFilled = 11`, `overcharmed = false`,
`royalCharmState = 4`, `blockerHits = 4`, `soulLimited = false`.
All other `equippedCharm_*` queried above are false (`_3,_4,_5,_6,_7,_12,_14,_15,_19,_20,_21,_26,_27,_31,_34,_35,_38`).

### 3.2 Spell cast gating (*code* + data)

- `CanCast()` *code* (`D:HeroController.cs:2973-2980`): `!gm.isPaused && !cState.dashing &&
  hero_state != no_input && !cState.backDashing && (!cState.attacking || attack_time >= ATTACK_RECOVERY_TIME)
  && !cState.recoiling && !cState.recoilFrozen && !cState.transitioning && !cState.hazardDeath &&
  !cState.hazardRespawning && CanInput() && preventCastByDialogueEndTimer <= 0f`.
- `F:Knight::Spell Control[Can Cast?]`: `CallMethodProper(HeroController.CanCast)`,
  `GetPlayerDataInt MPCharge → $MP`, `IntCompare($MP, $MP Cost)`, `BoolTest($Return Bool)`;
  **`MP Cost = 33`** (`F:Knight::Spell Control` var).
- **`[Spell Choice]` spell selection**, from the dump's serialised `FsmEvent` fields. The direction is
  latched one state earlier: `[Button Down]`'s `ListenForUp` / `ListenForDown` run with
  `stateEntryOnly = true` and send no events — they only write `isPressedBool` into `$Pressed Up` /
  `$Pressed Down` (`D:HutongGames.PlayMaker.Actions/ListenForUp.cs:62-76`). `[Spell Choice]` then
  reads those latches:

  | idx | action | field → event |
  |---|---|---|
  | 0 | `BoolTest($Pressed Up)` | `isTrue = SCREAM` |
  | 1 | `BoolTest($Pressed Down)` | `isTrue = QUAKE`, `isFalse = FIREBALL` |
  | 2 | `ListenForUp` (**disabled**) | `isPressed = SCREAM` |
  | 3 | `ListenForDown` (**disabled**) | `isPressed = QUAKE`, `isNotPressed = FIREBALL` |

  Transitions: `FIREBALL → [Has Fireball?]`, `QUAKE → [Has Quake?]`, `SCREAM → [Has Scream?]`.
  ⇒ **up → Howling Wraiths / Abyss Shriek; down → Desolate Dive / Descending Dark; neither →
  Vengeful Spirit / Shade Soul.** With both latched, action order decides, and whether action 1 still
  runs after action 0 has raised `SCREAM` is the FSM's action-loop/switch semantics
  (`analysis/specs/fsm-runtime.md`), not something this spec establishes.
- `[Button Down]` / `[Held Down]`: `Wait time = $Button Down Time = 0.25` decides tap (cast) vs
  hold (focus): `BUTTON UP → Can Cast?`, `FOCUS START → Can Focus?`.

### 3.3 Spell damage (data)

`fireballLevel = 2` ⇒ `[Level Check]` picks `[Fireball 2]`, which
`SpawnObjectFromGlobalPool("Fireball2 Top")`; likewise `quakeLevel = 2` and `screamLevel = 2` pick the
level-2 variants. Each spell hit object's `Set Damage` / `Fireball Control[Set Damage]` writes a base
value, then a `PlayerDataBoolTest equippedCharm_19` (Shaman Stone) writes an upgraded value.
`P:equippedCharm_19 = false` ⇒ **base column applies**.

| spell | emitter | base | Shaman Stone | attackType | magnitudeMult |
|---|---|---|---|---|---|
| Vengeful Spirit (lvl 1) | `_GameManager/GlobalPool/Fireball(Clone)` | 15 | 20 | 2 Spell | 1.5 |
| Shade Soul (lvl 2) | `_GameManager/GlobalPool/Fireball2 Spiral(Clone)` | 30 | 40 | 2 Spell | 1.5 |
| Desolate Dive (lvl 1) | `Knight/Spells/Q Slam/Hit L`, `…/Hit R` | 20 | 30 | 2 Spell | 2.0 |
| Descending Dark (lvl 2) | `Knight/Spells/Q Slam 2/Hit R` | 30 | 50 | 2 Spell | 2.0 |
| Descending Dark (lvl 2) | `Knight/Spells/Q Slam 2/Hit L` | 35 | 50 | 2 Spell | 2.0 |
| dive fall damage | `Knight/Spells/Q Fall Damage` | 15 | 23 | 2 Spell | 0.0 |
| Howling Wraiths (lvl 1) | `Knight/Spells/Scr Heads/Hit {L,R,U}` | 13 | 20 | 2 Spell | 1.5 |
| Abyss Shriek (lvl 2) | `Knight/Spells/Scr Heads 2/Hit {L,R,U}` | 20 | 30 | 2 Spell | 1.5 |

(The `damageDealt` values sitting in the dump at SceneReady — 15 for Fireball, 25 for Fireball2 Spiral,
15 for Q Slam / Scr Heads — are the *serialized* values; `Set Damage` overwrites them at cast time. The
L/R asymmetry of Q Slam 2 is what the dump says; not a transcription error.)

Cast timing (data, `F:Knight::Spell Control`): `[Fireball Antic]` waits for the tk2d `ANIM END` event,
then `[Level Check] → [Fireball 1|2]` spawns the projectile, then `[Fireball Recoil]` sets
`gravityScale = 0`, `velocity = (Fireball Recoil Distance × localScale.x × 2, 0)` every frame
(`Fireball Recoil Distance = 1.0`) until `ANIM END`, then `[Spell End]` restores
`velocity = 0` and `gravityScale = $Hero Gravity = 0.79`. *code* counterpart:
`H:CAST_TIME = 0.4`, `H:CAST_RECOIL_TIME = 0.1`, `H:CAST_RECOIL_VELOCITY = 10.0` (used by
`cState.casting`/`castRecoiling` paths, not by this FSM).
Frame-exact antic length depends on the tk2d clip → Q-dmg-4.

### 3.4 Focus (*code* + data)

- `CanFocus()` *code* (`D:HeroController.cs:2982-2989`): `!gm.isPaused && hero_state != no_input &&
  !cState.dashing && !cState.backDashing && (!cState.attacking || attack_time >= ATTACK_RECOVERY_TIME)
  && !cState.recoiling && cState.onGround && !cState.transitioning && !cState.recoilFrozen &&
  !cState.hazardDeath && !cState.hazardRespawning && CanInput()`.
- `F:Knight::Spell Control[Can Focus?]` additionally requires `MPCharge >= focusMP_amount`
  (`IntCompare($MP, $Focus MP amount)` with `$Focus MP amount = GetPlayerDataInt(focusMP_amount) = 33`).
- `[Focus Start]`: sets `cState.freezeCharge = true`, `cState.focusing = true`, zeroes velocity every
  frame, `Wait time = $Focus Start Time = 0.25` → `[Set Focus Speed]`.
- `[Set Focus Speed]`: `$Time Per MP Drain = $Time Per MP Drain UnCH = 0.027`; if `equippedCharm_7`
  (Quick Focus, `P:` false) `= $Time Per MP Drain CH = 0.018`.
- `[Focus]`: `CallMethodProper(HeroController.StartMPDrain, $Time Per MP Drain)`;
  exits on `BUTTON UP` / `LEFT GROUND` → `[Grace Check]` (`$Grace Time = 0.45`, `$Grace Timer` accumulates
  at 1/s while focusing) or on `FOCUS COMPLETED` → `[Spore Cloud] → [Set HP Amount] → [Focus Heal]`.
- `StartMPDrain(float time)` *code* (`D:HeroController.cs:1551-1555` → `orig_StartMPDrain` `:5097-5104`):
  `drainMP = true; drainMP_timer = 0; MP_drained = 0; drainMP_time = time;
  focusMP_amount = playerData.focusMP_amount;` then `focusMP_amount *= ModHooks.OnFocusCost()`.
- Drain loop *code* (`D:HeroController.cs:5280-5296`, in `orig_Update`):
  ```
  drainMP_timer += Time.deltaTime; drainMP_seconds += Time.deltaTime;
  while (drainMP_timer >= drainMP_time) {
      MP_drained += 1; drainMP_timer -= drainMP_time; TakeMP(1);
      gm.soulOrb_fsm.SendEvent("MP DRAIN");
      if (MP_drained == focusMP_amount) { MP_drained -= drainMP_time; proxyFSM.SendEvent("HeroCtrl-FocusCompleted"); }
  }
  ```
  ⇒ **33 soul at 0.027 s each = 0.891 s of drain**, plus `Focus Start Time = 0.25` before it begins.
  (`MP_drained -= drainMP_time` on line `:5292` subtracts a *time* from a *count*; transcribe as written.)
- `[Set HP Amount]`: `$Health Increase = 1` (a `[Deep Focus Speed]` branch on `equippedCharm_34`,
  `P:` false, would set 2). `[Focus Heal]`: `StopMPDrain()`, spawn `White Flash R`,
  `CallMethodProper(HeroController.AddHealth, $Health Increase)`, `Wait time = 0.2` → `[Full HP?]`,
  which loops back into `[Focus]` when `HP < Max HP` and `MPCharge >= focusMP_amount`.
- `AddHealth(int)` *code* (`D:HeroController.cs:2171-2175`): `playerData.AddHealth(amount);
  proxyFSM.SendEvent("HeroCtrl-Healed");`
- A hit while focusing: `TakeDamage` → `proxyFSM.SendEvent("HeroCtrl-HeroDamaged")` (`:1928`) →
  `F:Knight::Spell Control` global `HERO DAMAGED → Reset Cam Zoom` (cancels the focus).

### 3.5 Nail arts (*code* + data)

- Charge accumulation *code* (`D:HeroController.cs:5374-5416`, in `orig_Update`):
  `if (!gm.isPaused && attack.IsPressed && CanNailCharge()) { cState.nailCharging = true; nailChargeTimer += Time.deltaTime; }`
  else clears. `CanNailCharge()` (`:4780-4787`): `!cState.attacking && !controlReqlinquished &&
  !cState.recoiling && !cState.recoilingLeft && !cState.recoilingRight && playerData.hasNailArt`.
- `nailChargeTime = equippedCharm_26 ? NAIL_CHARGE_TIME_CHARM : NAIL_CHARGE_TIME_DEFAULT`
  (`D:HeroController.cs:832-839`, `:5474-5480`). `H:NAIL_CHARGE_TIME_DEFAULT = 1.35`,
  `H:NAIL_CHARGE_TIME_CHARM = 0.75`, `P:equippedCharm_26 = false` ⇒ **1.35 s**. `H:nailChargeTime = 1.35`.
- `CanNailArt()` *code* (`D:HeroController.cs:2991-2999`): `!cState.transitioning && hero_state != no_input
  && !cState.attacking && !cState.hazardDeath && !cState.hazardRespawning && nailChargeTimer >= nailChargeTime`;
  **it zeroes `nailChargeTimer` on both paths.**
- `F:Knight::Nail Arts`: `[Inactive]` `ListenForAttack` → `BUTTON UP → [Can Nail Art?]`, which calls
  `HeroController.GetState("dashing")` and `HeroController.CanNailArt()`. `[Take Control]` sends
  `RelinquishControlNotVelocity` + `StopAnimationControl` to the HeroController first.
- **`[Move Choice]` art selection**, read directly from the dump's `FsmEvent` fields (they serialise;
  only the `FsmEventTarget` *routing* is `__unserialized`, and its `target` enum survives as `Self`):

  | idx | action | field → event |
  |---|---|---|
  | 0 | `SendEvent` (**disabled**) | `sendEvent = CYCLONE` |
  | 1 | `ListenForDown` | `isPressed = CYCLONE`; `wasPressed` / `wasReleased` / `isNotPressed` null |
  | 2 | `ListenForUp` | `isPressed = CYCLONE`, `isNotPressed = GREAT SLASH`; `wasPressed` / `wasReleased` null |

  Transitions: `CYCLONE → [Has Cyclone?]` (gated on `hasCyclone`), `GREAT SLASH → [Has G Slash?]`
  (gated on `hasDashSlash`). `ListenFor*` sends each non-null `FsmEvent` whose input predicate holds
  (`D:HutongGames.PlayMaker.Actions/ListenForUp.cs:48-78`; `ListenForDown.cs` is the same file with
  `inputActions.up` → `inputActions.down`, verified by diff), and
  `Fsm.Event(null)` is a no-op (`analysis/decomp/PlayMaker/HutongGames.PlayMaker/Fsm.cs:2192-2198`),
  so an unset field means "send nothing". ⇒ **down held → Cyclone Slash; up held → Cyclone Slash;
  neither → Great Slash.** Both directions select Cyclone; Great Slash is the neutral art, and it is
  reached through the `isNotPressed` branch of `ListenForUp` rather than by an explicit event.
  Cyclone: `[Cyclone Spin]` ticks `$Timer` and `$Cyclone Timer` at 1/s, compares against **0.45** and
  **1.2**; `[Cyclone Extend]` subtracts 0.3 from `$Timer`, clamps `velocity.y ≥ −4`, `Wait 0.25`.
  Great Slash: `[G Slash]` `Wait time = 0.075` then `[Stop Move]`.
  Global `HERO DAMAGED / LEAVING SCENE / FSM CANCEL → [Cancel All]`, which deactivates
  Great Slash / Cyclone Slash / Dash Slash.

---

## 4. Boss health & phases (GG_Hornet_1)

### 4.1 HP

- `B:hp = 900`, `B:hpScale = { level1: 0, level2: 1250, level3: 1250 }`, `B:enemyType = 1`,
  `B:isDead = false`, `B:invincible = true` (at SceneReady), `B:invincibleFromDirection = 0`,
  `B:preventInvincibleEffect = true`, `B:hasSpecialDeath = false`, `B:deathReset = false`,
  `B:damageOverride = false`, `B:battleScene = null`, `B:sendHitTo = null`, `B:sendKilledToName = ""`,
  `B:smallGeoDrops = mediumGeoDrops = largeGeoDrops = 0`, `B:effectOrigin = (0,0,0)`.
- `Start()` (`D:HealthManager.cs:297-315`): `hp = hpScale.GetScaledHP(hp)` then
  `BossSceneController.ReportHealth(this, baseHP, hp)`.
  `HPScaleGG.GetScaledHP` (`:20-47`): only inside a boss scene, `BossLevel 0 → level1<=0 ? originalHP : level1`,
  `1 → level2`, `2 → level3`. With `level1 = 0` and forced tier 0 ⇒ **900**.
- `ReportHealth` (`D:BossSceneController.cs:302-333`) records `{baseHP, adjustedHP}` in
  `BossHealthLookup` for HMs in `Instance.bosses` (or `forceAdd`).

### 4.2 `invincible`

`HealthManager.IsInvincible` is a plain property over the serialized `invincible` bool
(`D:HealthManager.cs:226-236`). In GG_Hornet_1 exactly two FSM actions write it
(`SetInvincible`, `D:SetInvincible.cs:21-38` → `HealthManager.IsInvincible = value`):
- `F:Boss Holder/Hornet Boss 1::Control[GG Intro 1]` → `Invincible = true, InvincibleFromDirection = 0`
- `F:Boss Holder/Hornet Boss 1::Control[GG Reset]`  → `Invincible = false, InvincibleFromDirection = 0`

So Hornet is untouchable during the intro and touchable for the whole fight. `invincible` gates
`IsBlockingByDirection` (→ `Invincible()` clink instead of `TakeDamage`) and `ExtraDamageable`.
Measured in the ENTITY block of the R2 traces (`Hornet Boss 1`.`invincible` per FRAME record):
`T:r2_rand1.a` true on 42 / false on 438 FRAMEs, first false at frame 24768; `T:r2_move.a` 42 / 558,
first false at 24827; `T:r2_idle.a` 42 / 198, first false at 24984 — a constant 42-frame intro window
followed by `false` for the rest of the capture. `B:invincible = true` in the dump is that intro
window, because dumps run at `SceneReady` (§0).

### 4.3 Stagger (`Stun Control`)

`F:Boss Holder/Hornet Boss 1::Stun Control` (bound as `HealthManager.stunControlFSM` by the
`FsmName == "Stun Control" || "Stun"` scan in `Awake`, `D:HealthManager.cs:261-269`;
`B:stunControlFSM` confirms the binding). Variables:
`Combo Time = 2.0`, `Stun Combo = 6`, `Stun Hit Max = 10`, `Stuns Max = 5`, plus counters
`Combo Counter = 0`, `Hits Total = 0`, `Stuns Total = 0` and an unused `Decrement = 0`. Global
transitions
`STUN CONTROL STOP → [Stop]`, `STUN CONTROL RESET → [Reset]`.

- `[Init] → [Heavy Blow]`: `PlayerDataBoolTest equippedCharm_15` → `Stun Hit Max -= 1`, `Stun Combo -= 1`.
  `P:equippedCharm_15 = false` ⇒ thresholds stay 10 / 6. → `[Idle]`.
- `[Idle]` `STUN DAMAGE → [Max Check]` (the event `HealthManager.TakeDamage` sends at `:510` on any
  non-fatal hit).
- `[Max Check]`: `IntCompare(Hits Total, Stun Hit Max)` → `FINISHED → [In Combo]`, `STUN → [Stun]`.
- `[In Combo]`: `Combo Counter += 1`, `Hits Total += 1`, `IntCompare(Combo Counter, Stun Combo)`,
  `Wait time = $Combo Time (2.0)`. `TIME OUT → [Reset Counter]` (zeroes `Combo Counter` → `[Idle]`);
  `STUN DAMAGE → [Continue Combo]`; `STUN → [Stun]`.
- `[Continue Combo]`: `IntCompare(Hits Total, Stun Hit Max)` → `[In Combo]` or `[Stun]`.
- `[Stun]`: `IntCompare($Stuns Total, $Stuns Max)` with **both** `equal` and `greaterThan` bound to
  the event `MAX`; then `Stuns Total += 1`; `SendEventByName(OWNER, "STUN")`; `Combo Counter = 0`;
  `Hits Total = 0`; → `[Idle]`.
  **`MAX` is inert.** `MAX` is declared in `Stun Control`'s event list, but no state transition and no
  global transition anywhere in `analysis/fsm/GG_Hornet_1.json` targets it on this FSM (the sole `MAX`
  transition in the scene is on the unrelated
  `_GameCameras/HudCamera/Inventory/Inv/Inv_Items/Heart Pieces::Set Pieces[Init]`). So `Stuns Max = 5`
  caps nothing and `Stuns Total` is a write-only counter.
⇒ **stagger on 6 hits within a rolling 2 s combo window, or on the 10th hit overall since the last
stagger. There is no cap on the number of staggers.**

`F:Boss Holder/Hornet Boss 1::Control` has global `STUN → [Stun Start]`:
- `[Stun Start]`: idle box size/offset, `Sphere Ball` off, spawn `Stun Effect`,
  `SetFsmFloat(OWNER, "recoil", "Recoil per second", 15.0)`, box collider non-trigger,
  `gravityScale = $Gravity (1.5)`, face the knight, `Tk2dPlayAnimation "Stun Air"`,
  `velocity = ($Stun Air Speed (10.0) × localScale.x, 20.0)`, deactivate `Needle`, `Hit ADash`,
  `Hit GDash`, `Needle Tink` collider; `NextFrameEvent → [Stun Air]`.
- `[Stun Air]`: `CheckCollisionSideEnter` / `CheckCollisionSide` → `LAND → [Stun Land]`.
- `[Stun Land]`: `Tk2dPlayAnimation "Stun"`, `velocity = 0`, **`Wait time = 3.0`**;
  `FINISHED → [Stun Recover]`, **`TOOK DAMAGE → [Stun Recover]`** (a hit ends the stagger early).
- `[Stun Recover] → [Set Jump Only]`.
Also `[Idle]`/`[Run]` have `TOOK DAMAGE → [Dmg Response]`, which does
`SendRandomEvent(weights [0.30, 0.15, 0.15, 0.40])` → EVADE / JUMP / ATTACK / IDLE — an RNG consumer
on every landed hit.

### 4.4 Death

`HealthManager.Die(float? attackDirection, AttackTypes attackType, bool ignoreEvasion)`
(`D:HealthManager.cs:548-683`):
1. `if (isDead) return;`
2. `sprite.color = Color.white`; `SendEventToGameObject(self, "ZERO HP")`.
   *No FSM in GG_Hornet_1 has a `ZERO HP` transition* (0 matches) → Hornet's death is handled entirely
   by `EnemyDeathEffects` + the `OnDeath` subscribers.
3. Godfinder icon (`B:showGodfinderIcon = true`, `B:showGodFinderDelay = 7`), `unlockBossScene` bookkeeping.
4. **`if (hasSpecialDeath) { NonFatalHit(ignoreEvasion); return; }`** — the stagger-instead-of-death path
   used by False Knight et al. `B:hasSpecialDeath = false` for Hornet.
5. `isDead = true;` `if (damageHero != null) damageHero.damageDealt = 0;` (`:572-576`) — the corpse stops
   hurting the knight.
6. battle-scene counter, death audio snapshot, `sendKilledTo` — all null/no-op here.
7. Geo fling (`AttackTypes` switch, `:598-673`): all three drop counts are 0 for Hornet, but
   `FlingUtils.SpawnAndFling` is still called three times.
8. `enemyDeathEffects.RecieveDeathEvent(attackDirection, deathReset, attackType == Spell,
   attackType ∈ {RuinsWater, Acid})`; `doKillFreeze` is forced false for `RuinsWater|Acid|Generic`.
   `orig_RecieveDeathEvent` (`D:EnemyDeathEffects.cs:345-400`): guards on `didFire`, records the kill,
   `EmitCorpse`, `EmitEffects`, `if (doKillFreeze) GameManager.instance.FreezeMoment(1)` (no-op'd),
   essence only outside boss scenes, snapshot transition, persistence/recycle.
9. **`SendDeathEvent()`** (`:682`, `:685-691`) → invokes the `OnDeath` delegate.

`OnDeath` subscribers for Hornet (`B:OnDeath.__invocationList` starts with `BossS…`):
- `BossSceneController.Setup` (`D:BossSceneController.cs:183-192`): `bossesLeft--; CheckBossesDead();`
  → `EndBossScene()` at 0 (`D:BossSceneController.cs:207-213`, `:215`).
- The mod: `OnBossActualDeath` bound per HM in `InitBossRefs` (`O:Environment/TrainingEnv.cs:1404-1450`,
  handler `O:…:1263-1275`), which sets `_bossDied`; `Step()` then sets `_episodeDone` with
  `_episodeResult = "win"` (`O:Environment/TrainingEnv.cs:652-655`). The knight-death branch is
  `PlayerData.instance.health <= 0` (`O:…:656-677`). Deliberately **not** keyed on `hp <= 0`, because
  `hasSpecialDeath` bosses reach 0 HP without dying (`O:…:1189-1200`, `wouldDie` at `:1199`).
- Boss set = union of `BossSceneController.bosses` and `BossHealthLookup.Keys`
  (`O:Environment/TrainingEnv.cs:1429-1444`).

---

## 5. What the observation reads

All six columns are produced in `HitboxObserver.GetSplitFeatures`
(`O:Game/HitboxObserver.cs:700-799`); the combat row is assembled at `:793-797` as
`[relX, relY, w, h, velX, velY, isTrigger, givesDamage, takesDamage, isTarget, isInvincible, hpRaw, hpMaxRaw, animPhase]`.

Bucketing (`O:Game/HitboxObserver.cs:100-123`), evaluated per `Collider2D` in this order:
1. `col.GetComponent<DamageHero>() != null || col.gameObject.LocateMyFSM("damages_hero") != null` → **Enemy**
2. `layer == TERRAIN && !isTrigger` → Terrain
3. `gameObject == HeroController.instance.gameObject && !isTrigger` → Knight
4. `GetComponent<DamageEnemies>() != null || LocateMyFSM("damages_enemy") != null ||
   (name == "Damager" && LocateMyFSM("Damage") != null)` → **Attack**

| column | line | runtime read | game state it corresponds to |
|---|---|---|---|
| `gives_damage` | `:763` | `kvp.Key == HitboxType.Enemy ? 1 : 0` | the collider's GameObject carries a `DamageHero` component or a `damages_hero` FSM — **exactly the two things `HeroBox.CheckForDamage` looks for** (`D:HeroBox.cs:43, 58`). It is a *capability* flag, not a live one: `DamageHero.damageDealt` can be 0 (intro, `F:…Control[GG Intro 1]`; post-death, `D:HealthManager.cs:575`) while `gives_damage` still reads 1. |
| `takes_damage` | `:764` | `hm != null` where `hm = reader.GetParentHm(col)` (`:761`) | a `HealthManager` exists on the collider's transform or on an ancestor within 8 levels (`O:…:221-236`). `HitTaker.Hit` only walks **3** levels (`D:HitTaker.cs:7`) and the `damages_enemy` FSM only walks self+parent+grandparent — so a collider 4–8 levels below an HM reads `takes_damage = 1` but cannot actually be damaged (Q-dmg-6). |
| `is_target` | `:765` | `hm != null && bossHms.Contains(hm)` | `bossHms` = `TrainingEnv._bossHMs` = `BossSceneController.bosses` ∪ `BossHealthLookup.Keys` (`O:Environment/TrainingEnv.cs:1429-1444`) — the same set `BossSceneController` uses to end the scene (`D:BossSceneController.cs:183-192`). |
| `is_invincible` | `:776` | `hm.IsInvincible` | the serialized `HealthManager.invincible` bool (`D:HealthManager.cs:226-236`), the same field `IsBlockingByDirection` (`:711`) tests before diverting a hit to `Invincible()`. Written only by `SetInvincible` FSM actions (`D:SetInvincible.cs:31, 35`). **It does NOT cover `evasionByHitRemaining`** — the 0.2 s post-hit / 0.15 s post-clink window that actually rejects most repeat hits (`D:HealthManager.cs:334, 427, 534`) is invisible to the observation (Q-dmg-7). |
| `hp_raw` | `:766` | `(float)hm.hp` | the public `HealthManager.hp` field (`D:HealthManager.cs:108`), post-`hpScale` (`:313`). Can go as low as −50 (`:504`). |
| `hp_max_raw` | `:767` | `reader.ObserveMaxHp(hm)` | `O:Game/HitboxObserver.cs:208-219`: `max(first-seen hp, current hp)`, cached per HM and bumped when `hp` rises (phase refills). **Not** `hpScale.level1/2/3` and not `BossHealthLookup[hm].adjustedHP`; it is whatever `hp` was the first time the collider was observed. |

`combatKinds[i]` = nearest ancestor with a `tk2dSpriteAnimator`, stripped (`O:…:134-142`);
`combatParents[i]` = the `"entity|clip"` key from `GetClipKey` (`O:…:152-175`) — note the field named
"parents" is filled with `clipKey` (`O:…:799`), not with `GetParentKind`.

The trace's ENTITY block records `HealthManager.IsInvincible` for every loaded HM with the same read
(`docs/trace-format.md` §0x01 ENTITY, `invincible`), so sim-vs-trace comparison of this column is direct.

---

## 6. Trace verification (R2 unless noted)

Recorder semantics, needed to read the rows below:
- `EVENT HERO_DAMAGE` is emitted **after** `HeroController.TakeDamage` returns, on **every** call,
  including calls rejected by `CanTakeDamage()`; `amount` is the *argument* (pre-`BossLevel`,
  pre-overcharm), `hp_after` is `PlayerData.health` after the call
  (`O:Oracle/TraceRecorder.cs:140-164`).
- `EVENT ENEMY_DAMAGE` is emitted after `HealthManager.TakeDamage`; `damage` is
  `hitInstance.DamageDealt` (pre-`Multiplier`, pre-`damageOverride`), `hp_after` is `self.hp`
  (`O:Oracle/TraceRecorder.cs:166-190`).
- `phase` is a **latch**, not a Unity-phase reading: `RecorderBehaviour.FixedUpdate` sets it to 1,
  `Update` to 0, `LateUpdate` to 2 (`O:Oracle/TraceRecorder.cs:277-286`); the `HeroController` hooks
  (`:256, :259`) and `RaiseFrame` (`:429`) overwrite it. `phase = 1` therefore means *"emitted after
  this frame's `RecorderBehaviour.FixedUpdate` and before its `Update`"* — a bracket that contains the
  physics step, not evidence of which sub-phase ran the code (Q-dmg-11).

### 6.1 Enemy → knight, `T:r2_rand1.a.hktrace` (seed 12345)

Frame 24993 → 25012 (dt = 0.02 per recorded frame, `time_scale = 1.00` throughout):

| frame | t | hp | invulnerable | recoilFrozen | recoiling | rb velocity | gravityScale | recoilTimer |
|---|---|---|---|---|---|---|---|---|
| 24993 | 24.5835 | 9 | 0 | 0 | 0 | (−6.000, 0.000) | 0.79 | 0 |
| 24994 | 24.6035 | **8** | **1** | **1** | 0 | **(0, 0)** | **0.00** | 0 |
| 24996 | 24.6235 | 8 | 1 | 0 | **1** | **(−15.000, 7.500)** | 0.00 | 0.0200 |
| 24997–25009 | … | 8 | 1 | 0 | 1 | (−15.000, 7.500) | 0.00 | 0.04 … 0.20 |
| 25011 | 24.8235 | 8 | 1 | 0 | 1 | (−15.000, 7.500) | 0.00 | **0.2200** |
| 25012 | 24.8435 | 8 | 1 | 0 | **0** | (0, 0) | **0.79** | **0** |
| 25093 | 25.9235 | 8 | 1 | 0 | 0 | — | 0.79 | 0 |
| 25095 | 25.9435 | 8 | **0** | 0 | 0 | — | 0.79 | 0 |

Confirms: `ResetMotion` + `AffectedByGravity(false)` + `recoilFrozen` land on the same frame as the
damage; the FreezeMoment yield costs exactly **1 frame** (mod no-op); `recoilVector = (−15, 7.5)` =
`(−RECOIL_VELOCITY, RECOIL_VELOCITY × 0.5)` for `CollisionSide.right`; velocity is re-asserted every
step; recoil ends after **11** `recoilTimer` increments (`0.02f × 11 = 0.22`, first sum ≥ `0.2f`).

I-frame length, measured identically in 4 traces / 7 hits
(`r2_rand1.a` ×2, `r2_rand1.b` ×2, `r2_rand2.a` ×2, `r2_move.a` ×1):
**67 recorded frames with `cState.invulnerable == 1`, span 1.3200 s, cleared within the next 0.02 tick**
⇒ effective window ∈ (1.3200, 1.3400] s of game time at dt = 0.02, against a nominal coroutine
time of `DAMAGE_FREEZE_DOWN + INVUL_TIME = 0.001 + 1.3 = 1.301 s`. The ≈0.02–0.04 s excess is not
explained by any source in `analysis/`: Unity’s `WaitForSeconds` resume rule is not decompiled —
Q-dmg-8. **The sim must reproduce the measured 67 ticks, not the nominal 1.301 s.**

Contact damage values, `T:r2_rand1.a.hktrace`: 44 `HERO_DAMAGE` events, all `amount = 1`,
`hazard_type = 1`, sources `Needle`, `Hit ADash`, `Hornet Boss 1`. Only 2 of the 44 changed `hp_after`
(9→8 at frame 24994, 8→7 at frame 25224); the other 42 are i-framed calls. This is the
`CanTakeDamage()==false, hazardType==1` no-op branch (`D:HeroController.cs:2018-2024`) firing on
`OnTriggerStay2D` every physics step, and it confirms **no `BossLevel` multiplier is applied**
(tier 0) and `DamageHero.damageDealt == 1` for all three sources.
`phase = 1` on every event ⇒ `hazardType == 1` is *not* buffered to LateUpdate (`D:HeroBox.cs:76-79`).

### 6.2 Knight → enemy, `T:noint2_move.a.hktrace` (dt 0.02, fpw 2, seed; pre-R2 only in the
`ShakePositionV2` pin, which does not touch this path)

| frame | fixed_count | event / state |
|---|---|---|
| 25121 | 1538 | `EVENT STEP step=265 action=[2,2,0,1]` (action[2]=0 = attack tap), phase 0 |
| 25122 | 1539 | FRAME: `attack` key set, `cState.attacking = 1`, `attack_time = 0.0000`, `attack_cooldown = 0.2300`, `altAttack 1→0`, anim `Slash/0` |
| 25123 | 1540 | FRAME: `attack_time = 0.0200`, `attack_cooldown = 0.2100` |
| 25124 | 1540 | `EVENT STEP step=266` — **no FixedUpdate this frame** (paused) |
| 25125 | 1541 | `FSM_EVENT AltSlash/damages_enemy HIT` → `DEALT DAMAGE` → `Hornet Boss 1/Control HIT`, `Stun Control HIT`, `HIT LANDED`, `TOOK DAMAGE`, `RECOIL HORIZONTAL`, `Soul Orb MP GAIN`, `Strike Nail R(Clone)`, `Slash Impact R(Clone)`, `Stun Control STUN DAMAGE`, `Stun Control Idle→Max Check→In Combo`, then **`EVENT ENEMY_DAMAGE owner="Hornet Boss 1" attack_type=0 damage=32 hp_after=868`** (phase 1). FRAME: boss hp 900→868 |

Confirms:
- `attack_cooldown = ATTACK_COOLDOWN_TIME_CH (0.25) − Time.deltaTime (0.02) = 0.23` on the frame the
  attack starts ⇒ `equippedCharm_32` path, and `LookForQueueInput` (`:5279`) runs *after* the
  cooldown decrement (`:5357`) is scheduled but the decrement executes in the same Update.
- `attack_time = 0` on the starting frame ⇒ `DoAttack` is reached via `LookForQueueInput` (`:5279`),
  **after** the `attack_time +=` block (`:5200-5208`).
- damage lands on the **2nd FixedUpdate after `StartSlash`** (1539 → 1541), matching
  `NailSlash.FixedUpdate`'s `stepCounter == 1` gate (`D:NailSlash.cs:107-111`).
- `damage = 32` = `round(nailDamage 21 × 1.5)` from `F:Knight/Attacks::Set Slash Damage`
  (Fragile Strength equipped, not broken).
- `SoulGain` fired (`Soul Orb MP GAIN`), consistent with `enemyType = 1 ∉ {3,6}` (`D:HealthManager.cs:457`).
- `Stun Control` advanced `Idle → Max Check → In Combo` on the `STUN DAMAGE` event (`D:HealthManager.cs:510`).
- Attack duration: over the whole trace the longest uninterrupted `attacking` run is
  `attack_time ∈ {0.00, 0.02, …, 0.28}` = 15 frames, ending the frame after `attack_time` reaches
  `0.28` ⇒ `attackDuration = ATTACK_DURATION_CH = 0.28` (`H:`).

### 6.3 Nail-art and super-dash damage

- `T:noint2_rand2.a.hktrace@27831` and `@27847`: `ENEMY_DAMAGE attack_type=0 damage=26`,
  hp 900→874→848, source FSMs `Hit R/damages_enemy` + `Hit R/nailart_damage` (Cyclone Slash).
  `26 = round(21 × 1.25)` — the **raw** `nailDamage`, i.e. `nailart_damage[Init]` does **not** apply
  Fragile Strength, unlike `Set Slash Damage`.
- The two Cyclone hits are `fixed_count` 1203 and 1214 = **11 ticks** = `0.22 s` apart, while the
  `Hit L`/`Hit R` `damages_enemy` FSMs fired `HIT` on *every* intervening physics step. This is
  `NonFatalHit`'s `evasionByHitRemaining = 0.2f` (`D:HealthManager.cs:534`) decremented by
  `0.02f` per `Update` (`:329`): after 10 subtractions the float residue is still `> 0`, so the
  11th tick is the first that passes `!(evasionByHitRemaining > 0f)` (`:334`).
- `T:noint2_rand1.a.hktrace@28498`: `ENEMY_DAMAGE attack_type=1 damage=10`, hp 900→890 =
  `Knight/SuperDash Damage` (`damageDealt = 10`, `attackType = 1 Generic`).

### 6.4 Coverage gaps in the corpus

No trace contains: a Great Slash / Dash Slash hit (52 or 53, Q-dmg-3), any spell hit, a focus heal, an
enemy `Invincible()` clink, a knight death, a boss death, a stagger (`STUN`), or a hit while the boss
`IsInvincible`. §7 lists what is therefore unverified.

---

## 7. Open questions

Ids are spec-local; the orchestrator consolidates them into `analysis/open-questions.md` by script.

**Closed by this revision** (evidence now exists, no question remains):
`Q-DMG-1` DamageHero values — closed by `S:…components[DamageHero]`, §1.1 table.
`Q-DMG-4` NailSlash `longnail`/`mantis`/`fury` — closed by `S:…components[NailSlash]`, §2.2 table.
`Q-DMG-5` `DamageEnemies` census — closed by `scene.json`, 0 of 1251 colliders, §2.3.
`Q-DMG-7` Hornet's `Recoil` fields — closed by `S:…components[Recoil]`, §2.6 table; `Sweep` is fully
decompiled at `D:Sweep.cs` and is documented there too.

### Q-dmg-1 — Is `BossSceneController.BossLevel` ever nonzero in the oracle install?

`HeroController.TakeDamage` multiplies incoming damage by 2 at `BossLevel == 1` and replaces it with
9999 at `BossLevel == 2` (`D:HeroController.cs:1833-1844`), so the tier is the single largest lever on
the enemy→knight path. The oracle forces 0 three ways — `BossChallengeUI.LoadBoss` override
(`O:HKOracle.cs:128-147`), a detour on the `BossSceneController.BossLevel` setter that suppresses every
nonzero write and logs the writer (`O:HKOracle.cs:159-170`), and a per-episode clamp
(`O:Environment/TrainingEnv.cs:1243-1256`) — because FullKnight observed `BossLevel = 2` appearing
mid-episode on `GG_Broken_Vessel` with no `LoadBoss` call to explain it. Whether that ever happens in
the oracle install is unknown: the traces carry no `BossLevel` field and no `[TierGuard]` /
`[TierClamp]` line has been checked. **Evidence that would close it:** a grep of
`analysis/traces/p0/logs/*.log` for those two tags, plus a `BossLevel` scalar added to the trace
header or the FRAME record so the sim can assert the invariant instead of assuming it.

### Q-dmg-2 — RNG consumers on the damage path, and their draw order

Under seeded RNG (regime R2 pins `Random.InitState` at `sceneLoaded`), any `UnityEngine.Random` draw
the sim skips desynchronises every later boss branch. The damage path draws in at least four places:
`HealthManager.TakeDamage`'s slash-impact rotations, `UnityEngine.Random.Range(340,380)` / `(70,110)` /
`(250,290)` per nail hit (`D:HealthManager.cs:467-479`); `HeroController.TakeDamage`'s carefree-shield
roll `Random.Range(1,100)` (`:1862-1899`, inactive with this save); `FlingUtils.SpawnAndFling` three
times on every `Die()` (`D:HealthManager.cs:627-668`, all counts 0 for Hornet but still called); and
`Hornet Boss 1::Control[Dmg Response]`'s `SendRandomEvent` on every landed hit
(`F:Boss Holder/Hornet Boss 1::Control[Dmg Response]`, weights 0.30/0.15/0.15/0.40). It also *triggers*
draws it does not itself make: `HealthManager.Invincible` sends `"EnemyKillShake"` to
`GameCameras.instance.cameraShakeFSM` (`D:HealthManager.cs:372`), and the knight-damage effect FSM
sends `AverageShake` — `analysis/specs/REVIEW-p1.md` D05 measures `ShakePositionV2` at three
`Random.Range` calls per update, ≈75 draws per boss dash/sphere and ≈153 per knight hit (REVIEW-p1 R2-3), none of which
this spec or `analysis/specs/boss-hornet.md` counts. **Evidence that would close it:** a `FRAME.rng`-delta census over
frames containing exactly one `ENEMY_DAMAGE` / `HERO_DAMAGE` versus frames containing none, plus a
draw census of `_GameCameras/CameraParent/CameraShake` and every FSM reachable from a damage event.

### Q-dmg-3 — `Mathf.RoundToInt` tie rule at exactly x.5

Every knight damage number is produced by `ConvertFloatToInt rounding=Nearest` →
`Mathf.RoundToInt` (`D:HutongGames.PlayMaker.Actions/ConvertFloatToInt.cs:52-58`), but `Mathf` lives in
UnityEngine.CoreModule, which is **not** in `analysis/decomp/`. Two of the three live cases avoid the
question (`26.25` is not a tie; `31.5` rounds to 32 under every candidate rule and is trace-confirmed
at 32), but Great Slash and Dash Slash compute `21 × 2.5 = 52.5` exactly, which is 52 under
half-to-even and 53 under half-away-from-zero or half-up. Stating either would be a plausible wrong
constant. **Evidence that would close it:** a corpus that charges 1.35 s and releases with up or down
held so a `Great Slash` / `Dash Slash` lands, giving an `ENEMY_DAMAGE` record; or decompiling
UnityEngine.CoreModule.

### Q-dmg-4 — Spell antic and cast durations are tk2d-clip-driven

`F:Knight::Spell Control[Fireball Antic]` and `[Fireball Recoil]` both terminate on the tk2d
`ANIM END` event rather than a `Wait`, so the wall-clock length of a cast — and therefore the frame
on which the projectile spawns and the frame on which the hero's velocity override ends — is a
function of the clip length and the animator's frame-advance rule. That rule is the subject of
`analysis/open-questions.md` Q10 and `analysis/specs/tk2d-animator.md`; this spec has no trace with a
cast in it. **Evidence that would close it:** Q10's animator model plus one corpus that casts, so the
`ENEMY_DAMAGE` frame can be predicted rather than described.

### Q-dmg-5 — CLOSED: input→event mapping for `ListenFor*` is in the dump after all

**The premise was wrong** (`analysis/specs/REVIEW-p1.md` R2-5). `ListenForUp` / `ListenForDown` /
`ListenForAttack` / `ListenForCast` carry their outgoing events in plain `FsmEvent` fields
(`wasPressed`, `wasReleased`, `isPressed`, `isNotPressed` —
`D:HutongGames.PlayMaker.Actions/ListenForUp.cs:10-16`), and `FsmDumper` serialises those as
`{"__fsm": "FsmEvent", "name": …}`. Only the sibling `FsmEventTarget` (the *routing*) is
`__unserialized`, and its `target` enum survives as `Self` — which is all the routing the sim needs
for these actions. Both mappings this question claimed were unreadable are in fact readable:
`F:Knight::Nail Arts[Move Choice]` — `ListenForDown.isPressed = CYCLONE`,
`ListenForUp.isPressed = CYCLONE`, `ListenForUp.isNotPressed = GREAT SLASH` (§3.5); and
`F:Knight::Spell Control[Spell Choice]` — `BoolTest($Pressed Up).isTrue = SCREAM`,
`BoolTest($Pressed Down).isTrue = QUAKE / isFalse = FIREBALL` (§3.2). Both are now transcribed in
full. The general `FsmEventTarget` serialisation gap remains an orchestrator-level dumper item
(`analysis/specs/REVIEW-p1.md` G11); it does not block the damage path.

### Q-dmg-6 — `takes_damage` walks 8 ancestors, the damage paths walk 3

`HitboxObserver.ClassifyParent` searches up to 8 levels for a `HealthManager`
(`O:Game/HitboxObserver.cs:225`), but `HitTaker.Hit` walks 3 (`D:HitTaker.cs:7`) and the
`damages_enemy` FSM walks self + parent + grandparent (§2.3). A collider 4–8 levels below its
`HealthManager` would therefore be advertised to the policy as damageable while being unhittable, and
the sim must reproduce whichever behaviour is real, not the intended one. Nothing establishes whether
such a collider exists in the boss roster. **Evidence that would close it:** a depth histogram of
`Collider2D` transform paths relative to their nearest `HealthManager` over
`analysis/dumps/*/scene.json` (the field is now dumped for all four scenes).

### Q-dmg-7 — `is_invincible` does not cover the evasion window

The observation's `is_invincible` column reads `HealthManager.IsInvincible`, i.e. the serialized
`invincible` bool that `SetInvincible` writes (§5). The mechanism that actually rejects most repeat
hits is a different field: `evasionByHitRemaining`, set to `0.2f` after a landed hit
(`D:HealthManager.cs:534`) and `0.15f` after a clink (`:427`), tested at `:334`. In `GG_Hornet_1`,
`invincible` is true for only 42 of 480 FRAMEs (§4.2) while the evasion window gates every one of the
~11-tick gaps between successive hits (§6.3), so the column the policy sees is nearly constant while
the mechanism it is meant to expose is not. The sim must reproduce this gap exactly for observation
parity (P6), which is not in question; what is open is whether the oracle should also record
`evasionByHitRemaining` so a divergence in it is detectable at all. **Evidence that would close it:**
an owner decision plus, if yes, an ENTITY field in `docs/trace-format.md` (orchestrator-owned).

### Q-dmg-8 — Unity coroutine timing: `WaitForSeconds` and `yield return StartCoroutine(<empty>)`

Two engine rules the damage path depends on are not in any decompiled assembly: how `WaitForSeconds`
decides the frame on which it resumes (and whether it uses scaled time), and how many frames
`yield return StartCoroutine(c)` costs when `c` yield-breaks immediately. They set, respectively, the
i-frame length and the `recoilFrozen` → `recoiling` delay. Measured: the i-frame window is a constant
67 recorded frames / 1.3200 s span across 7 hits in 4 traces against a nominal 1.301 s (§6.1), and
`recoilFrozen` occupies 1–2 recorded frames (§1.3). The sim must reproduce the tick counts, not the
nominal seconds. **Evidence that would close it:** capture the same corpus at a second
`captureDeltaTime` (0.01 with `frames_per_wait` 4) — that separates a `ceil(duration/dt)` rule from a
`ceil(duration/dt) + 1` rule and settles the scaled/unscaled question independently of Q3.

### Q-dmg-9 — Knight death in `GODS_GLORY` never sets `cState.dead` or `HeroBox.inactive`

`HeroController.Die()` takes an early-return branch when the map zone is `DREAM_WORLD` or
`GODS_GLORY` (`D:HeroController.cs:3717-3727`): it relinquishes control, disables the renderer, turns
off gravity and sets `playerData.isInvincible = true`, then `yield break`s — never reaching the code
that sets `cState.dead`, `rb2d.isKinematic`, `HeroBox.inactive` and `gameObject.layer = 2`
(`:3728-3743`). Every GG boss scene takes that branch. So on the sim's episode-ending path, the
`!cState.dead` conjunct of `CanTakeDamage` never trips and `HeroBox` keeps dispatching; the only thing
stopping post-death damage appears to be `playerData.isInvincible`. Unverified: no trace in the corpus
contains a knight death. **Evidence that would close it:** one corpus that kills the knight, checked
for `HERO_DAMAGE` records after `EPISODE_END`.

### Q-dmg-10 — `hp_max_raw` is first-observed hp, not a scaled max

`ObserveMaxHp` caches the `hp` value seen on first sight of a `HealthManager` and only raises it when
`hp` later increases (`O:Game/HitboxObserver.cs:208-219`). It is not `hpScale.level1/2/3` and not
`BossSceneController.BossHealthLookup[hm].adjustedHP`. For a boss whose `hp` is written by an FSM after
the first observation — a phase transition, a scripted refill, a `SetFsmInt` into `health_manager_enemy`
— the column is wrong until the next increase bumps it, and the policy's normalised HP is wrong with
it. Which bosses in the roster do that is not established. **Evidence that would close it:** compare
per-boss `hp` at `SceneReady` against `BossHealthLookup[hm].adjustedHP` and against `hp` at the first
`ENEMY_DAMAGE` across `analysis/dumps/*/bosses.json` and the four scenes' traces.

### Q-dmg-11 — Unity physics-callback dispatch: when, how often, and in what order

The whole enemy→knight path and the whole knight→enemy path run inside `OnTriggerEnter2D` /
`OnTriggerStay2D` (`D:HeroBox.cs:25-39`, the `damages_enemy` FSM's `Trigger2dEvent` actions, §2.3). The
sim needs three rules no source in `analysis/` states: (a) where in the frame Unity dispatches 2D
trigger callbacks relative to `FixedUpdate` and the physics step; (b) whether `OnTriggerStay2D` fires
exactly once per physics step for a persistent overlap; (c) what happens on the step a collider is
enabled or disabled or its GameObject is `SetActive`-toggled — `X:Physics2D.callbacksOnDisable = true`
is dumped, but its precise meaning is not. The trace only brackets these: `phase = 1` on damage events
means "after the recorder's `FixedUpdate`, before its `Update`" (§6), and consecutive `HERO_DAMAGE`
records from one source are 1 `fixed_count` apart, which is consistent with (b) but does not prove it.
This is the damage-path instance of `analysis/specs/REVIEW-p1.md` G6. **Evidence that would close it:** a recorder
capture point inside a purpose-built trigger callback that logs `fixed_count` and the recorder's phase
latch, run against a scripted persistent overlap and a scripted enable/disable.

## 8. Disagreements between sources

1. **`Recoil`: decomp `Reset()` vs the live object.** `D:Recoil.cs:77-84` (`Reset()`, the editor
   default) gives `stopVelocityXWhenRecoilingUp = true`, `recoilDuration = 0.5`,
   `preventRecoilUp = false`. Hornet's live values are `false`, `0.15`, `true`
   (`S:Boss Holder/Hornet Boss 1…components[Recoil]`, §2.6). The dump wins; three of five fields
   differ, and `preventRecoilUp` inverts the up-hit behaviour entirely.
2. **Nail-art damage: serialized vs computed.** `F:` `damages_enemy.damageDealt` at SceneReady is 32
   for Cyclone Hit L/R and 64 for Great/Dash Slash (written by `Set Slash Damage`, with Fragile
   Strength), but `nailart_damage[Init]` recomputes from **raw** `nailDamage` and overwrites on
   activation. The trace resolves it: Cyclone deals 26, not 32 or 40 (`T:noint2_rand2.a@27831`).
   The dumped value is a stale snapshot; the FSM override is authoritative.
3. **Fireball serialized damage.** `F:_GameManager/GlobalPool/Fireball2 Spiral(Clone)::damages_enemy.damageDealt`
   = 25, but `Fireball Control[Set Damage]` writes 30 (or 40 with Shaman Stone) on cast. Same
   stale-snapshot pattern; unverified by trace (Q-dmg-4 corpus gap).
4. **Descending Dark L/R asymmetry.** `Q Slam 2/Hit R` base = 30, `Hit L` base = 35, both → 50 with
   Shaman Stone. Transcribed as dumped; no code explains the asymmetry.
5. **`HeroBox` side test.** `D:HeroBox.cs:48` compares `otherCollider.transform.position.x` against
   `HeroBox.transform.position.x` — GameObject origins, not collider bounds centres. A wide attack
   collider whose origin is on the far side of the knight yields the "wrong" recoil direction. This is
   the code as written; the sim must copy it.
6. **`MP_drained -= drainMP_time`** (`D:HeroController.cs:5292`) subtracts a duration from a counter.
   Almost certainly a Team Cherry bug; it is on the focus-completion path and must be ported verbatim.
7. **Stagger cap.** `Stun Control` carries `Stuns Max = 5` and an `IntCompare` that raises `MAX`, but
   no transition consumes `MAX` (§4.3). The variable reads like a cap and is not one. `analysis/specs/boss-hornet.md`
   reached the same conclusion independently; the first revision of this spec asserted the cap and was
   wrong (`analysis/specs/REVIEW-p1.md` D13 / C7).
8. **`Mathf` is not decompiled.** Three damage numbers are produced by `Mathf.RoundToInt` and one of
   them (`52.5`) is an exact tie. The first revision resolved it from a remembered .NET rule; there is
   no source for that in `analysis/`, so it is now UNKNOWN (Q-dmg-3, `analysis/specs/REVIEW-p1.md` D14).

---

## Review fixes (applied 2026-08-31, against `analysis/specs/REVIEW-p1.md`)

| id | class | change |
|---|---|---|
| D13 | UNSUPPORTED | §4.3: deleted "at most 5 staggers". Added the measured fact that `Stun Control`'s `MAX` event has no consumer anywhere in `analysis/fsm/GG_Hornet_1.json` (the scene's only `MAX` transition is on `Heart Pieces::Set Pieces[Init]`), so `Stuns Max = 5` caps nothing. Added §8 item 7. Also added the previously omitted `Decrement` int variable to the Stun Control variable list. |
| D14 | UNCITED | §2.4: removed "`Math.Round` = half-to-even" (no `Mathf` in `analysis/decomp/`). Great/Dash Slash damage is now **UNKNOWN, 52 or 53**, in the prose, in the per-emitter table and in §6.4. Explained why `31.5 → 32` and `26.25 → 26` do not discriminate. New Q-dmg-3; §8 item 8. |
| D15 / C4 | STALE | §0: dump provenance rewritten — `timestampUtc = 2026-08-31T02:49:29Z`, `frameCountAtDump = 29672`, `X:Time.captureDeltaTime = 0.02`. Only `timeScale = 0` is now called an artefact, and of the capture point rather than the regime. Re-read every `H:`/`P:`/`B:`/`X:`/`F:` value cited in the spec against the current dump; all still hold. |
| D16 | WRONG-LINE | §2.3: the 21-38 range → `D:CheckSendEventLimit.cs:21-36` (the file is 37 lines); `LimitSendEvents` split into `OnEnable :12-15`, `Update :17-31`, `Add :33-41`. Corrected the clearing rule: the unconditional clear at `:27-30` is reachable only when `monitorCollider` is null; every knight slash sets `monitorCollider` to its own collider (`S:`), so the list survives until that collider's enabled state changes. |
| D17 | WRONG-LINE | §2.1: `:3375-3385` → `:3377-3388`, `:3420-3423` → `:3419-3422`. |
| D18 | MISREAD | §1.3: the "uninitialized public fields" claim now covers only the `public float` block at `:103-181`; `DEATH_WAIT` (`:197 private float = 2.85f`) and `ATTACK_QUEUE_STEPS` (`:189 private int = 5`) are called out as initialised, and the `DEATH_WAIT` table row is annotated. |
| D19 | OMISSION | Added every missing conjunct: carefree gate `&& hazardType == 1` (`:1853`) plus the full threshold table; Baldur `&& !flag` (`:1919`); Grubsong `&& damageAmount > 0` (`:1970`); `OnTakenDamage` `damageAmount > 0` (`:1990`); `RecoilDown` `&& !controlReqlinquished` (`:2317`); the flip guard's full condition at `:974-989` (`!cState.backDashing && !cState.dashing` outer block, `(!cState.attacking || attack_time >= ATTACK_RECOVERY_TIME) && !cState.wallSliding && !wallLocked` inner); `ModHooks.OnHitInstanceBeforeHit` at `TakeDamage.cs:95` before `HitTaker.Hit`. |
| D20 | UNCITED | Six engine sentences resolved: trigger-callback timing → Q-dmg-11 + the `phase` bracket; `yield return StartCoroutine(<empty>)` = 1 frame → Q-dmg-8 + the measured 1–2 `recoilFrozen` frames; `WaitForSeconds` scaled time → Q-dmg-8 + the `.a`/`.b` constancy argument; `deltaTime == 0` at `timeScale 0` → cited `analysis/specs/frame-order.md` §3.2; `phase` byte → cited `TraceRecorder.cs:277-286` and restated as a latch/bracket; `WaitForSeconds` rounding → Q-dmg-8 with "reproduce 67 ticks, not 1.301 s". |
| D08 / Q-DMG-1 | STALE | §1.1: replaced "UNKNOWN" with the `S:…components[DamageHero]` table — body 0 (intro), `Hit GDash` / `Hit ADash` / `Sphere Ball` / `Needle` all `damageDealt = 1`, `hazardType = 1`, `resetOnEnable = false`. Q-DMG-1 closed. |
| D09 / C5 | UNCITED-as-value | §2.6: replaced the `Reset()` defaults with the live `S:…components[Recoil]` values — `preventRecoilUp = **true**` (so an up-hit on Hornet produces no knockback and no `HIT UP`), `recoilDuration = 0.15`, `stopVelocityXWhenRecoilingUp = false`, `recoilSpeedBase = 15`. Q-DMG-7 closed; §8 item 1 rewritten. Also documented `Sweep` in full from `D:Sweep.cs` (it is decompiled; it was wrongly listed as unknown). |
| — | (Q-DMG-4) | §2.2: closed from `S:…components[NailSlash]` — `longnail = mantis = true`, `fury = false` on the four non-wall slashes (⇒ the ×1.4 `" M"` branch), all false on WallSlash; `animName` and `scale` tabulated. |
| — | (Q-DMG-5) | §2.3: closed — 0 `DamageEnemies` components across all 1251 loaded-scene colliders in `scene.json`, with the collider-rooted-enumeration caveat stated. |
| C1 | (support) | §4.2: added the ENTITY-block measurement backing this spec's reading of `GG Reset` — `Hornet Boss 1`.`invincible` true on 42 / false on 438 FRAMEs in `T:r2_rand1.a` (first false at frame 24768), 42/558 in `r2_move.a`, 42/198 in `r2_idle.a`. |
| R2-5 | UNSUPPORTED (premise) | §3.5 / §3.2: Q-dmg-5's premise was wrong — `ListenFor*` emit plain `FsmEvent` fields that the dump serialises (only the sibling `FsmEventTarget` is `__unserialized`, and its `target` enum survives as `Self`). Transcribed both mappings in full from the dump: `[Move Choice]` `ListenForDown.isPressed = CYCLONE`, `ListenForUp.isPressed = CYCLONE`, `ListenForUp.isNotPressed = GREAT SLASH` ⇒ up **or** down → Cyclone, neither → Great Slash; `[Spell Choice]` `BoolTest($Pressed Up).isTrue = SCREAM`, `BoolTest($Pressed Down).isTrue = QUAKE / isFalse = FIREBALL`, with the `$Pressed Up`/`$Pressed Down` latches written by `[Button Down]`'s `stateEntryOnly` listeners. Q-dmg-5 retitled `CLOSED:`. |
| — | (format) | §7 rewritten: four questions closed, the rest renumbered `Q-dmg-1..11` as `### Q-dmg-<n> — <title>` with one paragraph each; every in-text reference updated. New Q-dmg-11 (physics-callback dispatch) added for the D20 engine facts. Q-dmg-2 extended with the `CameraShake` / `ShakePositionV2` draws from `analysis/specs/REVIEW-p1.md` D05, which this spec's RNG census had omitted. |
