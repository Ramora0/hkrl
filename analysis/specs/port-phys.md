# sim/phys — Unity Physics2D: Unity's Box2D fork and its callback machinery

Scope: the subset of Unity 2020.2.2f1 Physics2D that the dumped scenes use, implemented in `sim/phys/*.c`
against the contract `sim/core/phys.h`.

- **The native decompile is the authority.**  Unity's player (`analysis/decomp_native`, full private PDB) carries
  its Box2D fork and the glue (`Rigidbody2D`, `Collider2D`, `PhysicsContacts2D`).  Their rulings are
  `analysis/native_specs/native-box2d.md` and `native-physics2d.md`; `sim/phys` cites the functions as
  `UP!<addr> <symbol>`.  Where the fork is unchanged from Box2D 2.3.1, the upstream `file:line`
  (`analysis/upstream/box2d-v2.3.1/Box2D/Box2D`) is cited instead.  Where the port and the fork differ with no
  entry under "Not ported" below, the port is wrong.
- **Measurements check the port.**  The E sections are the trace measurements that confirm the numeric
  translation bit-exactly (`tests/test_phys.py` replays them); the engine conformance probes
  (`docs/engine-lifecycle.md` R5, root/conf `tests/test_conformance.py`, `analysis/conformance/`) and the
  recordings confirm the callback machinery.  Where a measurement and the native reading disagree, the
  disagreement is listed under "Conflicts".

Sources (citation keys):

| key | file |
|---|---|
| `UP!<addr> <symbol>` | `analysis/decomp_native/src/{box2d,physics2d}/*.c` (see its README) |
| `b2World.cpp:<n>` etc. | `analysis/upstream/box2d-v2.3.1/Box2D/Box2D/{Collision,Dynamics,Common}/...` |
| `phys.json#…` | `analysis/dumps/GG_Hornet_1/physics.json` (`#Physics2D`, `#rb2d`, `#heroColliders`, `#layerCollisionMatrix`) |
| `scene.json#…` | `analysis/dumps/GG_Hornet_1/scene.json` → `colliders[path=…]` |
| `bosses.json#rb2d` | `analysis/dumps/<scene>/bosses.json` → `healthManagers[].rb2d` |
| `r2_<name>.a.hktrace@f<frame>` | `analysis/traces/p0/r2_{move,idle,rand1,rand2}.a.hktrace`, record at Time.frameCount |
| lifecycle corpus | `analysis/lifecycle/<scene>/*.lifecycle.gz` (`docs/engine-lifecycle.md`): every physics callback the game dispatched, in order |
| conformance | `analysis/conformance/2026-09-23-drain` (101 probe scenarios, 4 game processes) |
| `HC:<n>` | `analysis/decomp/Assembly-CSharp/HeroController.cs:<n>` |

Regime: R2 only (fixed dt 0.02, one physics step per live frame, `rb2d.interpolation = None`).

## The translation

| the fork (native) | sim/phys |
|---|---|
| `b2DynamicTree` / `b2BroadPhase` (2.3.1 unchanged, native-box2d.md §4): one LIFO node pool for leaves and internal nodes, fat AABBs (0.1), `MoveProxy` re-inserting only when the tight box leaves the fat one and stretching it by 2 x displacement, the move buffer, `UpdatePairs` (query every buffered proxy, sort by `(min id, max id)`, dedupe) | `phys_broadphase.c` |
| `b2ContactManager` (`UP!0x180bac390` OnContactCreate, `UP!0x180bab360` Collide, `UP!0x180babb80` Destroy): each contact is appended to `m_contactsNonTOI` or, when it is solid and a body is a bullet (Continuous) at creation, to `m_contactsTOI`, for its whole life; prepended to `m_contactList` and to both bodies' edge lists; `Collide` walks the non-TOI array then the TOI array, sensors included, destroying contacts whose fat AABBs separated (swap-remove, the moved contact examined next) | `phys_world.c` `add_pair`, `collide_array`, `contact_destroy` |
| `b2Contact::Update` (`UP!0x180bb0180`): overlap for sensors, manifold + warm start by contact id for solids, BeginContact / EndContact on a touching change, PreSolve when touching; `b2Contact::Create`'s registry (`UP!0x180baf780`): fixture A is the lower proxy id's for a same-kind pair, the edge / polygon for mixed kinds | `ph_contact_update`, `registry_swaps` |
| `b2World::Step` (`UP!0x180baf000`): `UpdatePairs` first only after a `CreateFixture` (`e_newFixture`), `Collide` x2, `Solve`, `SolveTOI`, ClearForces, then the fork's trigger pass over `m_contactList` (newest first) on the end-of-step poses | `phys_step`, `update_sensors` |
| `b2World::Solve` (`UP!0x180bad930`): islands seeded in `m_nonStaticBodies` array order (append on create, swap-remove), stack DFS over newest-first edges taking contacts with a dynamic side; then `SynchronizeFixtures` in array order and `UpdatePairs` | `phys_solver.c` `ph_solve_islands` |
| `b2Island::Solve` (`UP!0x180bad210`): 2.3.1's integration and solver order (E1, E3), `b2_maxTranslation` = `maxTranslationSpeed` per step (100) | `island_solve` |
| `b2World::SolveTOI` (`UP!0x180bae240`): minimum over `m_contactsTOI` in array order, a bullet body required on either side (native-box2d.md §5.3); the rest is 2.3.1 | `ph_solve_toi` |
| `b2ContactSolver`, `b2TimeOfImpact`, `b2Distance`, `b2CollidePolygons`, `b2EPCollider` (2.3.1 operation for operation, native-box2d.md §7, §10, §11) | `phys_solver.c`, `phys_distance.c`, `phys_collide.c`, `phys_collide_edge.c` |
| `b2CollideRadialPolygons` (`UP!0x180ba2a60`), Unity's, for a polygon pair or a chain child (`SetAsEdge`, `UP!0x180bafea0`) with a radius above the contact offset (U9) | `phys_collide.c` `collide_radial_polygons` |
| `b2PolygonShape::Set` (`UP!0x180ba7630`): weld at `dist² < 6.25e-6`, hull from the rightmost (lowest on a tie) point, gift wrap; `ComputeAABB` with `GetEffectiveRadius` (`UP!0x180ba69a0`: `r − 0.01` above 0.01) | `phys_shape.c` `poly_set`, `ph_piece_aabb` |
| `b2Body::ResetMassData` (`UP!0x180bac920`): dynamic bodies only, the fixture list newest first, sensors left out, `lc = Σ(c·m)·(1/Σm)` | `ph_body_reset_mass_data` |
| `PhysicsContacts2D` (`UP!0x180bfee50` BeginContact, `UP!0x180c02770` EndContact, `UP!0x180c06220` ProcessContacts, `UP!0x180c07260` RemoveContact, `UP!0x180c05000` PreSolve, `UP!0x180c07980` SendCallbackReports): one `Collision2D` record per collider pair in `m_Collisions` (U4) | `phys_world.c` `begin_contact`, `end_contact`, `pre_solve`, `process_contacts` |
| `Collider2D::CreateFixtures` / `Cleanup` / `RecreateCollider` (`UP!0x180c004a0`, `UP!0x180c00240`, `UP!0x180c07050`), `Rigidbody2D::SetBodyType` (`UP!0x180c158a0`), `b2Body::SetType` / `SetActive` / `SetTransform` | `fixture_create`, `fixture_destroy`, `shape_recreate`, `phys_body_set_*` |

## Unity's deltas and the rules the port adds

### U1 — The centre of mass leaves out triggers
`ResetMassData` sums fixtures with `density != 0 && !m_isSensor` (`UP!0x180bac920`).  E1 confirms it: the
Knight's `(0,-0.75)` is his body box, Hornet's `bosses.json#rb2d.centerOfMass` her body box; the area-weighted
centroid with the HeroBox fails 262/492 steps.  `ph_body_reset_mass_data`.

### U2 — Fixture A, proxy ids, and the Knight first
Fixture A is the lower proxy id's fixture unless the shape registry swaps it (`UP!0x180baa590`,
`UP!0x180baf780`).  Proxy ids are a pure function of the order of proxy creations and destructions: the pool is
a LIFO free list, so a new proxy takes the most recently freed id (native-box2d.md §4.3; `tests/test_phys.py`
N7).  What the port cannot know is the tree at scene load, which depends on the whole session.  The port
starts from an empty tree and creates the Knight's body box first, then the scene's static colliders, then the
FSM world's bodies, then the Knight's other colliders (`sim/core/sim.c build_world`, `world_bind_hero_body`).
The Knight is DontDestroyOnLoad and exists before the boss scene loads, and the traces confirm his box is
fixture A against the terrain: E2 replays with the terrain created first give 17 velocity mismatches (every
wall step loses the hero-face residual of E6) instead of 6.  The Knight's other colliders stay last because the
FSM world that owns them is built after the core's statics; the native-state dump (native-box2d.md §12 N0) would
replace the whole assumption.

