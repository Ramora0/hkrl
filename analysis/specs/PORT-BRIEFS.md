# P3–P5 port worker briefs (orchestrator-owned; issued when the P1 gate passes)

Common rules for every port worker (PLAN.md 2.1–2.5, owner addenda):
- The sim is a PORT of decompiled code + dumped data. Specs in `analysis/specs/` are a cited index into
  `analysis/decomp`, `analysis/dumps/<scene>`, `analysis/fsm/<scene>.json`; when a spec and its cited
  source disagree, the source wins and you note it in `analysis/specs/REVIEW-p1.md` (append-only section
  "Port findings"). Traces (`analysis/traces/p0/r2_*.hktrace`) verify; they never define behaviour.
- Every constant and rule in `sim/` carries a comment `// cite: <path>:<lines>` or `// cite: dumps/<scene>/<file>#<key>`.
  A value you cannot cite is written as `HKSIM_UNKNOWN(...)` which aborts at runtime with a message naming it,
  and gets a Q entry appended to `analysis/open-questions.md` (append-only, `## Qnn — ...`).
- Language: C11, gcc, flags in `sim/CMakeLists.txt` (do not change). Native Unity/Box2D code: float32 per
  operation. Ported C# (Mono): each statement evaluates in DOUBLE and rounds to float32 once per C# float
  store / float argument (docs/float-parity.md amendment 2026-08-31, measured 542/542 payloads). No FMA.
- Ownership is exclusive: write only inside your directory + your test file; the C ABI in `sim/core/hksim.h`
  and internal interfaces in `sim/core/*.h` are orchestrator-owned — request changes via your report.
- Regime R2 only: dt = fixedDt = 0.02, frames_per_wait 2, per step exactly 1 frozen frame (Update with dt 0,
  action applied after it, no FixedUpdate) + 2 live frames (FixedUpdate → physics → Update → LateUpdate).
- Build: `cmake -S sim -B sim/build -G Ninja && cmake --build sim/build`; tests are C executables or
  python scripts under `harness/tests/`; run them before reporting.
- Never `cd X && cmd`; absolute paths. Never touch `C:\Users\Lee\coding\CSharp\HK\FullKnight`.

## Worker H — `sim/hero/` (HeroController port)
Sources: `analysis/specs/hero-motion.md`, `damage-path.md` §hero, `frame-order.md`; decomp
`HeroController.cs`, `HeroControllerStates.cs`, `InputHandler.cs`, `HeroBox.cs`, `NailSlash.cs`;
`dumps/<scene>/hero.json` (inspector constants — the only source for 325 fields), `playerdata.json`.
Deliver: `hero_state` struct mirroring the recorded HeroController primitive fields + cState bits (same
names as the trace header, so the trace writer can emit them); `hero_update(dt)`, `hero_fixed_update()`,
`hero_apply_input(bits)`, the 9 `Can*` predicates, TakeDamage/Invulnerable/StartRecoil, attack timing
(NailSlash collider window), coroutine-equivalents as explicit step counters (cite the WaitForSeconds
rule used). Physics is used only through `sim/core/phys.h` (orchestrator-owned contract; sim/phys implements it); contact/trigger events arrive from `phys_events()` after each `phys_step`. The Knight's own PlayMakerFSMs (spells, focus, nail arts, superdash, ProxyFSM, acceptingInput lockouts) belong to worker F — you port HeroController C# and name the hand-off points. Untraced
paths (wall slide, hard land, spells, focus, superdash…) are ported from code and marked `UNVERIFIED`.
Gate P3: hero-only corpora (idle, walk_lr, jump_mix, dash_mix, attack_mix, movement_all) replay against
`r2_*` traces with DH = full length on the hero fields (boss entities excluded by the harness).

## Worker F — `sim/fsm/` (PlayMaker runtime + action library + HK components)
Sources: `fsm-runtime.md`, `fsm-actions.md`, `boss-hornet.md`, `tk2d-animator.md`; decomp
`PlayMaker/HutongGames.PlayMaker/{Fsm,FsmState,FsmTransition,FsmEvent,...}.cs`, `PlayMakerFSM.cs`,
`PlayMakerFixedUpdate.cs`, `Assembly-CSharp/HutongGames.PlayMaker.Actions/*.cs`, `HealthManager.cs`,
`DamageHero.cs`, `Recoil.cs`, `HitTaker.cs`, `ConstrainPosition.cs`, `tk2dSpriteAnimator.cs`,
`ActionHelpers.cs` (incl. the missing-FSM fallback); `analysis/fsm/<scene>.json` (states, transitions,
actions with parameter values; `useVariable:true`+`name:null` = None), `dumps/<scene>/globals.json`.
Scope includes EVERY FSM in the dumped scene that can affect the observed state: Hornet, Boss Holder / BossSceneController, camera shake (RNG!), the Knight's 13 FSMs, spawned objects (Needle, Sphere Ball...). Deliver: data-driven interpreter (`fsm_def` static tables compiled from the JSON by a Python generator
you own in `sim/fsm/gen/`, emitted as C tables; `fsm_rt` mutable state), exact update/transition/event
semantics (synchronous dispatch, `switchToState` deferral, sender-identity commit rule, loopCount
guard, restartOnEnable), the action library for every type used by the four dumped scenes (unported
type → `HKSIM_ERR_UNIMPLEMENTED` naming the type/state), RNG draws in the cited order (audio included),
tk2d animator with LateUpdate completion events, HealthManager/DamageHero/Recoil/iframes.
Gate P4: `r2_rand1`/`r2_rand2` replay with DH = full length on Hornet's `Control`/`Stun Control`
active state + FSM vars + entity pose (given the hero port), seeded.

## Worker P — `sim/phys/` (2D physics for the HK subset)
Sources: `dumps/<scene>/physics.json` (gravity, iterations, layer matrix, materials), `scene.json`
(all colliders with world geometry), `hero-motion.md` §contact solver, `boss-hornet.md` §physics,
Q16 in `open-questions.md`. Unity Physics2D is native (no decomp): behaviour is pinned by experiment
against traces + documented per rule with the experiment cited (the one sanctioned exception to 2.2).
Implement exactly the interface in `sim/core/phys.h` (request changes via your report, do not edit it). Deliver: bodies (static / kinematic / dynamic, non-rotating), Box/Circle/Polygon/Edge shapes with
edgeRadius, layer-matrix filtering, trigger overlaps, contact enter/stay/exit events in Unity's order
(as measured), semi-implicit Euler with per-body gravityScale, `Continuous` sweep for the hero,
raycast (for `CheckCollisionSide`), `MAX_FALL_VELOCITY`-style clamps left to callers.
Gate: hero corpora rest/land/wall cases bit-match `rb_pos`/`rb_vel` (hero-motion.md table), and the
harness ULP-perturbation test still trips.

## Core (orchestrator, may delegate pieces) — `sim/core/`
Done: `rng.{h,c}` (bit-exact), `phys.h` contract, harness `--subset` mode, `harness/sim_driver.py`.
RNG (xorshift128 + pinned mappings, Q15), frame scheduler (R2 pattern), trace writer (schema v1 subset),
obs packer (P6, byte-identical to `Net/BinaryProtocol.cs`), scene loader for the compiled tables, the
C ABI, harness driver `harness/sim_driver.py` (ctypes → `.hktrace` → `divergence.py` with
intersection-schema comparison).
