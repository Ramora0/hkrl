# REVIEW-p1 — independent P1-gate review of analysis/specs (2026-08-31)

Reviewer: non-author agent. Method: `harness/tools/cite_check.py` (mechanical) → per-spec citation spot-check
(every cited claim below was opened at the cited file:line / dump key / trace record; verbatim quotes kept in
the verifier transcripts) → uncited-engine-fact sweep → cross-spec diff → port-gap sweep. Dumps used are the
current ones (`analysis/dumps/GG_Hornet_1/meta.json` timestampUtc 2026-08-31T02:49:29Z, frameCountAtDump 29672),
FSM dump `analysis/fsm/GG_Hornet_1.json` (fsms[] carries restartOnEnable/manualUpdate/... per Q17), traces
`analysis/traces/p0/r2_{idle,move,rand1,rand2}.a.hktrace`. Trace record indices below = FRAME-record index
("TR@i") or global record index ("rec"), as the spec in question uses.

cite_check.py: boss-hornet 101 cites/0 unresolved; damage-path 141/1 (the spec cited lines 21-38 of CheckSendEventLimit.cs, a 37-line file; OnEnter is 21-36);
frame-order 76/0; fsm-actions 171/0; fsm-runtime 220/0; hero-motion 113/22 — all 22 are false positives
(spec-defined keys `phys.json#`/`pd.json#`, brace-expanded trace glob at L25, `FRAME.cstate` at L318);
tk2d-animator 113/0.

## 1. Per-spec verdicts

