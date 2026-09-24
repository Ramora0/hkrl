#pragma once
/* UnityEngine.Random port.  Generator + output mappings pinned bit-exact by experiment:
 *   cite: analysis/open-questions.md#Q15
 *   cite: analysis/dumps/GG_Hornet_1/rng_probe.json  (before/after state + result per draw)
 *   cite: analysis/traces/p0/r2_move.a.hktrace FRAME.rng chain (599/599 pairs reachable by k steps)
 * State words (x,y,z,w) == the recorded Random.state (s0,s1,s2,s3).  One draw == one step; the drawn
 * word is the NEW w.  Everything is uint32 / float32; no double intermediates. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "hksim.h"

/* GLOBAL: training, the calloc default.  ORACLE: the gate's replay by (site, occurrence). */
typedef enum { HK_RNG_GLOBAL = 0, HK_RNG_ORACLE = 2 } hk_rng_mode;

#define HK_RNG_OCC_CAP 512

typedef struct {
    uint64_t site;
    uint32_t count;
    uint8_t  occupied;
} hk_rng_occ_entry;

typedef struct {
    uint64_t site;
    int32_t  occurrence;
    uint32_t word;
} hk_rng_oracle_entry;

typedef struct hk_rng {
    uint32_t x, y, z, w;
    hk_rng_mode mode;
    int32_t  master_seed;

    /* Per-site occurrence counters */
    hk_rng_occ_entry occ[HK_RNG_OCC_CAP];
    uint32_t n_occ;

    /* Oracle table (growable) */
    hk_rng_oracle_entry *oracle_entries;
    int32_t *oracle_buckets;
    int32_t *oracle_next;
    int32_t  oracle_count;
    int32_t  oracle_cap;
    int32_t  oracle_buckets_cap;

    /* Oracle stats */
    int32_t  oracle_hits;
    int32_t  oracle_misses;
} hk_rng;

HKSIM_API void     hk_rng_init(hk_rng *r, int32_t seed);                 /* Random.InitState(seed) */
HKSIM_API void     hk_rng_free(hk_rng *r);
HKSIM_API uint32_t hk_rng_next(hk_rng *r);                               /* one xorshift128 step -> new w */
HKSIM_API float    hk_rng_value(hk_rng *r);                              /* Random.value */
HKSIM_API float    hk_rng_range_f(hk_rng *r, float lo, float hi);        /* Random.Range(float,float) */
HKSIM_API int32_t  hk_rng_range_i(hk_rng *r, int32_t lo, int32_t hi);    /* Random.Range(int,int) */
HKSIM_API uint64_t hk_rng_site(const char *owner_path, const char *fsm_name, const char *state_name, int32_t action_index);
HKSIM_API float    hk_rng_value_site(hk_rng *r, uint64_t site);
HKSIM_API float    hk_rng_range_f_site(hk_rng *r, uint64_t site, float lo, float hi);
HKSIM_API int32_t  hk_rng_range_i_site(hk_rng *r, uint64_t site, int32_t lo, int32_t hi);

HKSIM_API size_t   hk_rng_sizeof(void);   /* ABI size, for out-of-process mirrors (tests) */
HKSIM_API void     hk_rng_set_mode(hk_rng *r, hk_rng_mode m);
HKSIM_API void     hk_rng_oracle_clear(hk_rng *r);
HKSIM_API int32_t  hk_rng_oracle_add(hk_rng *r, uint64_t site, int32_t occurrence, uint32_t word);
HKSIM_API void     hk_rng_oracle_stats(hk_rng *r, int32_t *hits, int32_t *misses, int32_t *entries);
