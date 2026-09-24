#pragma once
/* hksim public C ABI (ctypes harness and trainer).
 *
 * Every function is synchronous and single-threaded per instance.  Floats are IEEE binary32 to
 * match Unity; the build flags (docs/float-parity.md) are part of the contract.
 *
 * Anything the sim does not implement is an abort, never a guess: the call returns HKSIM_ERR_* and
 * hksim_last_error() names the exact action/branch/feature.
 *
 * STABLE calls are the trainer contract (docs/sim-api.md; tests/test_contract.py); changing
 * one bumps HKSIM_ABI_VERSION.  INTERNAL calls exist for the verification harness only.
 *
 * hksim_drain() emits the oracle's .hktrace record stream (docs/trace-format.md, schema v1), byte
 * for byte.  Header field lists name the SUBSET of fields the sim models; the harness compares over
 * the intersection.
 */
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#define HKSIM_API __declspec(dllexport)
#else
#define HKSIM_API __attribute__((visibility("default")))
#endif

#define HKSIM_ABI_VERSION 1u

enum {
    HKSIM_OK = 0,
    HKSIM_ERR_UNIMPLEMENTED = 1,   /* trap: unported action/branch/feature */
    HKSIM_ERR_UNKNOWN_LEVEL = 2,   /* no compiled scene data for this level */
    HKSIM_ERR_BAD_ARG = 3,
    HKSIM_ERR_INTERNAL = 4,        /* invariant violated inside the sim (bug) */
};

typedef struct hksim hksim;

/* Options mirror the corpus header.  frames_per_wait frames are simulated per hksim_step; each frame
 * runs exactly one FixedUpdate (capture_dt == fixed_dt == 0.02, the only supported regime). */
typedef struct {
    const char *level;        /* HK scene name, e.g. "GG_Hornet_1" */
    uint32_t    frames_per_wait;
    int32_t     seed;         /* UnityEngine.Random.InitState(seed) at scene load */
    uint32_t    trace;        /* 1 = buffer .hktrace records for hksim_drain */
    /* Initial clocks, copied from the trace's SCENE_READY / first FRAME.  Frame phase matters
     * (HeroController.Update10 runs on frameCount % 10 == 0, hero-motion.md; Q-frame-*).  0 = unknown. */
    uint32_t    scene_ready_frame;
    uint32_t    fixed_count0;
    float       time0;                     /* Time.time at SCENE_READY */
    float       time_since_level_load0;
} hksim_config;

typedef struct {
    uint32_t done;            /* episode ended this step (knight or boss death) */
    float    damage_landed;   /* TrainingEnv reward signals, same units as the wire */
    float    hits_taken;
    float    hp_healed;
    uint32_t frame;           /* frame counter after the step */
} hksim_step_result;

HKSIM_API uint32_t hksim_abi_version(void);                       /* STABLE */
HKSIM_API float    hksim_fp_probe(float a, float b, float c);     /* INTERNAL: a*b+c, must NOT fuse */

HKSIM_API hksim   *hksim_create(const hksim_config *cfg);         /* STABLE.  NULL + hksim_last_error(NULL) on failure */
HKSIM_API void     hksim_destroy(hksim *s);                       /* STABLE */
HKSIM_API int      hksim_reset(hksim *s, int32_t seed);           /* STABLE.  reload scene, reseed; emits RESET/SCENE_READY events + reset OBS */
HKSIM_API int      hksim_step(hksim *s, const int32_t action[4], hksim_step_result *out);   /* STABLE */

/* INTERNAL.  .hktrace bytes produced since the last drain (header JSON via hksim_trace_header).
 * Returns bytes copied; if cap is too small nothing is consumed and the needed
 * size is returned. */
HKSIM_API size_t   hksim_drain(hksim *s, uint8_t *buf, size_t cap);
HKSIM_API const char *hksim_trace_header(hksim *s);               /* JSON, docs/trace-format.md header */