| spec | verdict | reason (one line) |
|---|---|---|
| hero-motion.md | PASS-WITH-FIXES | 47/50 cites confirmed; 2 misreads (attackDuration stale, "attackDuration decremented"), 1 wrong-line, Q9 misreads a coroutine as fixed-step, pause-frame count 308≠302, ~15 uncited Unity/Box2D/InControl sentences. |
| damage-path.md | PASS-WITH-FIXES | 41/47 confirmed; "at most 5 staggers" (L735) contradicted by dump (MAX has no transition); half-to-even rounding uncited (52.5→52 unsupported); dump timestamp/captureDeltaTime stale; 2 line-range errors. |
| frame-order.md | PASS-WITH-FIXES | All trace censuses reproduce exactly (22/28), but several Unity-phase attributions are stated as fact where the trace only bounds them; 2 wrong-line, 3 count errors, 1 out-of-repo citation. |
| tk2d-animator.md | PASS-WITH-FIXES | 16/18 confirmed, model reproduces 3600/3600 records; wrong caller name for g_Paused, 2 cites missing the Actions/ dir, "three missing clips" is five. |
| fsm-runtime.md | PASS-WITH-FIXES | 37/45 confirmed incl. DoTransition hinge verbatim; 5 "not dumped" statements + FSM-Q1/Q3/Q5 stale vs Q17 (fields ARE dumped; restartOnEnable=false on 8 FSMs incl. 5 Knight/*), OnEvent returns LAST flag not OR, 55 not 53 system events, Awake re-scan omitted. |
| fsm-actions.md | PASS-WITH-FIXES | 54/56 confirmed; SetParent world-pose claim uncited/inverted; DelayedEvent rationale wrong; V3 forces the LAST over-limit index; GetGameObjectFsm first-FSM fallback omitted; Q1-Q3 stale vs Q15; RNG census scope excludes CameraShake draws that Hornet triggers. |
| boss-hornet.md | FAIL (revise, then re-gate) | Two definitional claims contradicted by dump+decomp+trace (GG Reset Invincible "None → untouched" and "invincible=true whole capture"; Needle Return Vector "scene-start position"), inverted transform.parent claim ×2, layer-8 mechanism misread, and the §6.4/§8 RNG draw list — which the spec itself calls gate-critical — omits ~78 ShakePositionV2 draws per G Dash/A Dash/Sphere. Everything else (timings 20/20, all constants, all FSM tables) confirmed. |
| fsm-census.md | (index only) | 4 cites resolve; not reviewed further. |

## 2. Defect table

Class: MISREAD = source says something else; WRONG-LINE = right file, wrong lines; UNSUPPORTED = no source says it;
STALE = true when written, false against the current dump/dumper; UNCITED = engine fact stated without a cite
(rule 2.2); OMISSION = cited source has a consequence the spec drops.

| id | spec:line | class | evidence | fix |
|---|---|---|---|---|
| D01 | boss-hornet:199-204, 719-722, 890-894 (Q5) | MISREAD | `GG_Hornet_1.json` Control/GG Reset SetInvincible `Invincible = {useVariable:false,name:null,value:false}` = literal (spec's own rule 1 at L20-22 says useVariable:false is a literal); `SetInvincible.cs:29-31` writes when `!IsNone`. | Delete "None → skipped"; GG Reset writes `IsInvincible=false`. Delete Q5; drop the Q5 clause from open-questions Q22. |
| D02 | boss-hornet:202, 717 | MISREAD (trace) | r2_rand1.a entity `Hornet Boss 1`.invincible: True on 42 FRAMEs, False on 438 (first False at frameCount 24768, the frame after GG Land→GG Reset→GG Music→Flourish at 24766 per fsm-runtime Case B). | Replace "true for the whole capture" with the measured 42/438 split. |
| D03 | boss-hornet:121-122 | MISREAD | Needle/Control `restartOnEnable:true` (dump); `Fsm.OnEnable` Fsm.cs:1847-1853 restarts in startState on every enable; Control/Throw runs `SetPosition($Needle)` (idx 6) before `ActivateGameObject($Needle,true)` (idx 7); Needle/Control dump `activeState:None, finished:true, started:true` = StopAndReset happened. ⇒ `Init` re-runs on every throw and `$Return Vector` = throw origin (Hornet pos − 0.5 y), not the scene-start position. | Rewrite; add Q on SetActive→OnEnable synchrony (fsm-actions Q7) since the value read depends on it. |
| D04 | boss-hornet:94, 847; fsm-actions:389-390 | UNSUPPORTED/UNCITED | `SetParent.cs:35` only assigns `transform.parent`; nothing in any source says world pose is not preserved. (Unity's documented `Transform.parent` setter preserves world pose; only `resetLocalPosition/Rotation` move the object — both false in Deparent/Setup.) | Mark UNKNOWN + Q (new N1). Do not port "world position not preserved". |
| D05 | boss-hornet:§6.4 (848-857), §8.2 of fsm-actions (705-777) | OMISSION | Control/`G Dash`(idx 3), `A Dash`(idx 2), `Sphere`(idx 3), `Sphere A` send `SendEventByName "EnemyKillShake"` → `_GameCameras/CameraParent/CameraShake` global `EnemyKillShake → ShakingKill` → `ShakePositionV2` (Extents 0.105, Duration 0.5, FpsLimit var = 0) draws `Random.Range` ×3 per UpdateShaking (`ShakePositionV2.cs:92`) = 3×(1+25) = **78 draws per attack**; `HealthManager.cs:372` (clink) and `Knight/Effects/Damage Effect/Knight Damage/Gen → AverageShake` (Duration 1.0 ⇒ ~153 draws per knight hit) likewise. None counted anywhere. | Extend the RNG census to every FSM reachable from Hornet/knight events (CameraShake at minimum); the P4 "seeded RNG" gate cannot pass on the current list. |
| D06 | boss-hornet:247-248, 536-537 | MISREAD (mechanism) | `CheckCollisionSide.cs:131-137` `if (!otherLayer) CheckTouching(8); else CheckTouching(otherLayerNumber);` — layer 8 comes from `otherLayer=false`, not from `otherLayerNumber=0`. Outcome identical for all 7 Hornet instances. | Say "`otherLayer=false` ⇒ layer 8". |
| D07 | boss-hornet:28 | STALE | meta.json `frameCountAtDump` = 29672 (spec 26760); dump re-taken 02:49:29Z. | Update. |
| D08 | boss-hornet:79-80, 107-108, 553-555, 730-731 (Q3/Q4/Q6/Q7) | STALE | `dumps/GG_Hornet_1/scene.json` now carries DamageHero fields (Needle/Hit GDash/Hit ADash/Sphere Ball damageDealt=1, body 0 at dump), the `Needle` PolygonCollider2D (layer 22, 3 pts), `Needle Tink` BoxCollider2D (layer 17, 3.2466×0.2511, enabled false), ConstrainPosition and Recoil fields. | Cite scene.json; close Q3/Q4/Q6/Q7 (open-questions Q22). |
| D09 | boss-hornet:725-726 | UNCITED-as-value | `Recoil.cs:81-82` are `Reset()` editor defaults (damage-path §8.1 says so); scene.json has the live `recoilSpeedBase`. | Cite scene.json values, not Reset(). |
| D10 | boss-hornet:442-443 | OMISSION | Stun Control also has int `Decrement` (dump). | Add to the variable list. |
| D11 | boss-hornet:103, 89-94 | OMISSION | Needle Tink is de-parented by its own `Setup` state (`SetParent(Owner,null)`), and `Setup` writes Control's `$Needle Tink` via `SetFsmGameObject`; `Deparent` de-parents **Needle** and deactivates it. | State both; the Pause-state `FindChild` chain is not the only writer of `$Needle Tink`. |
| D12 | boss-hornet:302-309, 469-472, 928-931 | OMISSION | `ActionHelpers.cs:70` fallback confirmed; bosses.json component order has two `PlayMakerFSM` entries (idx 7, 10) and the FSM dump enumerates `Stun Control` before `Control` — which one `GetComponent` returns is not established. Result ($HP stays 0) is the same either way (FsmVariables.GetFsmInt:1304-1305 returns a fresh `FsmInt(0)` on miss). | Keep Q14; note the outcome is order-independent. |
| D13 | damage-path:734-735 | UNSUPPORTED | Stun Control/`Stun`: `IntCompare($Stuns Total,$Stuns Max)` → event `MAX`; no state or global transition named MAX (dump) — boss-hornet L464 already says the cap is inert. | Delete "at most 5 staggers". |
| D14 | damage-path:429-432, 948 | UNCITED | `ConvertFloatToInt.cs:56-57` → `Mathf.RoundToInt` ✓; "= Math.Round = half-to-even" has no source (no Mathf in decomp). 31.5→32 / 26.25→26 traces do not discriminate tie rules. | UNKNOWN + Q (Q-DMG-6 → open-questions). 52.5→52 is unsupported. |
| D15 | damage-path:56-57 | STALE | physics.json `Time.captureDeltaTime` = 0.02 (spec 0.00848); meta timestamp 02:49:29Z (spec 01:53Z). | Update; only `timeScale=0` is the regime artefact. |
| D16 | damage-path:380-383 | WRONG-LINE | `CheckSendEventLimit.cs` is 37 lines, OnEnter is 21-36 (spec had 21-38); `LimitSendEvents.cs` OnEnable 12-15, Update 17-31; the "unconditionally when non-empty" clear (`:27`) applies only when `monitorCollider` is null. | Fix ranges and the clearing rule. |
| D17 | damage-path:285-286 | WRONG-LINE | `attack.WasPressed` block is HC:3377-3388; queued attack HC:3419-3422. | Fix. |
| D18 | damage-path:203-205, 229 | MISREAD | `HeroController.cs:197 private float DEATH_WAIT = 2.85f;` (and :189 ATTACK_QUEUE_STEPS=5) are initialised, not "uninitialized public fields". | Exclude them from the statement. |
| D19 | damage-path:129-136, 257, 977 (conjuncts) | OMISSION | Baldur branch also requires `!flag` (carefree, HC:1919); carefree gate requires `hazardType==1` (:1853); Grubsong/OnTakenDamage require `damageAmount>0` (:1970, :1990); `RecoilDown` requires `!controlReqlinquished` (:2317); flip guard needs `!wallSliding && !wallLocked` (:977); `TakeDamage.cs:95` passes `ModHooks.OnHitInstanceBeforeHit` before `HitTaker.Hit`. | Add the conjuncts. |
| D20 | damage-path:85, 184, 201, 480-481, 828, 855 | UNCITED | Unity trigger-callback timing, `yield return StartCoroutine(empty)` = 1 frame, WaitForSeconds scaled-time, deltaTime==0 at timeScale 0, phase byte meaning, WaitForSeconds rounding. | Cite frame-order §3.2/TraceRecorder or mark UNKNOWN (Q-DMG-12 → open-questions). |
| D21 | hero-motion:586 | STALE/MISREAD | hero.json `attackDuration` = 0.0 (assigned only inside `Attack()`, HC:1332/1336; hero idle at dump). | Say "0 at dump; 0.28 by code path with charm 32". |
| D22 | hero-motion:570-571 | MISREAD | HC:5200-5205 increments `attack_time` and compares to the constant `attackDuration`; nothing is decremented. | Reword. |
| D23 | hero-motion:453-460, 697 | WRONG-LINE | dashingDown condition HC:3539-3543; `dashCooldownTimer` HC:3551-3558 (spec pairs them backwards). | Fix. |
| D24 | hero-motion:196 | MISREAD (arith.) | 908 HC_UPDATE − 606 FIXED = 302 Updates without a FixedUpdate (+1 SCENE_READY frame = 303 frozen, frame-order L47); 908−600 mixes FRAME and FIXED counts. | 302. |
| D25 | hero-motion:777-779 (Q9) | MISREAD | `CheckForTerrainThunk` is `IEnumerator` with `yield return null` (HC:4358, 4423) and a countdown (`thunkTimer -= Time.deltaTime`, :4421); not a fixed-step context. Only `Dash()` (HC:1520) reads deltaTime inside FixedUpdate. | Rewrite Q9. |
| D26 | hero-motion:141-144, 69 | OMISSION | `cState.onGround=true` is written by `BackOnGround()` (HC:4195) from Update timers/failsafes (5156, 5165, 3954, 3990) and scene-entry paths (1564, 2471, 2513, 2567, 2697, 2797) — not "only" from collision callbacks; `ExitAcid`/`EnterAcid` (HC:4289, 4297) write gravityScale directly (obsolete, no callers). | Soften both "only" claims. |
| D27 | hero-motion:27-28 | MISREAD (count) | 23 initialisers (19 at 183-219 + 233, 713, 719, 723; 719 is a `const`) ⇒ 324 of 347. | 324. |
| D28 | hero-motion:80-81,109-110,131-132,137-138,163,202,204,260-261,267-268,272-273,277,285 | UNCITED | semi-implicit Euler; Baumgarte "~2 steps"; Continuous CCD "is why"; velocityThreshold/no-bounce; recoil "inside the physics step"; raycast-hits-triggers; coroutine resumes after Update; deltaTime = timeScale×captureDeltaTime; deltaTime==fixedDeltaTime in FixedUpdate; InControl `updateMode` default / commit tick / 0.7071 normalisation; "FixedUpdate precedes Update". | Cite frame-order/trace where measured (285 is measured: 0 inversions), else mark as engine assumptions (Q16). |
| D29 | hero-motion:153-156 | OMISSION | physics.json hero `rb2d.interpolation` = **Interpolate**; regime R2 sets None from sceneLoaded (STATE.md); spec is silent. | State it; ties to Q13. |
| D30 | frame-order:138-141, 171 | MISREAD/UNCITED | phase==1 owners include Evade Check 62/418 and Floor Check 0/164 (boss/scene FSMs), not only "hero attack/range colliders"; "physics-callback sub-phase … after the FixedUpdate list and before the Update list" is an engine claim the trace cannot separate (spec concedes at 141-143). | Reword as bounded, not attributed. |
| D31 | frame-order:155, 192 | UNCITED (over-claim) | `phase==2` only tags records after `RecorderBehaviour.LateUpdate` (:286); LateUpdates dispatched earlier in the list would carry phase 0. "FRAME precedes all LateUpdate-phase FSM activity" is proven only for post-recorder LateUpdates. | Qualify. |
| D32 | frame-order:176, 327, 403, 412-413 | UNCITED (over-claim) | InControl sampling is bounded to (ApplyAction at F) < sample < (HeroController.Update at F+1); nothing excludes F+1's FixedUpdate/physics phase. "Update phase" not established. | Qualify; keep Q2. |
| D33 | frame-order:251-253 | UNSUPPORTED (circular) | FRAME.dt is sampled at `RaiseFrame` (TrainingEnv.cs:617) before the `timeScale=0` write at :627, so frame F's dt cannot witness a same-frame effect. | Drop the corollary or re-derive from Δtime only. |
| D34 | frame-order:265-266 | MISREAD (count) | frozen frames carrying `HeroCtrl-LeftGround`: 71/303 (move) — 68 is the collapsed-signature count; "every frozen frame spent airborne" overstated (FallCheck fires only when `v.y <= -1e-6`, HC:3855). | 71; reword. |
| D35 | frame-order:292-293, 356-361, 363 | UNSUPPORTED/MISREAD (counts) | per-step signatures: 3/3 with fsm runs stripped, 24/28 unstripped ("5/6" not reproducible); corpus L/R transitions are 32 not 29 (31/32 first move at F+2, the 32nd is wall-pinned and still consistent); no-effect jump presses are 8 not 7. | Fix counts; state the filter. |
| D36 | frame-order:10-12, 193 | WRONG-LINE | OBS writes `frame` at TraceRecorder.cs:414 (omitted); obs build is TrainingEnv.cs:748-759 (SnapshotFsms at :759). | Fix. |
| D37 | frame-order:34 | OUT-OF-REPO | `%USERPROFILE%/.../HKOracle_oracle_g1.log` is outside analysis/; repo logs (`analysis/traces/p0/logs/*`) lack the string. HC grep (no LateUpdate) suffices. | Cite the grep only, or copy the log line into analysis/. |
| D38 | frame-order:170, 200 | MISREAD (counts) | `HandleFixedUpdate = true` classes = 50 in Actions/ + `CheckTrackTriggerCount.cs:42` = 51 (fsm-runtime L323 says 50); Knight/* fixed FSMs = 8 (the 8 names listed), not 7. | 51 / 8. |
| D39 | frame-order:127-128, 421-422 | OMISSION | `_phase` is also written at TraceRecorder.cs:256/259 (HC hooks) and :429 (FRAME→0); inference at L129-130 survives. | Complete the semantics. |
| D40 | tk2d:51-52 | MISREAD | sole caller is `tk2dAnimatedSprite.g_paused` (static, :61-71), not the instance property `Paused` (:73-77, which reads `Animator.Paused`). | Rename. |
| D41 | tk2d:179, 409-418 | WRONG-LINE (path) | `Tk2dPlayAnimationWithEvents.cs` / `Tk2dWatchAnimationEvents.cs` live under `HutongGames.PlayMaker.Actions/`. | Add the dir. |
| D42 | tk2d:311-317, 275, 362-363 | MISREAD (count) | Play targets absent from the Knight library: `Back Dash`(:272), `Fireball`(:299), `Swim`(:341) **and `Backdash Land 2`(:149, the P3 prologue)**; `IsPlaying("Backdash Land")`(:407) and `IsPlaying("Lookup")`(:416) also miss. P3 therefore takes the `Play(null)` path too; the L362-363 delegate note is moot. | Five/six, and fold P3 into Q-A. |
| D43 | tk2d:15, 58, 72, 252, 512, 594-595 | UNCITED | LateUpdate once per frame; C# truncation; scaled deltaTime; FMA; `bounds` = world AABB. | Cite or mark. |
| D44 | fsm-runtime:133-136, 259-260, 289-290, 403, 637-638, 911-916, 923-927, 934-937 | STALE | FsmDumper.cs:178-186 emits restartOnEnable/manualUpdate/keepDelayedEventsOnStateExit/maxLoopCountOverride/maxLoopCount/exposedEvents/hasHost/subFsmCount; :116-130 DumpGlobals → `dumps/<scene>/globals.json` (15 vars, 51 events); JSON carries them on 962/962. restartOnEnable=false on 8 Hornet-scene FSMs incl. Knight/{Map Control,Spell Control,Nail Arts,Superdash,Surface Water} (15 FK / 8 Gruz / 8 MMC). | Delete "not dumped"; §1.5 must read the per-FSM flag; close FSM-Q1/Q3/Q5 (= Q17). |
| D45 | fsm-runtime:232-236 | OMISSION (port hazard) | `FsmState.OnEvent` FsmState.cs:322-328: `flag = action.Event(e)` is overwritten each iteration; return is `IsSwitchingState || flag` of the **last** active action, not an OR. | State it explicitly. |
| D46 | fsm-runtime:420 | MISREAD (count) | 55 `AddSystemEvent("…")` calls in FsmEvent.cs:440-497 (56 matches incl. the method decl at :499). | 55. |
| D47 | fsm-runtime:59-62 | OMISSION | `Fsm.Awake` Fsm.cs:1598-1601 re-runs `CheckFsmEventsForEventHandlers()` when `!preprocessed` — relevant to the §1.2 hazard. | Add. |
| D48 | fsm-runtime:446-447, 130, 138-145, 171, 202-250 | WRONG-LINE (stale) | `globalTransitions` field is Fsm.cs:59 (:518 is the property); FsmDumper.cs: comp.Fsm :148, InitData forcing :158-163, globalTransitions :199, action loop :248+. | Re-point. |
| D49 | fsm-runtime:100, 278-280, 347, 508, 882 | UNCITED | inactive-GameObject callback rule; "once per rendered frame"; FsmList order = enable order; "in FsmList order" as ABI. | Mark as engine assumptions (FSM-Q4). |
| D50 | fsm-actions:105-106 | MISREAD (rationale) | In the delay-0 `!everyFrame` branch `Finish()` runs inside `OnEnter` (SendEvent.cs:40-43); `OnUpdate`/`WasSent(null)` never executes there. | Delete the "which is why" clause. |
| D51 | fsm-actions:278-281, 932 | MISREAD (nuance) | SendRandomEventV3.cs:43-49: the missed-max scan has no `break` ⇒ forces the **highest** over-limit index. | "highest", not "that index". |
| D52 | fsm-actions:607-621 | OMISSION | `ActionHelpers.cs:70` returns the first `PlayMakerFSM` on a name miss; `SetFsm*`'s own "Could not find FSM" branch fires only when the object has none. | Add to the cross-FSM rows. |
| D53 | fsm-actions:957-975 (Q1-Q3), 690-691, 777 | STALE | open-questions Q15 (closed): float `Range(a,a)` always draws, int `Range(a,a)` does not; streams shared. All 11 Hornet audio sites and `RandomFloat [41,41]` draw; `Range(8,9)` draws. | Close Q1-Q3 → Q15; remove "subject to Q1". |
| D54 | fsm-actions:251, 256, 236-241 | OMISSION (counts) | both Hornet `SendEvent` are `enabled:false`; 1 of 14 `SendEventByName` (Throw) disabled; 4/13 GetScale, 8/27 SetScale, 1/19 SetBoxCollider2DSizeVector disabled — instance counts ≠ enabled counts. | Label counts as instances. |
| D55 | fsm-actions:297, 604-605, 646, 650, 983-984 | UNCITED | trigger dispatch in the physics step; FreezeMoment "pauses the game clock"; pool residue; Destroy deferral; particle seed. | Cite GameManager.cs:2887-2897 for FreezeMoment; mark the rest. |
| D56 | fsm-actions:126-127 | OMISSION | `GetRandomWeightedIndex` also returns −1 when the draw equals `sum` (float Range is max-inclusive, Q15), not only when `sum==0`. | Note. |

## 3. Cross-spec contradictions

| id | topic | locations | what the sources support |
|---|---|---|---|
| C1 | GG Reset `SetInvincible.Invincible` | boss-hornet:199-204, 719-722, Q5 ("None → untouched"; "trace invincible=true throughout") vs damage-path:708-711 ("writes false") | damage-path. Dump literal `false`; SetInvincible.cs:29-31; trace False on 438/480 FRAMEs from f24768. open-questions Q22's Q5 clause is void. |
| C2 | frozen frames per trace | hero-motion:196 (308) vs frame-order:47 (303; 302 with HC_UPDATE) | frame-order (302 measured). |
| C3 | `HandleFixedUpdate` action-class count | frame-order:170 (51) vs fsm-runtime:323 (50) | 51 (50 in Actions/ + CheckTrackTriggerCount.cs:42). |
| C4 | dump regime | damage-path:56-57 (pre-R2, captureDeltaTime 0.00848, 01:53Z) vs hero-motion:261-262 (`phys.json#Time` = 0.02) | hero-motion; the current dump is R2 (0.02, 02:49:29Z). |
| C5 | `Recoil` values | boss-hornet:725-726 quotes Reset() defaults as if live; damage-path:996-998 treats them UNKNOWN | damage-path; scene.json now has the live values (D08/D09). |
| C6 | Needle `$Return Vector` | boss-hornet:121-122 ("scene-start spawn position") vs fsm-runtime:117-136 (RestartOnEnable → restart in startState on enable) + dump `Needle/Control restartOnEnable:true` | fsm-runtime (D03). |
| C7 | stagger cap | damage-path:734-735 ("at most 5 staggers") vs boss-hornet:463-464 ("Stuns Max cap is inert") | boss-hornet (D13). |
| C8 | transform.parent | boss-hornet:94, 847 and fsm-actions:389-390 ("world position not preserved") — not contradicting each other but both uncited and contrary to the engine default | neither; UNKNOWN (D04). |
| C9 | RNG draw accounting | fsm-actions §8 + boss-hornet §6.4 ("every draw, in order") vs open-questions Q14 / STATE.md 08-30 23:40 (ShakePositionV2 ×3 per update after every knight hit) | Q14/STATE; both specs omit the CameraShake FSM (D05). |
| C10 | Hero rb2d interpolation | physics.json `Interpolate` vs STATE.md R2 "None from sceneLoaded"; bosses.json Hornet `None`; hero-motion silent | unresolved (Q13); spec must say which it ports (D29). |
| C11 | `Wait`/tk2d/frame timing rules | boss-hornet §4.1-4.2 (N = floor(D/dt)+1 for anim states; N = k−1 for Wait states entered from an anim-complete) vs tk2d §1.6/§3.3 (f32 accumulator, completion in LateUpdate same frame) vs fsm-runtime §2.1/§2.2 (StateTime += dt in Update; foreign-sender events commit immediately) vs fsm-actions Wait (`timer += Time.deltaTime`, Finish then Event) | CONSISTENT. Verified numerically: f32 `Σ0.02f ≥ T` first at k = 13 (0.25), 18 (0.35), 51 (1.0), 151 (3.0 — answers boss-hornet Q1: Stun Land = 150 records); `Turn` completes at LateUpdate 6. Port the mechanism, not the closed-form rule (the −1 depends on the entering path). |
| C12 | frame order / dt regime | frame-order §3.3-3.4, hero-motion §2.1-2.2, tk2d §1.1-1.2, damage-path §0, boss-hornet §4 (dt=0.02, fpw 2, 1 frozen frame/step, HeroController.Update runs on the frozen frame with dt 0, PlayMaker gated, tk2d idle) | CONSISTENT; only the hero-motion count (C2) differs. fsm-runtime §5 (ungated dispatch on frozen frames) agrees with frame-order §3.2/§6 Q1 and open-questions Q18. |
| C13 | FRAME capture slot | boss-hornet:589 "sits in Update" vs frame-order:188-193 (coroutine resume after all Updates, before LateUpdate) | compatible; boss-hornet wording loose. |
| C14 | `attackDuration` | hero-motion:586 (0.28 from dump) vs damage-path:888-890 (0.28 from trace) vs dump (0.0) | code path (0.28 with charm 32); dump value is 0 (D21). |

## 4. Port-blocking gaps (what a sim/hero, sim/fsm, sim/phys worker would have to guess)

| gap | blocks | existing Q | status |
|---|---|---|---|
| G1 CameraShake/ShakePositionV2 draws on Hornet attacks (78 per EnemyKillShake) and on every knight hit (Damage Effect → AverageShake, ~153) and Knight FSM shakes (Nail Arts, Spell Control, Superdash, Dream Nail) | sim/fsm seeded-RNG gate (P4) | Q14 (wall-clock aspect only) | MISSING Q: draw census of `_GameCameras/CameraParent/CameraShake` + rule that the sim ticks it |
| G2 The Knight's 13 PlayMakerFSMs (ProxyFSM, Spell Control, Nail Arts, Superdash, Dream Nail, Map Control, Roar Lock, Surface Water, Dream Return, Spore Cooldown, Globalise, Control Interpolation, …): own super-dash/spell/focus/nail-art motion, `acceptingInput` lockouts, camera shakes, `restartOnEnable=false` on 5 | sim/hero ∩ sim/fsm scope; no spec enumerates them (hero-motion Q8, tk2d Q-A, damage-path §3 partial) | none | MISSING Q + owner scope decision |
| G3 `transform.parent` world-pose semantics (Needle, Needle Tink) | sim/fsm SetParent | none | MISSING Q (D04) |
| G4 `GameObject.SetActive` → `OnEnable`/`Start` synchronous re-entry (Needle restart reads position set one action earlier) | sim/fsm ActivateGameObject | fsm-actions Q7 (spec-only) | MISSING in open-questions |
| G5 Script-execution order within a phase: PlayMakerFSM.Update vs HeroController.Update vs InControl sample vs HealthManager.Update (evasion timer) vs NailSlash.FixedUpdate vs PlayMakerFixedUpdate vs physics step; tk2d LateUpdate vs PlayMakerLateUpdate; which of two `PlayMakerFSM` components `GetComponent` returns | sim/core scheduler; damage-path §6.3 11-tick spacing; boss-hornet Escalation fallback | Q4 (partial), FSM-Q4(d), frame-order Q2/Q4/Q5, boss-hornet Q9/Q14 (spec-only) | MISSING consolidated Q with the exact pairs |
| G6 Physics callback timing and cadence (OnTriggerStay every fixed step? enter/exit on collider enable/disable and SetActive? callback order among simultaneous contacts) | sim/phys, damages_enemy re-fire, Trigger2dEvent detectors | Q16 (generic), FSM-Q4(a,b) spec-only | MISSING explicit Q |
| G7 Box2D contact details: resting-y 28.40812 vs sweep target 28.40809, Hornet `Land Y` snap 27.55 → resolved to 28.5619 over 2 frames, edgeRadius/contactOffset, `queriesStartInColliders=false` for rays starting inside own bounds | sim/phys | Q16; hero-motion Q4 (spec-only; terrain now in scene.json) | partially MISSING (Hornet snap-resolve not in any Q) |
| G8 `Mathf.RoundToInt` tie rule (52.5) | sim/fsm damage | Q-DMG-6 spec-only | MISSING in open-questions |
| G9 `WaitForSeconds` tick rule (i-frames 67 ticks vs 1.301 s) and `yield return StartCoroutine(empty)` = 1 frame | sim/hero Invulnerable | Q-DMG-12 spec-only | MISSING in open-questions |
| G10 iTween (needle return → `Thrown` length; Sphere Ball radius growth = live hitbox) | sim/fsm Throw, Sphere | boss-hornet Q12, fsm-actions Q10 spec-only | MISSING in open-questions |
| G11 `FsmEventTarget` fields are `__unserialized` in the dump (only `__fields.target` enum survives) — SendEvent/SendEventByName/ListenFor* targets | sim/fsm dispatch | Q-DMG-9 (ListenForUp/Down only, spec-only) | MISSING general Q / dumper fix |
| G12 Hero rb2d interpolation (dump Interpolate vs regime None) | sim/hero transform reads (Q13) | Q13 partial | needs the decision recorded |
| G13 `ParticleSystem` and `FindGameObjectsWithTag` order, `SendMessage` receiver order, `Physics2D.Raycast` tie-break | sim/fsm | fsm-actions Q4-Q6, Q9; boss-hornet Q10-Q11 (spec-only) | MISSING in open-questions |
| G14 Knight death in GODS_GLORY never sets `cState.dead`/`HeroBox.inactive` | sim/hero episode end | Q-DMG-13 spec-only | MISSING in open-questions |
| G15 `hp_max_raw` = first-observed hp; `is_invincible` excludes evasion window; `takes_damage` 8-level walk vs 3-level hit | sim/obs (P6) | Q-DMG-10/11/14 spec-only | MISSING in open-questions |

Spec-local questions with no `analysis/open-questions.md` entry (rule 2.2 requires one): hero-motion Q1-Q4, Q7-Q9;
damage-path Q-DMG-1..14 (all); frame-order Q2-Q8; tk2d Q-A..Q-G; fsm-runtime FSM-Q2, Q4, Q6, Q8; fsm-actions
Q4-Q12; boss-hornet Q1, Q8-Q14. Closable now: hero-motion Q4 & boss-hornet Q3/Q4/Q6/Q7 & Q-DMG-1/7 (scene.json);
fsm-actions Q1-Q3 & boss-hornet Q2 (Q15); fsm-runtime FSM-Q1/Q3/Q5 (Q17); boss-hornet Q5 (void, D01); boss-hornet
Q1 (C11: 150 records); boss-hornet Q8 (tk2d §1.3); tk2d Q-C (fsm-runtime §2.2: delegate raised outside the FSM's
execution stack ⇒ immediate commit); open-questions Q2, Q4, Q5, Q10 are answered by hero-motion §1.3, frame-order
§2-3, fsm-census, tk2d §1 and can be marked closed.

## 5. Spot-check tally

| spec | checked | CONFIRMED | WRONG-LINE | UNSUPPORTED | MISREAD | STALE | notes |
|---|---|---|---|---|---|---|---|
| hero-motion.md | 50 (+trace: landing, airborne Δv, resting y, dashCD 0.58, attack 15 ticks, acceptingInput 139/65) | 47 (+6 trace) | 1 | 0 | 2 (+3 un-numbered: Q9, L143 "only", L69 "only"; +L196 count) | 1 (attackDuration) | ~15 uncited engine sentences (D28) |
| damage-path.md | 47 (+trace §6.1 rows, 67-tick i-frames) | 41 (+2 trace) | 2 | 2 | 2 | 1 | 6 rows with omitted conjuncts (D19) |
| frame-order.md | 28 | 22 (4 with sub-list errors) | 2 | 1 | 2 | 0 | 1 out-of-repo cite; 6 phase attributions over-stated (D30-D33) |
| tk2d-animator.md | 18 (+trace: rec 241, 127-148, 537/538, 585/586, r2_rand1 162-173, 177/178) | 16 (+6 trace) | 1 | 0 | 1 (+L311 count) | 0 | 5 uncited (D43) |
| fsm-runtime.md | 45 | 37 | 2 | 0 | 3 | 5 riders (D44) | DoTransition hinge verbatim at 2340-2343 |
| fsm-actions.md | 56 | 54 | 0 | 1 | 1 | 3 (Q1-Q3) | 105 dump-value checks all match |
| boss-hornet.md | 33 (+10 dump/trace: GG Reset field, invincible column, restartOnEnable, TR@354/@355/@289/@255/@114, r2_move@421-422, §4.4 counts, ground y) | 31 (+7) | 0 | 1 | 1 (+3: D01, D02, D03) | 1 (D07) + 4 (D08) | RNG census incomplete (D05) |

## 6. Ten most consequential defects

1. D01/D02/C1 — boss-hornet: GG Reset does write `IsInvincible=false` (literal in dump; trace False 438/480); Q5 is a phantom that propagates into open-questions Q22 and would send the fsm port looking for a non-existent clearing agent.
2. D05/C9 — RNG census omits the CameraShake FSM: 78 `Random.Range` draws per Hornet dash/sphere and ~153 per knight hit; the P4 seeded-RNG gate is unpassable on the current draw list.
3. D03/C6 — Needle `$Return Vector` is re-captured on every throw (restartOnEnable=true), not fixed at scene start; a port of the spec text returns the needle to the wrong place.
4. D04/C8 — "transform.parent does not preserve world position" (×3 locations) is uncited and contrary to the engine default; porting it teleports Needle/Needle Tink.
5. D44 — fsm-runtime's "assume RestartOnEnable = true" and five "not dumped" statements are stale; 8 FSMs (5 Knight/*) are false and the field is in the JSON.
6. D45 — `FsmState.OnEvent` returns the last action's flag, not an OR; a verbatim port of the spec's wording changes which events reach transitions.
7. D13/C7 — damage-path's "at most 5 staggers" is contradicted by the dump (`MAX` has no transition) and by boss-hornet.
8. D14 — half-to-even rounding is memory-sourced; the 52.5→52 Great/Dash Slash prediction is unsupported (rule 2.2 "plausible wrong constant").
9. G2 — nobody owns the Knight's 13 FSMs (spell/focus/nail-art/super-dash motion, input lockouts, shakes); hero-motion defers to fsm-runtime, fsm-runtime scopes only boss-scene FSMs.
10. D30-D33/D35 — frame-order states Unity phase attributions (physics sub-phase, LateUpdate ordering, InControl slot, timeScale-write timing) as facts the trace does not establish, plus three count errors; the §3.4 step loop is right, the justifications are not.

Verifier-agent tallies were cross-checked by re-opening a sample (SetInvincible.cs:21-40, ActionHelpers.cs:56-71,
GetFsmInt.cs:52-76, SetParent.cs:30-46, DelayedEvent.cs:68-88, FsmEvent.cs system-event count, CheckCollisionSide.cs:127-140,
FsmDumper.cs:176-186, HeroAnimationController.cs missing-clip sites) — all agreed.

## Re-check (round 2) — 2026-08-31, after the authors' `## Review fixes` passes

Method: no sub-agents; every fix row below was re-opened against the decomp/dump/trace by the reviewer.
`harness/tools/cite_check.py`: 0 unresolved / out-of-range on all seven specs (the 9+16 "unresolved" it reports for
`PORT-BRIEFS.md` and this file are cross-references to other specs by bare filename, not source citations).
`analysis/open-questions.md` carries the generated index of 69 `Q-<spec>-n` questions plus the Q22 closure note.

### Round-1 correction to this file
- D05 / §6 item 2 / G1: the draw count per `EnemyKillShake` is **75**, not 78 — `ShakePositionV2.UpdateShaking` is
  called once from `OnEnter` (`:62`) and once per `OnUpdate` (`:68`), `timer += Time.deltaTime` precedes the draw
  (`:81`, `:96`) and the stop test `timer > Duration` follows it (`:98`); f32 `Σ 0.02f` first exceeds 0.5 at the
  25th call (0.50000006), so 25 × 3 = 75. Duration 1.0 → 51 calls = 153 (unchanged); 2.5 → 126 = 378.
  boss-hornet §6.4.1 has it right; fsm-actions §8.3 and damage-path Q-dmg-2 inherited the 78 (R2-3, R2-4 below).

### boss-hornet.md — full re-gate

| id | verified against | result |
|---|---|---|
| D01 | `GG_Hornet_1.json` Control/GG Reset `Invincible {useVariable:false,value:false}`; `SetInvincible.cs:29-32` | FIXED (§2.2 L256-266, §5 L815; old Q5 deleted) |
| D02 | traces: r2_rand1 42/438 (first False idx 42, f24768), r2_rand2 42/438 (f24863), r2_move 42/558 (f24827), r2_idle 42/198 (f24984) | FIXED, numbers exact |
| D03 | dump `Needle/Control restartOnEnable:true, started:true, finished:true, activeState:null`; `Fsm.cs:1847-1853`; Throw idx 6 `SetPosition` < idx 7 `ActivateGameObject` | FIXED (§1.2 L173-182; Q-hornet-4 for the SetActive→OnEnable dependency). Nit: dump has `activeState: null` / `activeStateName: ""`; spec quotes `activeState:""`. |
| D04 | `SetParent.cs:30-46`; Deparent/Setup `resetLocalPosition/Rotation` literal false | FIXED (L135-137, L966-967, Q-hornet-3) |
| D05 | `ShakePositionV2.cs:62,68,81,96,98`; CameraShake globals `EnemyKillShake → To Kill Shake`; `To Kill Shake` = `FloatCompare($Priority,6) lessThan→FINISHED` + `GotoPreviousState`; all 15 `ShakePositionV2` instances `$FPS Limit = 0`; f32 sums | FIXED — **75 / 153 correct**, priority gate correct, `HealthManager.cs:371-372` and `Knight Damage/Gen → AverageShake` paths present (§6.4.1) |
| D06 | `CheckCollisionSide.cs:39` (`public bool otherLayer`), `:131-137` | FIXED (L318, L395, L612-615) |
| D07 | meta.json 29672 / 02:49:29Z | FIXED |
| D08 | scene.json: DamageHero `1/1/false` on Hit GDash, Hit ADash, Sphere Ball, Needle; body 0; Needle `PolygonCollider2D` layer 22 trigger 3 pts; Needle Tink `BoxCollider2D` (3.2466042, 0.251131058) off (−0.070205, −1.9e-06) layer 17 trigger `enabled:false`, no DamageHero (`TinkEffect`); ConstrainPosition `constrainX true, 15.07, 37.96, constrainY false` | FIXED except **R2-1** below (Needle rigidbody) |
| D09 | scene.json Recoil `recoilDuration 0.15, stopVelocityXWhenRecoilingUp false, preventRecoilUp true, recoilSpeedBase 15`; `Recoil.cs:122` | FIXED |
| D10 | Stun Control ints incl. `Decrement`, string `Tag` | FIXED |
| D11 | Needle Tink `Setup` (`SetParent(Owner,null)`, `SetFsmGameObject(Hornet,"Control","Needle Tink")`), `Deparent` (`ActivateGameObject(Needle,false)`, `SetParent(Needle,null)`) | FIXED |
| D12 | `ActionHelpers.cs:70`; `FsmVariables.cs:1304-1308` returns `new FsmInt(name)` on a miss | FIXED (Q-hornet-2). Wording nit: Control *does* declare an int `HP` (value 0), so "neither FSM declares an int `HP`" is wrong; the conclusion (stays 0) holds either way. |
| D13/C7 | `MAX` transition exists only on `Heart Pieces::Set Pieces[Init]` | FIXED |
| C11 | `Wait 3.0` → 150 records (f32 k=151) | FIXED (old Q1 closed) |
| C13 | §4.1 now "after every Update and before LateUpdate" | FIXED |
| old Q2 | rng_probe.json: 281/282 ops advance; sole exception `Range(3,3)` (int) | FIXED, matches the dump |
| Q-hornet-1..10 | present in the index | ✓ |

**R2-1 (new, MISREAD)** — boss-hornet.md L150: "`Needle` … **no Rigidbody2D** (`SCENE#Needle`, `rigidbody: null`)". scene.json
`Needle.rigidbody` = `{bodyType Dynamic, isKinematic false, gravityScale 0, interpolation None, collisionDetectionMode Continuous,
constraints None, mass 1}` and its `components[]` list `UnityEngine.Rigidbody2D` and `PlayMakerFixedUpdate`. The needle IS a dynamic
body (which is what `SetVelocityAsAngle`/`DecelerateV2` act on). Fix: replace with the dumped rigidbody block.

**Verdict: PASS-WITH-FIXES** (was FAIL). One new misread (R2-1) and two wording nits (D03, D12); all round-1 blocking items closed.

### Other specs — spot-checks (≥8 fixes each, against sources)

| spec | fixes verified | new problems |
|---|---|---|
| damage-path.md | D13 (`MAX` only on Heart Pieces), D14 ("52 or 53", Q-dmg-3), D15 (0.02 / 02:49:29Z), D16 (`:21-36`; LimitSendEvents 12-15/17-31/33-41), D17 (`:3377-3388`, `:3419-3422`), D18, D19 (`:1853 hazardType==1`, `:1919 !flag`, `:1970`, `:1990`, `:2317 !controlReqlinquished`, `TakeDamage.cs:95`), D09 (Recoil live values; `Sweep.cs` exists), Q-DMG-4 (Slash `longnail/mantis true, fury false`; WallSlash all false), Q-DMG-5 (0 `DamageEnemies`), C1 (42/438, 42/558, 42/198) — all confirmed | **R2-3** Q-dmg-2 (L1100) says ≈78 per boss dash/sphere → 75. **R2-5** Q-dmg-5's premise is contradicted by the dump: `ListenForUp`/`ListenForDown` emit plain `FsmEvent` fields (`wasPressed/wasReleased/isPressed/isNotPressed`, `ListenForUp.cs:10-16`), which the dump serialises — `Knight/Nail Arts/Move Choice`: ListenForDown `isPressed → CYCLONE`; ListenForUp `isPressed → CYCLONE`, `isNotPressed → GREAT SLASH`; only `eventTarget` is an `FsmEventTarget`, and its `__fields` carry `target/excludeSelf/gameObject/fsmName/sendToChildren` (fsm-runtime §3.6, 2045/2045). Close Q-dmg-5 (and drop the "event name is invisible" sentence). |
| frame-order.md | D30 (owner census incl. Evade Check 62/418, Floor Check 0/164; Q-frame-1), D31 (Q-frame-2), D32 (open interval; Q-frame-3), D33 (corollary withdrawn; Q-frame-4), D34 (71/303; guard `:3855`), D35 (3/3, 32/32), D36 (`:414`, `:748-759`), D37 (out-of-repo cite removed), D38 (50+1=51; Knight/* 8), D39 (`_phase` writers `:35` init 3, `:256`, `:259`, `:279`, `:285`, `:286`, `:357`, `:429` — exact), C2 (302/242) — all confirmed | **R2-2 (MISREAD, introduced by D35)** — §4 L447-450: the 8th no-effect jump press (corpus 144, STEP 145 at f25195) is explained as "a wall-jump ramp already in flight". The trace says otherwise: `HERO_DAMAGE` at f25193/f25194, FRAME f25194 `cState.recoiling=1, hero_state=7 (no_input)`, and the F+1 `HC_FIXED_POST` velocity (+15.000, +7.500) is exactly `recoilVector = (RECOIL_VELOCITY, RECOIL_VELOCITY·0.5)` (HeroController.cs:3794); `wallJumping`/`wallLocked` are 0. The conclusion (the press cannot execute — `CanJump` is false in `no_input`) stands; the mechanism named contradicts hero-motion Q-hero-2 (0 wall-jump frames in the corpus). Nit: "68 shake-send sites" — 69 `SendEvent*` actions with a `*Shake` name in the dump (the disabled `Throw` site is presumably the difference). |
| fsm-actions.md | D04 (Q-fsmact-11), D50 (`SendEvent.cs:37-43`), D51 ("highest", `:43-50`), D52 (§0.6 `ActionHelpers.cs:56-71`, `:70`), D53 (Q1-3 → Q15), D54 (26/558 disabled — exact), D55 (`GameManager.cs:2860-2885`; `ObjectPool.cs:472-526` orig_Spawn; `PlayMakerUnity2DProxy.cs:143,190`), D56 (`ActionHelpers.cs:83` strict `<`), §8.3 priority gate / `FpsLimit=0` (15/15) — confirmed | **R2-4 (count)** §8.3 table L818/L821 and prose L841-857/L1177: the Duration-0.5 rows use "26 updates × 3 = 78"; the rule the same table applies to Duration 1.0 (51 → 153) and 2.5 (126 → 378) gives **25 → 75** for 0.5. Contradicts boss-hornet §6.4.1 (75). |
| fsm-runtime.md | D44 (954/8; 7539 states, 0 breakpoints; globals 30 GameObject + 1 Bool, 51 events; `FsmDumper.cs:178-186`, `:116-130`), D45 (`FsmState.cs:321/326/328`), D46 (55), D47 (`Fsm.cs:1598-1601`), D48 (`Fsm.cs:58-59`; dumper `:148`, `:158-163`, `:199`, `:230-271`, `:263-264`), D38 (`CheckTrackTriggerCount.cs:42`), G2 (12 Knight FSMs: hero.json 12/29 components, dump 12 at path `Knight`), G11 (2045/2045 `__fields.target`; GameObject 1031 / Self 851 / BroadcastAll 122 / GameObjectFSM 41 — exact) — all confirmed | none |
| tk2d-animator.md | D40 (`tk2dAnimatedSprite.cs:61-71` static `g_paused`; `:73-77` instance), D41 (paths), D42 (§2.5: 7 sites; `:407` has both `Backdash Land` and `Backdash Land 2`; `:416 Lookup`), P3 dead (`playBackDashToIdleEnd` only at `:21/:147/:151` in all of Assembly-CSharp), D43 (`Turn` LateUpdate 4 → 1.5999999 / frame 1; `docs/float-parity.md:10` `-ffp-contract=off`), 12 Knight FSMs — all confirmed | none |
| hero-motion.md | D21 (attackDuration mutable, 0 at dump), D22, D23 (`:3539-3543`, `:3551-3558`), D24 (302), D25 (Q-hero-9 coroutine), D26 (`:4289`, `:4297`, Obsolete), D27 (324/347), D28 InControl rewrite (`PlayerTwoAxisAction.cs:23-45` Obsolete no-op setters, `:57 Raw = true`; `TwoAxisInputControl.cs:181`; `InputDevice.cs:141`, `:708-714`, `:414 DPad.DeadZoneFunc = DeadZone.Separate`; `DeadZone.cs:16-26`), D29 (`RegimeTweaks.cs:30-36` env-armed, `:61-75` sets None on hero + HMs; ports None), G2 §3.2c handles (`HC:815 Superdash`, `:820 Thorn Counter`, `:830 Spell Control`, `:5015 ProxyFSM`), G7/Q-hero-4 (`Hornet Saver/Colliders` bounds centre y 17, size 20 → top 27.0) — all confirmed | **R2-6 (STALE cross-spec line cites)** — hero-motion now cites other specs by line number, and those specs were revised in the same pass: `analysis/specs/frame-order.md:48-50` (now census rows, not the 0-inversion statement), `:141-142` (now the phase==0 bullet), `:152-155` (owner census), `:167` ("Proven by the traces" header), `:192` ("After HC_UPDATE_POST"), `:248-253` (§3.1 table), `:390-405` (§3.4 loop); `analysis/specs/damage-path.md:888-890` (now Stun Land text, not the 15-tick attackDuration measurement). `cite_check.py` cannot catch these (the ranges exist). Fix: cite section anchors (§n.m) or re-point after every edit. |

### Port-blocking gaps G1–G15 → Q coverage

G1 Q-fsmact-10 / Q-hornet-1 / Q-dmg-2 · G2 fsm-runtime §5 + Q-fsmrt-5 + Q-hero-8 (count is 12) · G3 Q-hornet-3 / Q-fsmact-11 ·
G4 Q-hornet-4 / Q-fsmact-4 · G5 Q-hornet-5, Q-frame-1/3/6/7, Q-hero-12 · G6 Q-dmg-11 / Q-fsmact-12 / Q-frame-1 · G7 Q-hero-4 (+Q16;
the Hornet `Land Y` 27.55 → 28.5619 two-frame resolve has no named Q — covered only by Q16's generic "pin by experiment") ·
G8 Q-dmg-3 · G9 Q-dmg-8 · G10 Q-hornet-7 / Q-fsmact-7 · G11 rejected with evidence (fsm-runtime §3.6) — but damage-path Q-dmg-5
still asserts it (R2-5) · G12 Q-hero-11 · G13 Q-fsmact-1/2/3/6, Q-hornet-6/8 · G14 Q-dmg-9 · G15 Q-dmg-6/7/10. No gap lacks a Q.

### Final verdicts (round 2)

| spec | verdict | residual |
|---|---|---|
| boss-hornet.md | PASS-WITH-FIXES | R2-1 (Needle has a Rigidbody2D); wording nits in D03/D12 rows |
| damage-path.md | PASS-WITH-FIXES | R2-3 (≈78 → 75 in Q-dmg-2); R2-5 (close Q-dmg-5: ListenFor* events are serialised) |
| frame-order.md | PASS-WITH-FIXES | R2-2 (corpus-144 is damage recoil, not a wall-jump ramp); 68 → 69 nit |
| fsm-actions.md | PASS-WITH-FIXES | R2-4 (Duration-0.5 rows 26×3=78 → 25×3=75) |
| fsm-runtime.md | PASS | — |
| tk2d-animator.md | PASS | — |
| hero-motion.md | PASS-WITH-FIXES | R2-6 (cross-spec line citations drifted; content correct) |

Gate: every round-1 blocking defect is closed and no round-2 residual is definitional except the 75-vs-78 constant (R2-3/R2-4),
which a sim/fsm worker would copy verbatim — fix before P4 kickoff; P3 (hero/phys) is unblocked now.
