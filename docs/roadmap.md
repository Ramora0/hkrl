# Roadmap

The goal is one policy that beats every Godhome boss. The sim covers that goal only if it runs what each
arena's scene runs, not just what a boss visibly needs. The roster is every (scene, tier) the game's
`BossStatue -> BossScene` assets define: 135 arenas over 57 scenes
(`analysis/dumps_all/_roster/boss_roster.json`). The tier matters beyond HP scaling:
`HealthManager.GetScaledHP`, Radiant one-hit damage, and `CheckGGBossLevel` branches in most boss FSMs.

## Why per-boss defects keep appearing

1. **No single component lifecycle.** Each component type had its own hand-written Start and a hand-placed
   update slot. Unity has one player loop with one set of rules (`docs/engine-lifecycle.md`).
2. **Porting what bosses need, not what the scene runs.** Actions, components and live FSMs were added one
   boss at a time. Anything the scene runs that the port skipped fails silently.
3. **Hand ports are checked only by whole fights.** A defect is found as a divergence hundreds of steps into
   a policy fight. No test pins an action's inputs to its outputs.
4. **Untested instruments.** The recorder, the corpora and the gate were trusted without controls. Some
   corpora were recorded under a broken environment, and some arenas do not reproduce their own recordings.

## Plan, in order

1. **Inventory and completeness gate.** Enumerate every component class and PlayMaker action in all 135
   arenas from the dumps. Each is either ported or an explicit, checked exclusion. It must be ported if it
   changes gameplay state or the observation. How many `Random` draws it makes is not a criterion. The
   generators refuse anything undecided and emit sorted output. Done for the ported scenes:
   `sim/fsm/gen/completeness.py` holds the ported classes, the exclusions with their evidence, the prefabs
   no save can spawn and the action types excluded to their unreachable roots; `gen_tables.py` stops on
   anything else, and every FSM and ported component on an active object ticks.
2. **Lifecycle recorder.** Done: `oracle/Record/LifecycleRecorder.cs`, rules in `analysis/lifecycle/rules.json`
   (`tools/lifecycle_rules.py`).
3. **One lifecycle dispatcher.** Done: `sim/fsm/lifecycle.c` schedules every component by the measured rules.
   Acceptance: `tools/lifecycle_compare.py`.
4. **Episode-start check for all arenas.** For every (scene, tier), the sim's state at the first step
   matches the game's. The tier is carried into the generators.
5. **Mass port.** Port the remaining backlog with source spans. Each port gets a unit test driven by
   recorded inputs and outputs, and a reviewer who is not its author.
6. **Native remainder.** Physics2D glue and Unity's changes to its Box2D 2.3.1 fork, Mecanim animators
   (Grey Prince Zote, Hive Knight, Hollow Knight, Radiance), and the remaining coroutines.
7. **Acceptance.** For as long as the game reproduces its own recording, the sim matches it. Past that
   point, a discriminator cannot tell sim fights from game fights. The final test is the sim-trained
   policy's win rate in the real game. This replaces the first-divergence gate as the bar.

## Open problems

- **Backlog of unported actions.** 66 action types under boss subtrees in the fight arenas are unported.
  Arena features no ported boss has needed yet: Mecanim animators, and scan-route boss binding.
- **Rotating bodies.** `sim/phys` has Box2D's angular dynamics (Q-pphys-19). Hornet 2's broken barb (`Hornet
  Barb` Control `Break`, `SpinSelfSimple.DoSpin`) still traps: the component's `spinFactor` is not compiled.
- **Coverage.** Boss states no corpus visits are unverified code. That is about half of them on
  Soul Master and Mega Moss Charger.
- **Damage registration.** On Gruz Mother, the game registers a Howling Wraiths head hit on the boss where
  the sim, with identical geometry, registers none. A spell also deals 35 in the game and 30 in the sim.
  The candidates are trigger/hit bookkeeping, the collider shape behind the AABB, and the `damages_enemy`
  `Multiplier` on `Knight/Spells/Scr Heads/Hit R`.