/* STABLE.  Wire observation exactly as Net/BinaryProtocol.cs would pack it.  cap == 0 sizes. */
HKSIM_API size_t   hksim_obs(hksim *s, uint8_t *buf, size_t cap);

/* INTERNAL.  The FSM world under the core, for the hkfsm_* test surface (sim/fsm/runtime/test_api.c). */
HKSIM_API void    *hksim_fsm_world(hksim *s);

/* ------------------------------------------------------------------ STABLE: the fast path
 * Fixed-shape numeric arrays the CALLER owns, filled in place for a vector of instances in one call,
 * strings resolved to vocab ids: no allocation, serialisation or UTF-8.  hksim_obs stays the parity
 * path; tests/test_contract.py asserts the two agree field for field.  Terrain row count is constant
 * per scene, so buffers are allocated once. */

enum { HKSIM_OBS_WIRE = 1, HKSIM_OBS_BATCH = 2 };   /* bit mask; default HKSIM_OBS_WIRE */

/* Which observation representations each step builds.  HKSIM_OBS_BATCH alone skips the wire pack;
 * both bits build both (the cross-check test).  Takes effect from the next reset/step. */
HKSIM_API void hksim_set_obs_mode(hksim *s, int32_t mode);

/* String -> int32 vocab: id 0 = "unknown" (also for NULL/""), id 1 = "terrain" (reserved, never
 * emitted on a row), then arrival order; at max_size the table stops growing and new strings encode
 * to 0.  Caller-owned: interning a saved i2s list in order reproduces the same ids.  The trainer
 * mirrors the reserved ids in train/sim_worker.py's VOCAB_RESERVED / VocabSpace. */
typedef struct hksim_vocab hksim_vocab;
HKSIM_API hksim_vocab *hksim_vocab_create(int32_t max_size);      /* max_size<=0 defaults to 512; the trainer always passes a positive kind_vocab_size (train/config.py) */
HKSIM_API void         hksim_vocab_destroy(hksim_vocab *v);
HKSIM_API int32_t      hksim_vocab_size(const hksim_vocab *v);
HKSIM_API const char  *hksim_vocab_str(const hksim_vocab *v, int32_t id);   /* NULL if out of range */
HKSIM_API int32_t      hksim_vocab_intern(hksim_vocab *v, const char *str); /* id, assigning one if new */

/* Caller-owned destination arrays.  Any out pointer may be NULL to skip that block and pay nothing for
 * it.  Rows past the caps are counted in n_combat / n_terrain but not written, so a caller can size
 * its buffers from a first call.  Layout is row-major and contiguous: numpy's (n_sims, cap, feat). */
typedef struct {
    int32_t  n_sims;          /* in:  entries in sims[] */
    int32_t  cap_combat;      /* in:  per-sim combat row capacity */
    int32_t  cap_terrain;     /* in:  per-sim terrain row capacity */
    float   *combat;          /* out: [n_sims][cap_combat][14]  obs-wire.md 3.3 column order */
    int32_t *combat_kind;     /* out: [n_sims][cap_combat]      vocab id of obs-wire.md 3.4 */
    int32_t *combat_parent;   /* out: [n_sims][cap_combat]      vocab id of obs-wire.md 3.5 (the clip key) */
    int32_t *n_combat;        /* out: [n_sims] rows this instance has (may exceed cap_combat) */
    float   *terrain;         /* out: [n_sims][cap_terrain][8]  obs-wire.md 4.2 column order */
    int32_t *n_terrain;       /* out: [n_sims] */
    float   *global_state;    /* out: [n_sims][33]              obs-wire.md 2 */
    float   *step;            /* out: [n_sims][3] damage_landed, hits_taken, hp_healed */
    uint8_t *done;            /* out: [n_sims] */
} hksim_batch;