### U3 — Trigger contacts are updated twice per step
`Collide` updates sensor contacts too, at the start-of-step poses and in contact-array order; the fork adds a
trigger pass after `SolveTOI`, over `m_contactList` from its head (newest first), at the end-of-step poses
(`UP!0x180bab360`, `UP!0x180baf000`).  E11 is consistent with both (a detector that leaves a roof during the
step exits in that step's trigger pass).  Two pairs one `UpdatePairs` created therefore begin in opposite orders
depending on the pass: overlapping at the start of the step, in array order (lower proxy pair first); reached
only by its end, newest contact first (`tests/test_phys.py` N1).  That is B2: NKG's first `Slash 2` parries when
the Knight stands (`rec_nkg_vis_114745344`, the clash begins first in `Collide`) and costs two masks when he
moves in (`rec_nkg_vis_32784384`, HeroBox–Slash2 begins first in the trigger pass).

### U4 — Callback order: `PhysicsContacts2D::m_Collisions`
One `Collision2D` record per collider pair (all pieces of both colliders), keyed by (ColliderA, ColliderB) with
ColliderA the lower `GetInstanceID()`.  The pair's first `BeginContact` appends the record (Enter); later begins
count up, turn Exit back into Stay and EnterAndExit back into Enter.  `EndContact` counts down; the last one turns
Enter into EnterAndExit and Stay into Exit.  Once per step, after the write-back, `ProcessContacts` walks the
array: it reports Enter (also for EnterAndExit), Stay or Exit, then Enter becomes Stay, and Exit and EnterAndExit
(which also reports an Exit) remove the record by moving the last record into its slot, which the walk visits
next.  All trigger reports go before all collision reports; each report reaches ColliderA's receivers, then
ColliderB's.  So a step's callbacks are in the order the pairs began touching, Enter/Stay/Exit interleaved,
except that an exiting pair hands its slot to the newest pair (`tests/test_phys.py` N2).  Stay is not reported
while neither side has an awake Rigidbody2D.  A collision report carries the pair's first manifold entry as
PreSolve last stored it: the normal toward ColliderA, the first point, the point count.
`begin_contact`, `end_contact`, `pre_solve`, `process_contacts`.

Checks that could have failed:
- The conformance suite on root/conf, run against this port with the probes' collider instance ids: every
  scenario labelled CF-8, CF-9, CF-10, CF-11, CF-12, CF-15 or CF-16 reproduces the game's log exactly, positions
  included; what remains in those scenarios is lifecycle (CF-1, CF-2) and sleep (CF-13).
- NKG first slash, `runs/gap/hkba/` replayed with `hit_ledger`: the game's outcome in 6/6 sampled episodes
  (parry) and 4/4 greedy episodes (two masks); greedy ep01 then follows the game's next three hits exactly.
- `polbat_GG_False_Knight` fkp_ep09 step 89: AltSlash–False Knight before False Knight–HeroBox, as in the game;
  the False Knight battery reaches step 90 (the corpus length) in all 12 episodes.

### U5 — A kinematic body's trigger meets static colliders
The fork's `b2Body::ShouldCollide` rejects only static–static pairs, and `WorldContactFilter2D::ShouldCollide`
passes a pair without a dynamic body when either side is a sensor (or a kinematic side sets
`useFullKinematicContacts`, which no HK body does) (`UP!0x180bad1d0`, `UP!0x180c16b80`).  Recorded:
`analysis/polbat_gruz/pg_ep00.a.hktrace` frames 25818-25855 (kinematic `Knight/Spells/Scr Heads 2` triggers
against static terrain).  `should_collide`, Q-pphys-11.

### U6 — A changed collider is re-created, its pairs kept
A resize, offset change, isTrigger toggle, facing flip, relative-transform change or body-type change runs
`RecreateCollider`: every record of the collider is flagged and set to Stay, its fixtures and contacts are
destroyed (the EndContacts leave the flagged records alone), and new fixtures are created, whose contacts come
from the next `UpdatePairs` (`e_newFixture`).  At the next `ProcessContacts` a flagged record with no touching
contact exits; otherwise it goes on as Stay: no Exit or Enter, and no warm start, because the contacts are new
(native-physics2d.md §2; E6).  `shape_recreate`; `tests/test_phys.py` E5e, N4.

### U7 — Collider disable and body deactivation
`Collider2D.enabled = false` runs `Cleanup(kColliderDisable)`: the fixtures and their contacts are destroyed, then
`ProcessContacts` for that collider's records reports their Exits inside the call (`UP!0x180c00240`;
`physics.json#Physics2D.callbacksOnDisable` true).  `phys_shape_set_enabled` queues them for
`phys_take_exit_events`, which `lc_physics_exit_on_disable` drains (A-12).  `Rigidbody2D.simulated = false`
(`b2Body::SetActive`) destroys the contacts but reports nothing until the next step's `ProcessContacts`
(`tests/test_phys.py` N2).  A callback that disables a collider while a step's reports are being sent gets
that collider's Exits at once; the step's remaining reports, built before the sends began, are still delivered,
so an Exit can precede an already-queued Enter or Stay of the same pair (`UP!0x180c06220` builds the report
arrays before `UP!0x180c07980` sends them, and a nested `Cleanup` runs its own walk).

### U8 — Continuous collision only for bullets
`OnContactCreate` puts a contact in the TOI array only if it is solid and a body is a bullet
(`Rigidbody2D.collisionDetectionMode = Continuous`), and `SolveTOI` also requires a bullet side, even against a
static body.  A Discrete body tunnels through a thin wall (`docs/engine-lifecycle.md` R5 `f_tunnel_discrete`;
`tests/test_phys.py` N6).  This replaces 2.3.1's `collideA = bullet || !dynamic` (Q-pphys-13).

### U9 — The Knight's contacts clip without the skin, and corners are vertex-vertex
A polygon pair where either radius exceeds the contact offset (the Knight's box, 0.01 + edgeRadius 0.0025), and a
chain child against such a polygon, go through Unity's `b2CollideRadialPolygons` (`UP!0x180bb0120`,
`UP!0x180bafea0` → `UP!0x180ba2a60`).  Its prologue is `b2CollidePolygons`', but the side planes are the
reference face's own ends (no radius), and when the clip leaves fewer than two points, or none within the radii,
it falls back to a one-point circles manifold between the nearest reference vertex and its nearer incident vertex
(no contact beyond the radii), whose normal is the corner-to-corner direction.  A chain child is collided as a
two-vertex polygon, without ghost vertices.  Face on face the impulses and positions are 2.3.1's (the clip
coordinate never enters a non-rotating solve): E2 stays bit-exact.  At a corner within 0.0225 the normal is
diagonal where 2.3.1 gives a face normal (`tests/test_phys.py` N8), which changes `FindCollisionDirection`
(`HC:4692-4715`) and ledge catches.

