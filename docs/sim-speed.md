# Sim speed

The sim is made faster in place, by computing the same things with less work. A speed change must not change
anything a caller can observe: the step results, the observation bytes, the RNG stream, the `.hktrace` bytes,
the whole-world state the lockstep harness reads, and what a checkpoint restores. The reference it is checked
against is the same sim at an earlier git revision.

## The reference and the checks

- `python tools/build_ref.py [rev]` builds the sim at a git revision (default `HEAD`) into
  `sim/build-ref/<rev>/hksim.dll`, from `git archive`, without a checkout.
- `python tools/sim_equal.py --ref <ref dll>` steps the reference and the working-tree build side by side on
  every scene, the way `train/sim_worker.py` drives the sim: batch observation, `EpisodeStart`'s health draws,
  reset on done. It uses random actions, invulnerable episodes (long fights, late phases), random mid-episode
  resets, and random checkpoint saves and restores on several instances per build. After every step it asserts
  byte equality of:
  - the step result and every `hksim_obs_batch` array;
  - the RNG state and the GameObject count;
  - the vocab.

  Every `--state-every` steps it also compares the whole world state as `hkls_export` / `hkls_struct` read it
  (`sim/core/lockstep_api.c`): every GameObject's pose and flags, FSM states, variables and actions, bodies,
  colliders, animators, the contact lists. `--modes trace` adds the wire observation and the `.hktrace` stream.
  Any difference is a bug in the candidate.
- `python tools/sim_bench.py --ref <ref dll>` times both builds single-threaded per scene through
  `tools/simbench/simbench.c`, the trainer's loop timed in C. It interleaves the two builds round by round and
  reports the median microseconds per step, the median speedup and its min..max. `--invuln` gives long fights;
  `--profile` samples one build's call stacks instead.
- `tools/check.py` and the fingerprint still gate every commit.

## How the work is avoided

Each mechanism below is exact by construction: it skips work whose result is already known, and it keeps every
side effect and every order the original work had.

- **Transform dirt is an epoch.** `world_invalidate_body_transforms` dirties the transform and the shapes of
  every object under a tracked body (`fsm_world.xf_gos`). It does so by advancing `fsm_world.xf_epoch`:
  - An object under a tracked body (`go_inst.xf_covered`) is dirty while its `xf_clean` / `shape_clean` epoch
    is older (`go_transform_dirty`, `go_shapes_dirty`).
  - Explicit marks (`invalidate_transform_dfs`) set the flags as before.
  - Coverage is updated for the subtree that moved (`world_xf_cover_subtree`, from `go_set_parent`,
    `world_xf_track` and `world_instantiate`), with the same transitions the walk had: an object that leaves
    the covered set keeps its marks, and one that joins it is clean until the next epoch.
  - The shape flush merges the epoch-dirty covered objects into its set (`world_shapes_dirty_set`), so it still
    visits them in ascending id order.
- **A world pose is recomputed only when its inputs changed.** `ensure_transform_clean` keeps each object's
  fold inputs (`go_inst.pose_in`: translation, scale and eulerZ as `local_t` / `local_s` read them) and numbers
  each computation (`pose_gen`). The cached pose is reused when every level of the chain reads bit-identical
  inputs and each parent is the computation its child last folded over (`pose_memo_valid`). A pose is recorded
  as reusable only if its ancestors' recorded inputs are the ones the fold read. The side effects of a resolve
  (body follow, rotation and static-body pushes) run every time, as before.
- **String lookups are hashed** (`core/str_hash.h`):
  - The scene's strings have an open-addressing index in the shared scene cache (`scene_cache.str_ix`).
  - Runtime-interned strings have one in the instance (`fsm_world.dyn_ix`).
  - `w_find_string` returns the same lowest id the linear scan did.
  - The observation vocab (`hksim_vocab`) indexes its strings the same way, and ids keep their arrival order.
- **A polygon's decomposition is replayed.** `add_polygon` is a pure function of the scaled points and the
  radius. A re-baked polygon keeps its last two results (`shape_t.bake_memo`), since colliders flip between two
  scales with the Knight's facing, and an identical input copies the pieces back.

The caches live in the instance's arena, so a checkpoint restores them together with the state they describe.
