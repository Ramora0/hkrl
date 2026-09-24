# port-rng — RNG decoupling: per-site generators, oracle injection, and the anti-oversync guards

Status: DESIGN, not yet applied. Owner decision, 2026-08-31.

## 0. The owner's decision

> "give up matching the RNG state through draw calls and just wherever RNG is needed use an
> arbitrary RNG generator and copy the value from HK; matching the exact calls to RNG state in
> order is a brutal task that at the end of the day doesn't matter"

and, on scope:

> "This should allow us to gut unrelated systems (audio, anything else) and try and match the exact
> transitions from the HK instance as close as possible, 100% confirming any synced thing is purely
> RNG; if we sync too much we could silently fail cases, so be extremely cautious here. Make sure we
> get the exact values taken from the code after the RNG so we're not silently skipping necessary
> code."

Two requirements follow, and they pull against each other. The design below satisfies both:
inject enough that draw ORDER stops mattering; inject so little that no defect can hide behind
the injection.

## 1. What is actually wrong today

`UnityEngine.Random` is ONE global xorshift128 stream. Every draw in the game — a jump-sound pitch,
a particle angle, a camera shake offset — advances the same stream that Hornet's `SendRandomEvent`
reads from. One missing or extra cosmetic draw shifts every later value, so a perfectly correct
boss port still makes the wrong decision.

Measured consequence (`harness/tools/rng_oracle.py`, HEAD 4d8029a):

| corpus | HK draws | sim draws | sim shortfall |
|---|---|---|---|
| r2_attack | 548 | 457 | -91 |
| r2_dash | 829 | 722 | -107 |
| r2_idle | 42 | 42 | 0 |
| r2_jump | 1168 | 907 | -261 |
| r2_move | 449 | 405 | -44 |
| r2_rand1 | 1045 | 914 | -131 |
| r2_rand2 | 960 | 710 | -250 |
| r2_rand3 | 846 | 829 | -17 |
| r2_walk | 914 | 822 | -92 |

We under-draw everywhere. Under a shared stream that is fatal; under per-site generators it is
irrelevant for every cosmetic site, and an explicitly reported defect for every decision site.

## 2. Verified facts this design rests on

Each was measured, not assumed.

- **F1. Every HK draw value is recoverable from the corpora we already hold.** `FRAME.rng` records
  the 4-word generator state each frame; the generator is a pure function, so stepping it between
  consecutive snapshots recovers the exact words drawn. 6801/6801 draws recovered across all nine
  corpora, zero unrecoverable gaps (`harness/tools/rng_oracle.py`). No re-recording is required.
- **F2. The decision surface is three draw sites.** Of 37 `hk_rng_*` call sites in `sim/`
  (`findings/rng-sites.md`), exactly three can change behaviour:
  - `sim/fsm/act_core.c:24` `random_weighted_index` — `SendRandomEvent` / `V2` / `V3`
  - `sim/fsm/act_core.c:62` `waitrandom_enter` — `WaitRandom`
  - `sim/fsm/act_core.c:327` `rf_enter` — `RandomFloat`

  One further site is physics-relevant: `sim/hero/hero_damage.c:455`, the Carefree Melody shield
  roll. The remaining ~32 are cosmetic (audio pitch, particle counts/angles, camera shake, corpse
  effects, nail-impact rotation).
- **F3. Site identity is already in scope at all three decision sites.** `act_inst`
  (`sim/fsm/fsm.h:38-43`) carries `fsm`, `state`, and `index`, so `owner|fsm|state|action_index` is
  derivable with no refactoring.
- **F4. The first draw-frame divergence is not an RNG defect.** In 8 of 9 corpora the earliest
  misaligned draw is Hornet's opening `GG Land -> GG Reset -> GG Music -> Flourish` burst, executed
  exactly 2 frames EARLY by the sim. That is a boss-start timing bug, tracked separately; it will
  not be fixed by this work and must not be masked by it.

## 3. Architecture

### 3.1 Site keys

Every draw site gets a stable identifier.
- FSM action sites: `owner_path|fsm_name|state_name|action_index` (derivable per F3).
- Non-FSM sites (hero audio, damage, tk2d, HealthManager helpers): a static string constant.

### 3.2 Three modes

- **`RNG_GLOBAL`** — today's single shared stream. Retained so the migration can be proven a no-op
  before anything else changes.