### Conflicts between the native reading and measurements
- **Destroy.** native-physics2d.md §2/§7.1 reads `Object.Destroy` as `Cleanup(kColliderDelete)` →
  `DestroyContacts`, which removes the records silently.  The probes measure Exit inside `Destroy` on an object
  with a Rigidbody2D (`f_exit_destroy_{fixed,update,late}`, 4/4 processes).  The port follows the measurement
  (the lifecycle's destroy deactivates, which disables the colliders).
- **Stale manifold entries.** During a recreate `EndContact` leaves the record's manifold entry pointing at the
  destroyed `b2Contact`, and `ProcessContacts` drops only null entries, so the entry stays until the pair exits.
  The port keeps it frozen at its last PreSolve data (`PH_MAN_STALE`); what the native report makes of the
  dangling pointer is not in the decompile.  It can only change a collision report's normal after a recreate
  of a multi-contact pair.

### Settings
Every tunable Unity exposes comes from `physics.json#Physics2D` through `Physics2DSettings::UpdateBox2D`
(`UP!0x180c08b80`): velocity/position iterations, `baumgarteScale`, `baumgarteTOIScale`, `maxLinearCorrection`,
`velocityThreshold`, `maxTranslationSpeed` (a per-step distance), `defaultContactOffset` (the polygon radius,
E2).  `b2_aabbExtension` 0.1, `b2_aabbMultiplier` 2, `b2_linearSlop` 0.005 and 8 TOI sub-steps are compiled in
(native-box2d.md §2).

### Not ported
Each item is a native delta the port does not model yet; the section that pins it says what it changes.
- Body sleep (Q-pphys-8, conformance CF-13).
- The recreate thresholds (`Δ² > 1e-10` for size and offset, `CompareApproximately(1e-5)` for a relative
  transform): the port re-creates on any exact change of the body-frame geometry.
- The `M`-matrix vertex formula for colliders on rotated or sheared chains (native-physics2d.md §1.1) and the
  quaternion route of a body's angle (Q-pphys-6).
- A report to the Rigidbody2D's own GameObject when it differs from the collider's (`SendCallbackReports`
  RigidbodyA/B): the port routes by the collider's object.
- The tree state at scene load (U2).

---

## E1 — Integration: what is integrated, in which order

**Hypotheses.**
H1a: semi-implicit Euler, `v += dt·(gravityScale·gravity)` then `p += dt·v` (`analysis/specs/hero-motion.md §1.2` H1).
H1b: the integrated quantity is the body origin `p` (`Rigidbody2D.position`).
H1c: the integrated quantity is the Box2D sweep centre `c = p + R·localCenter`, `p = c − R·localCenter`
after every step (b2Sweep / b2Body::SynchronizeTransform structure), with `localCenter` = the reported
`Rigidbody2D.centerOfMass` (`phys.json#rb2d.centerOfMass` (0,−0.75) for the Knight = the body BoxCollider2D
centre; `bosses.json#rb2d.centerOfMass` (0.1484013,−0.9687844) for Hornet = her BoxCollider2D centre — in
both cases the trigger colliders on the same body are excluded).

**Records.** Hero: every step whose `HC_UPDATE_PRE` velocity equals `HC_FIXED_POST` + gravity and whose
position equals the integrated one in both axes (no contact): 486 chained steps in 23 runs, r2_move /
r2_rand1 / r2_rand2, run from the first step's pose with the recorded per-step velocities and the
recorded facing (`FRAME.hero.scale_x`).  Hornet: `r2_move.a.hktrace@f24764–f24806` (GG Fall: 28 gravity-only
frames from the dump pose (31.12, 44.59476) at v = 0, `entities[Hornet Boss 1].pos_y/vel_y`).

**Measurement.**
- Operation order: `v.y += dt·(gs·g)` with `gs·g` rounded first, then `dt·(…)`, then `p += dt·v` with
  `dt·v` rounded first — the only order that is bit-exact; `(dt·gs)·g` and `p + v·dt` variants fail.
- H1b (origin): hero 492/493 single steps; the one failure is `r2_rand1.a.hktrace@f25282` (predicted `82de0142`,
  recorded `81de0142`).  Hornet's intro: 24/28, failing from `@f24800` on (`3d940342` vs `3e940342`).
- H1c (centre): Hornet 28/28.  Hero with lc=(0,−0.75) and the facing flips applied (each flip rebuilds
  the fixture, and b2Body::ResetMassData then re-anchors `c = fl(p + lc)` from the coarser-grid `p`):
  **486/486**; without the re-anchor at the flip `@f25281` the same frame `@f25282` is off by one ulp,
  which is exactly a double-rounding case (the fine-grid intermediate `c + Δ` lands on a coarse-grid
  midpoint: `S/2^-19` has fractional part 0.695 → the odd fine multiple → tie-to-even on the 2^-18 grid).
- The area-weighted centroid of the Knight's two colliders (body box + HeroBox trigger, both density 1)
  as `localCenter` fails 262/492 — triggers do not contribute.

**Rule adopted (H1a + H1c).**  `sim/phys/phys_solver.c: island_solve` integrates `c`; `p = c − R·lc`
(`ph_body_sync_transform`); `lc` = density-weighted b2 `ComputeMass` centroid of the body's enabled
non-trigger shapes (`ph_body_reset_mass_data`), re-applied — with `c0 = c = p + R·lc` — whenever a shape
of the body is added, enabled/disabled or re-created (U6).  The max-translation clamp is
`phys.json#Physics2D.maxTranslationSpeed` = 100 per step (`UP!0x180c08b80`, `UP!0x180bad210`), never reached.

---

## E2 — Skin radii, linear slop, resting geometry

**Hypothesis.**  Each polygon fixture carries a radius `r = defaultContactOffset + edgeRadius`
(`phys.json#Physics2D.defaultContactOffset` 0.01, `phys.json#heroColliders[0].edgeRadius` 0.0025), a contact
manifold exists while the core polygons are closer than `rA + rB`, and the position solver stops
correcting once `separation ≥ −linearSlop` with `linearSlop = 0.005` (Q-hero-4 asked for exactly this
decomposition).

**Records.** Hero at rest: 1227 rest steps, `rb_pos_y = 28.4081211` (`d543e341`) on
`scene.json#Hornet Saver/Colliders[118246]` (top y = 27.0); Hornet at rest `r2_move.a.hktrace@f24815`
`pos_y = 28.561871` (`b67ee441`), her box `scene.json#Boss Holder/Hornet Boss 1` (offset (0.14840126,
−0.968784332), size (1.39357567, 1.15617847), edgeRadius 0); hero at the left wall `r2_rand1.a.hktrace@f25248`
`rb_pos_x = 15.267498` (wall face x = 15.0, `scene.json#Hornet Saver/Colliders[118250]`); hero at the right
wall `r2_move.a.hktrace@f25385` `rb_pos_x = 37.732536` (face x = 38.0).

**Measurement.**  Gaps between core shapes: hero–floor 0.0174961, hero–left wall 0.0174980,
hero–right wall 0.0174640, Hornet–floor 0.0149980.  With `(0.01 + 0.0025) + 0.01 − 0.005 = 0.0175`
(hero) and `0.01 + 0.01 − 0.005 = 0.015` (Hornet) every gap is the predicted value minus a few ulps —
the sign and size of the residue is the position solver's stopping condition (E3), not a different
constant.  `max(contactOffset, edgeRadius)` (gap 0.015 for the hero) and `edgeRadius` alone are
excluded.  Box corners are `offset ± size/2` in float32 then scaled by `lossyScale`
(`scene.json#…[118246].world` = 26.4 − 13.8 = 12.5999994 confirms the arithmetic).

**Rule adopted.** `PH_POLYGON_RADIUS = 0.01f`, `PH_LINEAR_SLOP = 0.005f` (`phys_internal.h`);
`radius = PH_POLYGON_RADIUS + edge_radius` per polygon piece (`phys_shape.c: ph_shape_bake`).

---

## E3 — Position solver (Baumgarte), friction, restitution

**Hypothesis.** b2ContactSolver::SolvePositionConstraints structure: for each contact, for each
manifold point in order, `C = clamp(0.2·(separation + slop), −0.2, 0)` applied along the normal with
`K = mA + mB` (non-rotating bodies), up to `positionIterations = 3` iterations
(`phys.json#Physics2D.positionIterations`), early-out when the minimum separation seen in the iteration is
≥ −3·slop; `baumgarteScale = 0.2`, `maxLinearCorrection = 0.2` (`phys.json#Physics2D`).  Velocity solver:
8 iterations (`phys.json#Physics2D.velocityIterations`), warm started from the previous step's accumulated
impulse, friction and restitution 0.

**Records.** Post-landing settles: `r2_move.a.hktrace@f25184–25191` (`c343e341 → ca43e341 → cf43e341 →
d243e341 → d443e341 → d543e341`), the same sequence after every one of the 13 landings; the skin
landing `r2_move.a.hktrace@f25258–25268` (`263ee341 → 3240e341 → 8241e341 → 5942e341 → e242e341 → 3a43e341 →
7243e341 → 9643e341 …`); Hornet's `r2_rand1.a.hktrace@f25237–25266` settle from her Land-Y snap (distance to
the fixed point shrinks by 0.262 = 0.8⁶ per step while deep, then 0.64 = 0.8² per step); every rest step
(1227) has `Δv.y = 0` exactly with `v.y_pre = −0.948`; every ground-running step keeps `v.x` (no
friction); no landing rebounds.

**Measurement.** A single manifold point per contact would give 0.8³ / 0.8 ratios; a 2-point manifold
with the points corrected sequentially gives 0.8⁶ / 0.8² — the recorded ratios.  The early-out at
−3·slop is what switches the ratio from 0.8⁶ to 0.8² at separation −0.015.  The skin landing
`@f25259` (pre-step separation −0.007779, i.e. inside the skin but above the TOI target, so no TOI):
one iteration of two corrections predicts +0.0010004, recorded +0.0009994 → `3240e341` bit-exact in the
replay.  Full replay (E-replay below): all 1227 rest and 57 other contact steps bit-exact in `rb_pos`.
Friction: the hero's `FrictionlessSurface` material makes the mixed coefficient 0 for every hero contact;
restitution 0 is the only value consistent with 13 landings at −12…−20 with `v.y_post = +0` exactly.