/* Fill `out` from `n` instances in one call.  Every instance must have been built with
 * HKSIM_OBS_BATCH set.  Returns HKSIM_OK, or HKSIM_ERR_BAD_ARG naming the problem in
 * hksim_last_error(NULL).  Single-threaded, like every other call. */
HKSIM_API int hksim_obs_batch(hksim *const *sims, int32_t n, hksim_vocab *vocab, hksim_batch *out);

/* INTERNAL.  Snapshot injection for verification: set one recorded value by trace key ("hero.f.<name>", "hero.pd.<name>",
 * "hero.cstate.<name>", "hero.rb_pos_x|rb_pos_y|rb_vel_x|rb_vel_y|rb_gravity|scale_x", "rng.s0..s3",
 * "input").  Returns 0, or HKSIM_ERR_BAD_ARG for an unknown key.  hkpy/sim_driver.py uses it to start
 * a replay from the trace's SCENE_READY state (an initial condition, never a fit).
 * Episode setup: "hp.resync" (any value) re-takes the hit/heal baseline from the knight's current health, for a
 * caller that set hero.pd.health at an episode start.  No key changes behaviour: the sim has one configuration.
 * hksim_get_value reads the same keys, plus "fsm.n_gos": the FSM world's GameObject count, which grows when a
 * pool Instantiates a clone at runtime. */
HKSIM_API int      hksim_set_value(hksim *s, const char *key, double value);
HKSIM_API int      hksim_get_value(hksim *s, const char *key, double *out);

/* INTERNAL.  The instance's RNG, for the gate's oracle replay (rng.h hk_rng_set_mode / hk_rng_oracle_*). */
typedef struct hk_rng hk_rng;
HKSIM_API hk_rng     *hksim_rng(hksim *s);
/* INTERNAL.  The FSM module's context, for the method oracle's test entry points (sim/fsm/runtime/method_oracle.c). */
HKSIM_API void       *hksim_fsm_ctx(hksim *s);
/* INTERNAL.  Bind the instance's arena on this thread (sim/core/alloc.h) for test entry points that run module code
 * outside an ABI call; it stays bound until the next bind. */
HKSIM_API void        hksim_bind_arena(hksim *s);

/* Checkpoints, for search: save the instance's whole mutable state and return to it.  A checkpoint is a
 * copy of the instance's arena (sim/core/alloc.h), so both directions are one memcpy, and it restores only
 * into the instance that produced it.  Reuse one object across saves: its buffer grows once and is kept.
 * Everything an instance does between save and restore is undone, including runtime pool growth and
 * hksim_set_value writes; the caller-owned hksim_vocab is not instance state and keeps its ids. */
typedef struct hksim_checkpoint hksim_checkpoint;
HKSIM_API hksim_checkpoint *hksim_checkpoint_new(hksim *s);                  /* capture now; NULL on failure */
HKSIM_API int      hksim_checkpoint_save(hksim *s, hksim_checkpoint *c);     /* re-capture into c */
HKSIM_API int      hksim_checkpoint_restore(hksim *s, const hksim_checkpoint *c);
HKSIM_API void     hksim_checkpoint_free(hksim_checkpoint *c);
HKSIM_API size_t   hksim_checkpoint_bytes(const hksim_checkpoint *c);        /* bytes a save/restore copies */
/* INTERNAL, for tests/test_checkpoint.py: the raw bytes (trap/error/profile scaffolding zeroed), so two
 * saves of one position compare equal.  Returns the size; copies only when cap is enough. */
HKSIM_API size_t   hksim_checkpoint_read(const hksim_checkpoint *c, uint8_t *buf, size_t cap);

HKSIM_API const char *hksim_last_error(hksim *s);                 /* STABLE.  s may be NULL for create failures */

/* STABLE.  Scenes compiled into this build (generated by sim/gen_registries.py).  i in [0, count). */
HKSIM_API int32_t     hksim_scene_count(void);
HKSIM_API const char *hksim_scene_name(int32_t i);                /* NULL when i is out of range */