- **`RNG_INDEPENDENT`** — the shipping mode. Each site owns a generator seeded from
  `hash(master_seed, site_key)`. No site can perturb any other. Deleting a cosmetic system becomes
  behaviourally free.
- **`RNG_ORACLE`** — verification only. A site with an attributed HK value uses it; any site
  without falls back to `RNG_INDEPENDENT`. Never the shipping mode, never the canonical gate.

### 3.3 Oracle attribution

Built offline from the recorded traces. Draws are attributed to sites using per-frame draw
reconstruction (F1) plus the co-located `FSM_TRANSITION` records. Where an assignment is not unique
the draw is marked AMBIGUOUS and left un-injected — it must trap, never guess (PLAN.md rule 2.3).

## 4. The anti-oversync guards

These exist to answer "if we sync too much we could silently fail cases". Each is a hard rule.

- **G1. Inject the raw 32-bit word, never the cooked value, and never the outcome.**
  All ported arithmetic downstream of the draw still executes: the `/8388607.0f` of
  `hk_rng_value`, the `u*lo + (1-u)*hi` of `hk_rng_range_f`, the modulo of `hk_rng_range_i`, and
  the weight-table walk in `random_weighted_index`. Consequence: a wrong range, a wrong weight
  array, or a mis-ported action still produces a wrong result and is still caught. We inject the
  INPUT to a decision, never the decision itself. This is the literal implementation of "get the
  exact values taken from the code after the RNG so we are not silently skipping necessary code".
- **G2. Every unconsumed oracle draw is a reported defect.** If HK drew at an attributed site and
  the sim never drew there, the accounting prints it. This turns "we silently skipped a code path"
  — currently invisible — into a named, per-site report. A NEW detector, not a relaxation.
- **G3. Every sim draw at an attributed site with no oracle entry is likewise reported.** Catches
  invented draws and mis-ported action semantics.
- **G4. The canonical gate does not change.** `harness/tolerances_p4.json`, `harness/divergence.py`,
  `harness/tools/gate.py` and `harness/tools/boss_seq.py` are untouched. `RNG_ORACLE` is an extra
  diagnostic profile reported alongside, never a replacement. Weakening a grader to make a number
  improve is already banned and stays banned.
- **G5. Weights get their own verification, because injection stops testing them.** Under G1 a
  wrong weight array is still caught whenever the injected word lands in a differing bucket, but
  not reliably. The weight arrays are literal values in `analysis/fsm/GG_Hornet_1.json`, so they
  are verified statically by reading the dump — a direct check, not a sampled one.
- **G6. Bit-exactness is kept where it is already free.** Hero physics and the observation packer
  stay bit-exact and keep their existing tests. This change is scoped to RNG.

## 5. Staging

Each stage is independently verifiable and independently revertible.

- **S0 — DONE.** Draw reconstruction: `harness/tools/rng_oracle.py`. 6801/6801 (F1).
- **S1 — diagnostic, no sim change.** Per-frame and per-draw-index rec-vs-sim alignment reporting.
  Quantifies how much of the boss_seq failure is draw ordering versus real defects, before any code
  moves.
- **S2 — plumbing, provably a no-op.** Introduce site keys and the mode switch with the default
  left at `RNG_GLOBAL`. Acceptance: `harness/tools/measure.sh` output byte-identical to HEAD.
- **S3 — oracle mode.** Attribution table + `RNG_ORACLE` + the G2/G3 accounting report.
- **S4 — gut the cosmetic systems.** Deletion list from `findings/rng-only-systems.md`, one system
  at a time. Acceptance per deletion: oracle-mode numbers unchanged. `world_cold_start`'s 52 foreign
  pre-SceneReady draws (`sim/fsm/fsm_world.c:1269`) become unnecessary and are removed here.
- **S5 — flip the default to `RNG_INDEPENDENT`** and add the G5 static weight check.

## 6. What this costs

Bit-exact whole-trace replay stops being the methodology for anything downstream of a random
decision. That is the owner's explicit call, on the grounds that stream-position fidelity does not
transfer. In exchange, every remaining divergence under `RNG_ORACLE` is a real defect rather than
an ambiguity, and verification survives to arbitrary episode length instead of dying at the first
shifted draw.