**Rule adopted.** `phys_solver.c: solver_solve_position` (discrete), `PH_BAUMGARTE`,
`PH_MAX_LINEAR_CORR`; `contact_create` sets friction = restitution = 0 (Q-pphys-3 for non-hero pairs).

**Q-pphys-3, status 2026-09-02 (d3): the gap in the reasoning is CONFIRMED, but it is NOT costing any
measured divergence, and the dumper extension is NOT justified by the evidence so far.**

*Confirmed.*  The justification above covers the hero only.  In `analysis/dumps/GG_False_Knight/scene.json`,
of 1275 colliders the `FrictionlessSurface` material is on **exactly one — `Knight`**.  The surfaces
False Knight lands on carry `Terrain` (12 colliders, including `Battle Scene/False Knight New/FK Terrain
Block`), and 528 more carry `Geo Small`.  A boss-vs-terrain contact is a non-hero pair, and nothing makes
it frictionless.  So `friction = 0` is an assumption there, not a derivation.

*Not quantified, and not the cause of the obvious candidate defect.*  False Knight's median divergence is
set by his horizontal displacement on the frame he lands (game +0.07288, sim +0.10000, a permanent
0.02712 offset; Y is bit-identical).  That looks like friction — X-only, Y exact, at the moment of ground
contact — and it is not.  Over **35 landings sharing one normal impulse** (`|dv_n| = 16.504`) with vx
spanning -12.0 to +12.0:

| quantity | measured across those 35 landings |
|---|---|
| `alpha = dx/(vx*dt)` | 0.728707 … 0.733232 — **constant to 0.6%** |
| `mu_implied = dvx/abs(dv_n)` | 0.116691 … 0.193969 — scales directly with abs(vx) |

Coulomb friction requires `dvx` constant for a given normal impulse; the measurement says `dvx` is
**proportional to vx**, i.e. a multiplicative truncation.  The error is also a single-frame jump that then
freezes forever, not drift accumulating while in contact.  So friction is refuted as that mechanism.

*Consequence.*  Emitting `sharedMaterial.friction` / `.bounciness` needs an oracle dumper change, a
rebuild and a re-dump of every scene, and no measurement currently demands it.  Recorded here as a known,
deliberate approximation for non-hero pairs.  If a boss ever shows drift that GROWS while sliding along a
surface, this is the first thing to suspect and the dumper field becomes worth the detour.
Evidence: `findings/boss-falseknight.md` FINDING 2.

---

## E4 — Continuous collision (time of impact) and the sub-step

**Hypothesis.** b2World::SolveTOI structure: after the discrete solve, for every non-sensor contact
with a `Continuous` body (`phys.json#rb2d.collisionDetectionMode`, `bosses.json#rb2d`), b2TimeOfImpact on the
sweeps `c0 → c` with target separation `max(slop, rA + rB − 3·slop)` and tolerance `slop/4`
(bisection then secant root finder); the earliest contact's bodies are advanced to the TOI
(`c0 += α·(c − c0)`, `c = c0`), the contact is updated (BeginContact if it becomes touching), an island
of the two bodies + their other touching contacts is solved with: position constraints first
(`toiBaumgarte = 0.75` (`phys.json#Physics2D.baumgarteTOIScale`), up to 20 iterations, early-out at
−1.5·slop, only the two TOI bodies move), then velocity constraints (8 iterations, no warm start), then
`c += (1 − α)·dt·v`; TOI impulses are not stored.

**Records.** 13 landings (`analysis/specs/hero-motion.md §1.2` table: r2_move @f25184, 25349, 25538, 25571, 25610, 25643;
r2_rand1 @f24787, 25033, 25335; r2_rand2 @f24882, 25209, 25428, and the −18.012 skin case @f25259),
2 wall TOIs (`r2_move.a.hktrace@f25379` dash at +20 into x = 38; `r2_rand1.a.hktrace@f25237` recoil at −15 into
x = 15) with partial-step translations (0.2345 of 0.4; 0.2345 of 0.3).

**Measurement.** Predicted landing height: TOI stops the core shapes 0.0075 apart (separation −0.015);
four sequential corrections (2 iterations × 2 points, 0.75 each, then the −1.5·slop early-out) leave
−0.005039 → `28.408086`; recorded `28.4080868` (`c343e341`) on all 13, `rb_vel_y = +0` exactly.  The
wall cases give `37.7325401` / `15.2674608` by the same arithmetic on x.  Replay: 13/13 landings and
11/11 wall steps bit-exact in `rb_pos` and (except Q-pphys-17) `rb_vel`.  Enter callbacks: the hero's
`HeroCtrl-Landed` (`HC:4848`, fired from `OnCollisionEnter2D`) is recorded on exactly the TOI frames
(@f25184, 25349, 25379 …) and on the frame AFTER a skin landing (@f25259 for the step that ended inside
the skin @f25258) — i.e. BeginContact from the TOI update in-step, from `Collide()` at the next step
otherwise.

**Rule adopted.** `phys_distance.c: ph_time_of_impact` (+ `ph_distance`, GJK with the simplex cache),
`phys_solver.c: ph_solve_toi`, `island_solve_toi`; `PH_TOI_BAUMGARTE`, `PH_MAX_SUBSTEPS = 8`,
`PH_MAX_TOI_CONTACTS = 32` (structure constants, never reached).  `Continuous` ≡ b2 bullet, and only contacts
with a bullet side are TOI candidates (U8).

---

## E5 — Fixture A/B assignment and callback timing

**Hypothesis.** For a polygon–polygon contact between the Knight and the terrain the Knight's fixture is fixture A
(its face is the reference face when the separations tie, `k_tol = 0.1·slop`); shape-type pairs follow the Box2D
contact registry (polygon before circle, edge before both).  Callback events: Enter when a pair starts touching,
Stay while it keeps touching, Exit when it stops or the collider is disabled
(`phys.json#Physics2D.callbacksOnDisable = true`); one event kind per pair per step; both colliders receive it,
normal oriented from the other body to the receiver.

**Records.** E6's residual (below) exists only if the hero's own side face is the reference face;
`HC:4692-4715 FindCollisionDirection` needs `normal.y ≥ 0.5` for a landing (normal from ground to
hero); 100+ grounded facing flips (r2_rand1 @f24796 … 25216, r2_rand2 @f24885 … 25515) fire no
`HeroCtrl-Landed`; worker H's replay (`analysis/specs/port-hero.md` Q-phero-1/2) needs no Stay on the
Enter step and trigger callbacks before collision callbacks within a step (r2_jump @f24630).

**Measurement.** With the terrain as fixture A the wall normal is exact and no residual appears (E6 fails
16/16); with the hero as A, 16/16.  Flip frames: no Enter/Exit ⇒ the pair survives a fixture rebuild.
`Collide` precedes `SolveTOI` in a step, and trigger reports go out before collision reports, so a trigger
overlap is reported before a TOI landing Enter of the same step — the order H measured.

**Rule adopted.** Fixture A is the lower proxy id's (the fork's rule), and the Knight's box has the lower id
because it is created first (U2).  The receivers are ordered by instance id (U4): the Knight's colliders
(dumped ids 84158-84244 in every scene) are below the boss scene's, so he hears first.  The order between pairs
is U4.

---

## E6 — The wall-contact velocity residual: inexact face normal, warm start, facing flips

**Records.** `rb_vel_x` after every wall step: `r2_rand1.a.hktrace@f25237` (TOI, pre −15) `aa000000`
(= −2⁻⁴³), @f25239 (first discrete step, pre −15) −2⁻⁴³, @f25240/25242/25243 (pre −15) `00000000`,
@f25245 (pre −8.3, facing flipped this frame) −2⁻⁴³, @f25246 (pre 0) +2⁻⁴³, @f25248 (pre 0) 0;
`r2_move.a.hktrace@f25379` (TOI, pre +20) `2a800000` (= +2⁻⁴²), @f25380 +2⁻⁴², @f25382/25383 0, @f25385
(pre 0) −2⁻⁴², @f25386 (pre +8.3) +2⁻⁴³, @f25388/25389 0.  Ground contacts never show a residual.