- **Hero movement in an air attack.** On Gruz Mother V the knight moves right at 3.75 in the game and left
  at run speed in the sim, from identical cState flags. Localise against `HeroController.cs`.
- **Soul Master step 0.** The boss's position differs at the first step: Mage Lord's teleport pose between
  SceneReady and the first recorded frame. It is not caused by the dump.
- **Markoth row set.** Markoth's two `Shield` colliders (DamageHero, layer 11) are rows on both sides now that
  membership is one predicate on the live world; confirm on a new recording.
- **Warrior Dream activation (Xero, No Eyes, Hu, Gorb).** The fix is the Start-on-enable rule (R1) applied
  to PlayMakerFSM, tk2dSpriteAnimator and iTween, which the lifecycle dispatcher now implements. Confirm it
  on the gate.
- **Restore leftovers.** The `Knight/Charm Effects | Slash Size Modifiers` cold-start override sits in
  `fsm_world.c` (`SNAP_COLD_OVERRIDE`). Seed `NailSlash.longnail`/`mantis` from the dumped PlayerData
  instead (`hero_dump_init.c`) and delete the override. Re-dump GG_Hornet_1 and GG_Mega_Moss_Charger
  post-intro so the wake replay can go. Two questions remain undecided: whether OnEnter side effects
  (`SetPosition`, `SetParent`, cross-FSM `SetFsm*`) are safe to re-run during a restore, and whether
  DontDestroyOnLoad FSMs should be restored at all.
- **Lifecycle gaps.** Component instance ids are synthetic (A-1, A-22): the dumper emits `GetInstanceID()` and the
  index of every component, and a re-dump of the ported scenes is pending. `PersonalObjectPool.Start` (a pool
  created when its owner is first activated) is not modelled.
- **Physics not yet ported from the native decompile** (`analysis/specs/port-phys.md` "Not ported"): body sleep
  (Q-pphys-8, conformance CF-13). Beyond any source: the broad-phase tree at scene load,
  whose proxy ids order the pairs one batch creates (U2; the native-state dump N0 would supply it).

## Instrument problems

- **Arenas that do not replay.** GG_Ghost_Gorb, GG_Ghost_Xero and GG_Grimm_Nightmare do not reproduce their
  own recordings (same script, same seed), in recordings made before the mod fixed each step's frame count
  (`docs/frame-order.md` "One agent step"). Re-record them to see whether they still diverge. The wall-clock
  reads a fight can reach are listed in `docs/frame-order.md` "Wall-clock reads"; `HK_ORACLE_WALLCLOCK=1`
  records which fire. Other candidates: iTween's `Time.deltaTime` accumulation and
  `AudioPlayRandom`. Gate numbers past that point are noise, so read every gate next to the game-vs-game
  control.
- **Legacy corpora.** Every corpus on disk predates the provenance stamp and the one configuration, so the
  gates label it LEGACY and replay it in the one configuration: its dump and mod are unverified, the
  `polbat_*` corpora were recorded in script mode at fpw 2 without the opt-in switches, and the two NKG `ws`
  sets in `runs/gap/hkba` most likely with them (the game eval set them; nothing recorded it). Re-record them
  stamped (`tools/rerecord.py`, `tools/record_corpus.py`) with a mod deployed from a clean checkout.
  `polbat_GG_Soul_Master` was recorded from a dirty reset, and
  `polbat_GG_Gruz_Mother_V` with the tier clamped to Attuned.
- **Game-vs-game control.** `gate/control.py` needs two recordings of one corpus. The only pairs are the
  `analysis/lifecycle/<scene>` re-recordings of four episodes each (lifecycle recorder on): False Knight,
  Hornet 1, Mega Moss Charger and Nosk reproduce their recordings to the end in script mode at fpw 2, so
  every sim-vs-game difference on those episodes is the sim's; Xero does not. There is no control yet for
  fpw 1 policy recordings.
- **`gate/boss_gate.py`** can die with empty output while another sweep runs. Gate on an idle machine.