**Hypothesis chain (all tested).**  (a) A brute-force over the standard solver arithmetic (effective
mass / inverse mass / damping perturbed by ±2⁻²⁴…2⁻²², 1/2/8 iterations, accumulation on/off) never
produces the residual (0 of 588 variants).  (b) The hero's side faces are 1.28125 long; b2 normalises
edge normals as `edge·(1/len)`, and `1.28125·fl(1/1.28125) = 1 − 2⁻²⁴`, while his 0.5-long top/bottom
faces and every terrain face normalise exactly — an x-only, hero-face-only effect.  With
`n = (−(1−2⁻²⁴), −0)` the 8-iteration accumulated-impulse solve reproduces 15/16 recorded values in
the two chains (fresh solves on the TOI frame and the first discrete frame because TOI impulses are not
stored; exact cancellation once the warm start carries).  (c) The 16th (@f25245) needs a fresh solve
although the previous step stored an impulse: `FRAME.hero.scale_x` flips −1 → +1 on that frame.  A
fixture rebuild re-indexes the mirrored vertices, so the manifold ids no longer match and
b2Contact::Update drops the stored impulses — while the contact itself persists (E5: no Enter/Exit on
flips).  With that rule: **16/16**, and the ground contacts stay exact because the hero's bottom normal
is exact.

**Rule adopted.** `phys_shape.c: poly_set` (normals via `v2_normalize`); a facing flip re-creates the collider
(U6), so the new contact starts without a warm start.  The ids themselves do not change on a flip: `Set`'s hull
order is the same for a box and its mirror (bottom-right, top-right, top-left, bottom-left; native-box2d.md §6.2),
and E2 is bit-exact with it.

---

## E7 — Orders

Every order is the fork's (The translation): `Collide` walks the non-TOI then the TOI contact array (creation
order, perturbed by swap-removal), the TOI minimum scan walks the TOI array, the trigger pass walks
`m_contactList` from its head (newest first), the island DFS walks each body's edge list (newest first), and
islands are seeded in `m_nonStaticBodies` order.  Multi-contact solve order matters only when one body touches
two or more solids (the hero in a corner, `r2_move.a.hktrace@f25379`: wall TOI while grounded); the two
constraints there are orthogonal and the replay is bit-exact.  The callback order is U4.

---

## E8 — Queries (raycast, boxcast, overlap)

Pinned by settings, not by trace (no query result is recorded): `phys.json#Physics2D.queriesHitTriggers =
true` (triggers are returned; the callers filter `isTrigger` themselves, `HC:4467`),
`phys.json#Physics2D.queriesStartInColliders = false` (a ray from inside a polygon/circle reports no hit —
the b2PolygonShape/b2CircleShape::RayCast outcome for an interior origin; test E4d).  Closest hit wins;
first shape id wins ties (the fork keeps one hit per collider and sorts stably, so its ties keep the dynamic
tree's traversal order: native-physics2d.md §10; exact ties only).  BoxCast: a shape overlapping the box at its start is ignored, otherwise the
same TOI as the solver on the swept box (`phys_world.c: phys_boxcast`); only the hit/no-hit and the
collider identity are load-bearing for `HC:4358-4425 CheckForTerrainThunk`.  Q-pphys-9.

---

## E-replay — the whole corpus through the port

`tests/test_phys.py` E2: the GG_Hornet_1 terrain from `sim/core/scene_GG_Hornet_1` (17 active
statics; the rotated `Needle Tink` trigger is accepted, no rotated solids exist), the Knight body
(`phys.json#rb2d`, `#heroColliders[0..1]`), every fixed step of the four traces driven with the recorded
`HC_FIXED_POST` velocity, the facing from `FRAME.hero.scale_x`, the collider from `FRAME.hero.cols[0]`
and the gravity scale in effect (the previous frame's `rb_gravity`, or — where HeroController wrote it
in this frame's FixedUpdate, dash start/end — the value the recorded velocity change implies).

| category | steps | rb_pos bit-exact | rb_vel bit-exact |
|---|---|---|---|
| free flight | 492 | 492 | 492 |
| resting | 1227 | 1227 | 1222 (5 = damage-path `ResetMotion` writes after the solve, `analysis/specs/hero-motion.md §1.3`) |
| landing (TOI + skin) | 13 | 13 | 13 |
| wall | 11 | 11 | 10 (`@f25293`, Q-pphys-17) |
| other contact | 57 | 57 | 57 |
| **total** | **1800** | **1800** | **1794** |

E1 (no terrain, chained): 486/486.  E3: hero at the recorded reset pose stays bit-stable for 60 steps;
a drop from y = 30 settles to `d543e341`; Hornet's intro drop from the dump pose: 28/28 fall frames,
landing `a37ee441` and the next 5 settle steps bit-exact, final rest 1 ulp low (Q-pphys-1).

---

## E9 — Closed loop (P3): `core;hero;phys;obs` over the nine r2 corpora, and per-shape layers

`harness/sim_driver.py --only r2_ --tolerances harness/tolerances_p3.json` with the per-shape-layer build
(`sim/build-p`, 2026-08-31).  DH = frames until the first divergence (`harness/divergence.py --subset --all`):

| corpus | DH (frames / agent steps) | first divergent record | attribution |
|---|---|---|---|
| r2_idle | 240 / 120 — full length | none | — |
| r2_move | 286 / 143 | `HC_UPDATE_PRE` real f25193 `rb_vel_x` 0 vs 8.3 | first `HERO_DAMAGE` of the corpus (`r2_move.a.hktrace@f25193`, source Hornet Boss 1): `TakeDamage -> StartRecoil -> ResetMotion` zeroes the velocity after the solve, sets `recoilFrozen`, gravityScale 0 and flips facing; the loop has no FSM so Hornet never attacks |
| r2_walk | 279 / 140 | `HC_UPDATE_PRE` real f25137 `rb_vel_x` 0 vs -8.3 | same: first `HERO_DAMAGE` `@f25137` (hero at x = 16.07, 1.07 from the left wall face: not a wall contact) |
| r2_jump | 49 / 25 | `HC_UPDATE_PRE` real f24630 `rb_vel_x` 0 vs 8.3 | same: first `HERO_DAMAGE` `@f24630` (x = 30.67, mid-arena) |
| r2_dash | 174 / 87 | `HC_UPDATE_PRE` real f28312 `rb_vel_x` 0 vs -2^-42 | `HERO_DAMAGE` events `@f28307-28312`: the real `FRAME@f28312` sets `recoilFrozen`/`invulnerable`, hp 9 -> 8, `scale_x` -1 -> +1.  The real solve is fresh (the flip rebuilds the fixture, E6) with pre-velocity 0 -> exactly 0; the un-hit sim keeps the warm start from the dash impulse and shows -2^-42, which is what the real engine shows in the un-hit version of the same situation (`r2_move.a.hktrace@f25385`, E6).  Not a solver defect |
| r2_attack | 100 / 50 | `HC_UPDATE_POST` real f24650 `rb_vel_x` -8.3 vs +3.75 | the recoil is real and comes from the nail hitting Hornet (`Slash/damages_enemy HIT` `@f24647-24650`, `recoilingRight` on both traces at `FRAME@f24650`), not from a terrain thunk: the horizontal `CheckForTerrainThunk` BoxCast from the recorded pose (origin (18.556, 29.508), 0.45 box, left, length 2.0, mask layers 8 and 25) returns no hit in `sim/phys` (nearest layer-8 collider is the wall face at x = 15.0, 3.3 away).  The real game applies the enemy-hit recoil AFTER `HeroController.Update` (`HC_UPDATE_POST@f24650` still -8.3, `HC_FIXED_POST@f24651` +3.75); the sim applies it before `HC_UPDATE_POST`: an ordering item for the hero/core side (`HC:2269-2282` `RecoilLeft/Right` write `rb2d.velocity` at call time; the caller runs in the PlayMaker Update phase after HeroController.Update).  Coordination note for worker H (`analysis/specs/port-hero.md`) |
| r2_rand1/2/3 | 178 / 178 / 180 | `FRAME` `hero.f.hero_state` 7 vs 1 at step 90 | hero-side state machine; no physics field differs before it |

Every physics field (`rb_pos_x/y`, `rb_vel_x/y`) matches bit-exactly up to those records in all nine corpora;
the three `rb_vel_x` "wall" divergences named in the P3 review are damage-path writes, not contacts (the
hero is 1 to 15 units from any wall in each).

**Per-shape layers** (contract change 2026-08-31): `phys_shape_desc.layer` / `phys_shape_set_layer` /
`phys_shape_layer`, `PHYS_LAYER_INHERIT` = the body's layer.  The layer matrix and every query use the
shape's effective layer (`phys_world.c: should_collide`, `shape_queryable`); a layer change re-filters the
shape's contacts at the next `Collide` (b2Fixture::Refilter).  Test E4f: a Hero-Box-mask ray hits the
HeroBox (layer 20) on the layer-9 Knight body, a Player-mask ray hits the body box.

---

## E10 — EdgeCollider2D chains (full-stack loop, 2026-08-31)

**Trigger.** `core;hero;phys;fsm;obs` trapped at step 10 of every corpus: `phys_collide.c: solid contact
manifold for shape kinds 2 vs 0 (edge vs polygon)`, pair = `TileMap Render Data/Scenemap/Chunk 1 0` (edge
chain at (0,32)) vs Hornet's body box at (31.12, 51.55).  Hornet is at (31.12, 60.559) on the loop's first
live frame (`r2_idle.sim.hktrace` FRAME step 1; the FSM module logs her body created at the dump pose
(31.12, 44.5948) and the real trace `r2_idle.a.hktrace@f24921` has her at 44.559 in `GG Fall`) — an FSM/core
placement item, not physics — so she falls onto the chain's top segment (y = 50) from above.

**Rule adopted (structure only).** `sim/phys/phys_collide_edge.c`: b2CollideEdgeAndCircle and b2EPCollider
(b2CollideEdgeAndPolygon) of Box2D 2.3 with the chain ghost vertices (`has_e0/has_e3`): edges collide from
both sides (the ghost vertices only smooth chain joints), front/back decision from the polygon centroid against the adjacent-edge normals, edge-axis vs polygon-axis
separation with the 0.98 / 0.001 hysteresis, clipping against the reference face's side planes, 2-point
manifolds with the same id scheme as polygon–polygon.  Fixture A = the edge (Box2D registry order, E5), skin
radius `0.01 + edgeRadius` (E2).  No r2 trace records a solid contact with an edge collider, so nothing here
is trace-pinned; what is verified (`tests/test_phys.py` E10):

| case | result |
|---|---|
| hero box dropped onto a 3-point chain (ghost vertices both sides) | rests with core gap 0.0175 above the segment — the E2 rule (`0.0125 + 0.01 − 0.005`), `v = 0`, one `COLLISION_STAY` per step with normal (0, 1) to the hero |
| box driven upward into the same chain from below | stopped with its top at the same gap (two-sided: y = 27.091875) |
| Hornet's box dropped from (31.12, 60.559) onto `Chunk 1 0` | rests with bottom at 50.015 (y = 51.562, the loop's value), `COLLISION_ENTER` then `STAY` delivered to her body first |

Events in the loop after the fix (`HKSIM_PHYS_DEBUG=1` / `HKSIM_FSM_DEBUG=1`, r2_idle): the world emits
8 events per step from step 1 (the Knight's floor `COLLISION_*` and three `Hero Detector` trigger boxes,
receiver = the Knight first), Hornet's ceiling `COLLISION_ENTER/STAY` from step 22, and the FSM module logs
every one of them (219 `[fsm-phys] event` lines / 120 steps).  Why nothing reached the FSM before: at
y ≥ 51.5 none of Hornet's trigger detectors overlaps anything, and the ceiling contact trapped before its
first event could be delivered.  The spawn-time `Evade Check ENTER` of the real trace (her Terrain
Detector box vs the Roof Collider at (31.12, 44.59)) is reproduced by the same world when she starts at the
dump pose: `TRIGGER_ENTER` at step 1, `STAY` steps 2–5, `EXIT` at step 6 as she falls clear.

---

## E11 — Trigger (sensor) overlap is evaluated with the end-of-step poses

**Trigger.** Full-stack gate (`harness/tools/gate.py --tol harness/tolerances_p4a.json`): r2_idle and r2_move
diverge at step 2 (`r2_idle.a.hktrace@f24925`, `r2_move.a.hktrace@f24768`) on Hornet's `Evade Check` FSM —
its Terrain-Detector box (layer 14, offset (1.256, 0), 3.51 × 1 on her body) leaves the `Roof Collider`
one frame earlier in the real game than in the port (worker F's Q-pfsm-21).

**Records.** Hornet's intro fall (`FRAME` entity pose after each step) and the `Evade Check` FSM's physics-phase
(`phase 1`) `ENTER` events, which its `Detect` state re-sends on every physics step that delivered a stay:

| frame (r2_idle) | box top at step start | box top at step end | real `ENTER` (phase 1) |
|---|---|---|---|
| @f24921 | 45.095 | 45.059 | yes |
| @f24922 | 45.059 | 44.987 | yes |
| @f24924 | 44.987 | 44.879 | yes |
| @f24925 | 44.879 | 44.735 | yes |
| @f24927 | 44.735 | 44.555 | **no** (FSM `EXIT` in Update) |
| @f24928 | 44.555 | 44.339 | no |

The roof polygon's lower edge (28.9468, 45.6295)→(37.0703, 44.1300) passes y = 44.672 under the box's
right corner (x = 34.132), the deepest point of the box.  At the START of @f24927's step the box still
overlaps the roof by 0.063 (0.083 with both skins), at its END it is 0.117 clear.

**Hypotheses.** H-start: sensor overlap computed from the start-of-step poses (Box2D 2.3 `Collide()` before
`Solve()`, the order the solid manifolds use — E4/E5: a skin landing fires Enter the step after).  H-end: the
sensor overlap uses the poses after the step's integration.

**Measurement.** Per live frame of the nine corpora, `ENTER` present ⇔ the `Evade Check` box overlaps any
layer-8 static (its matrix row: layers 3, 6, 7, 8, 25 — never the Knight) at the chosen pose (`phys_overlap_any`
with the port's radii): H-end 4123/4191 frames, H-start 4090/4191.  The roof-exit frames above are the
decisive ones: H-start keeps the stay through @f24927 in every corpus (r2_idle 238/239, r2_move 596/599), H-end
matches them all (239/239, 599/599).  The 68 frames H-end misses are FSM-side: Hornet standing beside a wall in
`Throw Antic` / `Sphere` / `Evade Land` with the detector geometrically inside the wall but the real FSM silent
(the collider or the FSM's listening state is off there — for worker F), and frames where the FSM snaps her pose
in the same frame (`Land Y`, A-Dash bounds, facing flips) so the FRAME pose is not the physics pose.  The skin
(0.02 total) is not resolved by these frames (margins ≥ 0.06).  Hornet's `Hero Detector` boxes cannot be used
for this: their FSMs send `ENTER` once, not per stay.

**Rule adopted (U3).** Sensor contacts are updated in `Collide` at the start-of-step poses and again by the
trigger pass after `SolveTOI` at the final poses; pairs found by the post-solve `UpdatePairs` are therefore
evaluated in the same step, so a trigger reached during the step reports Enter in that step, and one left during
the step reports Exit in it.  Solid contacts keep the start-of-step manifold (E3/E4/E5).  A step's trigger
callbacks precede its collision callbacks (worker H: `HeroCtrl-HeroDamaged` precedes the landing in the physics
phase of `r2_jump.a.hktrace@f24630`).  `tests/test_phys.py` E11 reproduces the table (ENTER, STAY ×3, EXIT at
step 5); E5d encodes the trigger-before-collision order.

---

## Findings (rules the port relies on, with their pin)

| rule | pin |
|---|---|
| semi-implicit Euler, `dt·(gs·g)` then `dt·v`, on the sweep centre `c`; `p = c − R·lc` | E1 |
| `lc` = centroid of enabled non-trigger shapes; re-anchored `c = p + lc` on any fixture change | E1 (flip @f25281 → @f25282), Q-pphys-2 |
| polygon skin `0.01 + edgeRadius`; `linearSlop 0.005` | E2 |
| radius above 0.01 (the Knight): radial clipping, vertex-vertex corners, chains as 2-gons | U9 |
| edge chains: two-sided b2EPCollider manifolds with ghost vertices, edge = fixture A | E10 (structure only) |
| trigger overlaps evaluated at the start and at the end of the step; trigger callbacks before collision callbacks | U3, E11 (4123/4191 detector frames; roof-exit frames exact) |
| contacts from sticky, displacement-stretched fat AABBs; pairs only for moved proxies; batch order by proxy id | native-box2d.md §4 (The translation), N7 |
| callbacks in `m_Collisions` order: pair-birth order with swap-with-last on exit, kinds interleaved, lower instance id first | U4 (native; conformance, NKG, False Knight) |
| Exit inside the call that disabled a collider; `simulated = false` exits at the next step | U7, `docs/engine-lifecycle.md` R5 |
| TOI only with a Continuous body | U8 |
| every body stays awake (no sleep) | Q-pphys-8 |
| 2-point box manifolds, sequential corrections, Baumgarte 0.2 / 0.75, early-outs −3·slop / −1.5·slop | E3, E4 |
| TOI target `max(slop, rA+rB−3·slop)`, tolerance `slop/4`, 20 TOI position iterations, no TOI warm start / storage | E4 |
| friction 0, restitution 0 (hero material) | E3, Q-pphys-3 |
| the Knight's box = fixture A against the terrain (created first); inexact face normals kept | U2, E5, E6 |
| warm-start impulses matched by manifold id; a re-created collider's new contacts start cold | E6, U6 |
| Enter in-step for TOI contacts, next step for skin contacts; no Enter/Exit on flips; Collide events before TOI events | E4, E5 |
| free-rotation dynamic bodies turn (inertia from ResetMassData, contacts, torque, angularDrag); the 2-point block solver runs | Q-pphys-10, Q-pphys-19 |

## Interface requests (sim/core/phys.h, orchestrator-owned)

1. **Per-shape layer.** DONE (2026-08-31): `phys_shape_desc.layer` with `PHYS_LAYER_INHERIT`, implemented
   in E9.  (`Knight/HeroBox` is layer 20 on the layer-9 Knight body, Hornet's `Hero Detector` (13) /
   `Terrain Detector` (14) / `Enemy Attack` (22) children sit on her layer-11 body.)
2. **Materials.** `float friction, bounciness;` per shape (needs a dumper extension: `sharedMaterial`
   values are not in scene.json, only the name).  Until then all contacts are frictionless / inelastic
   (Q-pphys-3).
3. **Sleep mode.** A `RigidbodySleepMode2D` per body once sleep is ported (Q-pphys-8).
5. **Collider instance id.** DONE: `phys_shape_desc.instance_id` (`GetInstanceID()` of the Collider2D) orders
   the two colliders of a pair (U4).
4. **Centre of mass override** (optional): `phys_v2 center_of_mass; bool has_center_of_mass;` so the
   core can pass the dumped `rb2d.centerOfMass` (Hornet's differs from b2 `ComputeMass` by one ulp,
   Q-pphys-2).

## Open questions

### Q-pphys-1 — Hornet's rest height is one ulp low after her intro landing
`r2_move.a.hktrace@f24807–24815`: the port reproduces the landing `a37ee441` and the settle steps `aa7e, af7e,
b27e, b47e` bit-exactly, then rests at `b57ee441` where the trace rests at `b67ee441` (the trace's last
correction is +2 ulp, the port's +1).  Neither the centre-of-mass value (tested 1 ulp lower, per the
dump) nor a mass-data re-anchor at any settle step changes the port's result; the divergence is in the
hidden sweep centre `c` after the TOI (her non-dyadic `lc` makes `p = fl(c − lc)` identical for two
adjacent `c`).  Settled by: a trace field recording `Rigidbody2D.worldCenterOfMass` (= `c`) per frame,
or a second Hornet landing sequence with a different impact speed.

### Q-pphys-2 — Unity's reported centre of mass vs Box2D's ComputeMass, and when it is re-applied
`bosses.json#rb2d.centerOfMass.y` prints `-0.9687844`, i.e. the float one ulp below the box centre
`-0.96878433` that every b2 `ComputeMass` formulation (average / first-vertex / origin reference) and
every bounds-centre formulation yields; the Knight's `(0,-0.75)` is exact under all of them.  The port
uses `ComputeMass`.  Independently, the port re-applies mass data (re-anchoring `c` from `p`) on every
shape add / enable / disable / resize / trigger toggle of the body, per b2Body::ResetMassData; the hero
evidence covers facing flips only (E1/E6).  Hornet's FSM toggles her attack colliders every attack, so
the exact trigger set matters for her sub-ulp state.  Settled by: a per-frame `worldCenterOfMass`
trace field and a dumper that prints floats round-trip.  Interface need (deferred by the orchestrator,
2026-08-31): `phys_v2 center_of_mass; bool has_center_of_mass;` in `phys_body_desc` so the core can pass the
dumped `rb2d.centerOfMass` (dump extension: print floats round-trip, "R" format); until then
`ComputeMass` is used.

### Q-pphys-3 — Materials are not in the interface (deferred by the orchestrator, 2026-08-31)
`phys.json#heroColliders[0].sharedMaterial.name = FrictionlessSurface`, the terrain material is `Terrain`
(`scene.json#Hornet Saver/Colliders`), Hornet's box has none; only names are dumped.  Every hero contact is
frictionless whatever the terrain value (sqrt mixing) and inelastic (E3), but Hornet-on-terrain
friction is unknown and set to 0.  Dump extension needed: `PhysicsMaterial2D.friction` / `.bounciness`
per `sharedMaterial` in `scene.json#colliders[]` (plus the project default material); then
`float friction, bounciness` in `phys_shape_desc` with the b2 mixing (sqrt / max).  Nothing in
GG_Hornet_1 needs it before a scene with a non-frictionless dynamic body.

### Q-pphys-4 — Edge-collider solid manifolds are structure-pinned only
The six `TileMap Render Data/Scenemap/Chunk` EdgeCollider2Ds and `Bounds Cage` lie outside the Hornet
Saver boxes (floor y=24 / walls x=11,42 / ceiling y=49 vs 27 / 15,38 / roof ≥43) and are never touched
in the corpus.  They are baked as b2EdgeShape chains with ghost vertices (radius `0.01 + edgeRadius`);
the solid manifolds (b2CollideEdgeAndPolygon / b2CollideEdgeAndCircle) were added 2026-08-31 (E10) when
the full-stack loop dropped Hornet onto the ceiling chain from above; they follow the Box2D-2.3 structure
and pass the E10 self-consistency tests but no trace pins them.  The fork routes a chain against a polygon of
radius above 0.01 (the Knight's box) through `b2CollideRadialPolygons` on a two-vertex polygon with no ghost
vertices (`UP!0x180bafea0`, native-box2d.md §7.3; U9); Hornet and the bosses (radius 0.01) take the
b2EPCollider path.  Settled by: a trace that reaches them (impossible in
GG_Hornet_1 without passing through a box), or another scene's tilemap floor.

### Q-pphys-5 — Concave polygon decomposition (answered, ported)
Unity decomposes a PolygonCollider2D with libtess2 (`UP!0x180c05490 PolygonCollider2D::PreparePolygonShapes`:
`tessTesselate(tess, WINDING_ODD, POLYGONS, 8, 2)`, then a weld, a collinear pass and `b2PolygonShape::Set` per
convex piece of up to 8 vertices; native-physics2d.md §1.3).  `sim/phys/phys_tess.c` ports libtess2 by hand from
its public source (odd-winding sweep, monotone triangulation, `tessMeshMergeConvexFaces`), cited against both
that source and the native decompile; `add_polygon` (`phys_shape.c`) replaces the former ear clipper, applying
the same 0.0025 weld / collinear cleanup and `b2PolygonShape::Set` gate `PreparePolygonShapes` does around it.
Not carried: multi-path (holed) `PolygonCollider2D`s -- the generator still compiles only path 0
(`gen_tables.py`'s "only path 0 compiled" warning), and none of the 64 compiled scenes' polygon colliders has a
second path. Verified: `tests/test_phys_tess.py` (every piece convex, <= 8 vertices, Set's hull order, an exact
sampled partition of the input) on synthetic shapes and on one instance of every distinct concave / >8-vertex
`PolygonCollider2D` shape reachable from the compiled scenes (28 shapes: the `Roof Collider`s, the Nosk head
`Terrain Box`, pooled projectile/effect hitboxes down to the hero's Blocker Shield charm, boss range/attack
triggers, and the wave-12 arenas' own thorn/spike hazards and attack hitboxes).

### Q-pphys-6 — Rotated bodies (refined)
A body's rotation is `b2Rot::Set(angle)` (`b2Math.h:312-317`) of the transform's z euler times
`Mathf.Deg2Rad` (`ph_rot_from_deg`), and every collision routine takes the full `b2Transform`, so rotated
solids collide like any other.  Unity's native route differs by an ulp or so: it is the Transform quaternion, `a = 2·atan2f(qz, qw)` with `w >= 0`, then `sinf` / `cosf` of the UCRT
(`UP!0x180c10e30`, `UP!0x180bace60`; native-physics2d.md §3.1): for eulerZ = 180 that is `±π_f`, not
`180·0.017453292`.  Settled by: porting the quaternion route with the UCRT's `sinf`/`cosf`, or a trace with a
solid contact against a rotated body.

### Q-pphys-7 — Circle skin and scaled circles
No circle takes part in a recorded contact.  The port gives circles their `CircleCollider2D.radius`
(no contact offset added) scaled by `max(|sx|,|sy|)`, which `UP!0x180c05b10 CircleCollider2D::PrepareShapes`
confirms (clamped to [1e-4, 1e6]).  Settled by: a trace with
a circle-vs-terrain contact (Geo, Sphere Ball) and a scaled circle overlap.

### Q-pphys-8 — Sleep is not ported
The fork sleeps bodies (`UP!0x180bad210` b2Island::Solve: an island whose bodies all stay under |v| 0.01 and
2 deg/s for `timeToSleep` 0.5 s, i.e. 25 steps, with the position solve converged, falls asleep), and a
sleeping body is not integrated, `Collide` and the trigger pass skip its pairs with static or sleeping
partners, and `ProcessContacts` reports no Stay while both sides are asleep or absent (native-box2d.md §9.3,
native-physics2d.md §9).  The probes measure it (`f_sleep`, conformance CF-13), and the lifecycle corpus has 262
Stays that pause and resume with no Exit.  The wake rules are native: `SetLinearVelocity` with v != 0, every
Transform write (`SyncTransforms` → `SetTransform` + `RecalculateContacts`), `AddForce` even with a zero
force, a new solid contact, a touching change of a solid contact, a collider re-creation, `SetType`, `WakeUp`.
A port of Box2D's sleep without those wake rules made Hornet 1 diverge earlier in 10 of the 20 lifecycle
episodes (FSM transform writes moved a sleeping Hornet).  Needed besides: `Rigidbody2D.sleepMode` for every
body (dumped only for the Knight, `phys.json#rb2d`, and the HealthManager bodies, `bosses.json`).

### Q-pphys-9 — Query semantics
Raycasts ignore the polygon skin (b2 core polygon), return the first shape id on equal fractions, and
report no hit from inside a shape; BoxCast ignores shapes overlapping the box at its start and returns
the swept-box TOI.  `queriesHitTriggers` / `queriesStartInColliders` come from `phys.json#Physics2D`; the
rest is structure (Q-hornet-6, Q-hero-10).  Settled by: an oracle raycast probe (origin inside /
grazing / tie cases) recorded in a dump.

### Q-pphys-10 — Two-point block solver (settled)
Between fixed-rotation bodies the 2-point velocity constraint is degenerate (`k11·k11 < 1000·det` fails) and
Box2D falls back to one point.  A free-rotation body makes it regular, and the block solver
(`b2ContactSolver.cpp:388-586` = `UP!0x180bb23c0`) is ported (`phys_solver.c solver_solve_velocity`).

### Q-pphys-11 — Kinematic/static trigger pairs (settled)
The fork rejects only static–static pairs and passes a pair without a dynamic body when either side is a
sensor (`UP!0x180bad1d0`, `UP!0x180c16b80`): U5.  Solid kinematic pairs need `useFullKinematicContacts`, which
no HK Rigidbody2D sets (not dumped: the default is false).

### Q-pphys-12 — Fixture order and multi-contact solve order (settled)
Fixture A is the lower proxy id's, the registry deciding mixed kinds (U2).  Islands are seeded in
`m_nonStaticBodies` order and walk newest-first edges, so a multi-contact island solves its contacts in that DFS
order (`UP!0x180bad930`).  Only corner contacts (orthogonal, verified) occur in the r2 corpus.

### Q-pphys-13 — Discrete dynamic bodies (settled: no TOI)
2.3.1 gives a non-bullet dynamic body TOI against static and kinematic bodies (`b2World.cpp:652-659`), but the
fork does not: a contact is a TOI candidate only if it was created solid with a bullet body
(`UP!0x180bac390`), and the scan requires a bullet side (`UP!0x180bae240`, asm 0x180bae4da-0x180bae4e8).  So a
Discrete body tunnels (U8), which the probes measure (`f_tunnel_discrete`, `f_pair_kin_solid_vs_dyn`,
`f_trigger_and_collision`).  It matters for Dynamic + Discrete bodies such as `GG_False_Knight` `Staff` and
`Death Head`.  The TOI-island gathering rule (`b2World.cpp:791-796`) is unchanged.

### Q-pphys-14 — Collider resize / isTrigger toggle semantics (settled)
Both re-create the collider (`UP!0x180c16730 BoxCollider2D::SetSize`, `UP!0x180c08050 Collider2D::SetIsTrigger`):
U6.  The pair keeps its record and reports Stay while a new contact touches; an isTrigger toggle that makes the
pair unreportable (two kinematic solids) exits it, and toggling back begins a new one (`f_trigger_toggle_pair_kind`,
`tests/test_phys.py` N4).

### Q-pphys-15 — Pair discovery after a teleport (settled)
`b2Body::SetTransform` synchronizes the proxies at once and sets no `e_newFixture` (`UP!0x180bace60`), so a
teleported body's new pairs come from the `UpdatePairs` at the end of the next `Solve`, unless a fixture was
created since the last step, in which case the whole move buffer is paired before `Collide`.  A pair whose fat
AABBs stopped overlapping is destroyed by the next `Collide`.  The probes' teleports (`f_teleport_*`) match.

### Q-pphys-16 — Unity's callback delivery order (settled: U4)
The order is the walk of `m_Collisions` (U4).  What the port cannot reproduce exactly is inherited state: the
records of pairs that began before the scene started (the priming step rebuilds the Knight–floor pair only) and
the proxy ids of the tree at scene load (U2).

### Q-pphys-17 — `r2_rand1.a.hktrace@f25293`: warm start not carried
At @f25291 the hero (pre `v.x = −8.3`, wall contact fresh since @f25290) leaves the recorded residual
−2⁻⁴³ and stores an impulse; at @f25293 (pre −8.3 again, no flip, no collider change, contact
persisting) the trace shows the fresh residual −2⁻⁴³ again while the port cancels exactly (`+0`).
Every other warm-started wall step (7) cancels exactly in both.  Positions are unaffected (E-replay
1800/1800).  Settled by: identifying what rebuilt the Knight's fixture or invalidated its manifold ids
between those frames (HeroBox toggle at the end of i-frames? `gameObject.layer` write?).

### Q-pphys-18 — Per-shape layers (CLOSED 2026-08-31)
The contract gained `phys_shape_desc.layer` (`PHYS_LAYER_INHERIT`), `phys_shape_set_layer`,
`phys_shape_layer`; filtering and queries use the shape's effective layer (E9, `tests/test_phys.py` E4f).

### Q-pphys-19 — Angular dynamics (settled)
Ported from the native decompile: `b2Body::ResetMassData` (`UP!0x180bac920`: the fixtures' `ComputeMass`
inertia about the origin, moved to the centre of mass, clamped to FLT_EPSILON, scaled by `Rigidbody2D.mass /
sum(shape mass)`; a body whose solid shapes give none gets 1; FreezeRotation gives 0; `useAutoMass` is false on
every serialized Rigidbody2D in `analysis/assets`, explicit inertia / centre of mass are never set),
`b2PolygonShape::ComputeMass` (`UP!0x180ba6020`, Unity's grouping), `b2CircleShape::ComputeMass`
(`UP!0x180ba5f70`), the island's angular integration with `angularDrag` and the `b2_maxRotation` cap
(`UP!0x180bad210`, physics.json `maxRotationSpeed` 360), the contact solver's angular terms and block solver
(Q-pphys-10), the sweep's angle through TOI (`b2Sweep`, `b2TimeOfImpact` normalises it, `UP!0x180ba46c0`),
`Rigidbody2D.AddTorque` (`UP!0x180c0ec20`), `angularVelocity` get/set in degrees/s (`UP!0x180c11a50`,
`UP!0x180c157d0`) and the body -> Transform rotation write-back of `PhysicsManager2D::Simulate`
(`UP!0x180beb390`, root to leaf, before the step's callbacks; `sim/fsm/runtime/gameobject.c
body_rotation_writeback`).  `Rigidbody2D.drag` / `angularDrag` come from the serialized Rigidbody2D
(`m_LinearDrag` / `m_AngularDrag`, `sim/fsm/gen/gen_tables.py add_rigidbody`).  Checked by
`tests/test_phys_rot.py` (R1-R6).  Open: the written-back z euler is `fmodf(a·57.29578, 360)`, not Unity's
quaternion `eulerAngles` (B21); a dumped body's angular velocity at SceneReady is not in the v1 dumps (restored
bodies start at 0); `SpinSelfSimple.DoSpin` (Hornet 2's barb) is not compiled.
