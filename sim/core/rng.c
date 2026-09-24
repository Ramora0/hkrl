#include "rng.h"
#include "trap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/alloc.h"

static int misslog_level(void);

/* cite: analysis/open-questions.md#Q15 — InitState expansion, 4/4 seeds bit-exact
 * (rng_probe.json ops InitState(12345|0|-1|2026)). */
void hk_rng_init(hk_rng *r, int32_t seed)
{
    uint32_t x = (uint32_t)seed;
    uint32_t y = x * 1812433253u + 1u;
    uint32_t z = y * 1812433253u + 1u;
    uint32_t w = z * 1812433253u + 1u;
    r->x = x; r->y = y; r->z = z; r->w = w;
    r->master_seed = seed;
    memset(r->occ, 0, sizeof(r->occ));
    r->n_occ = 0;
    r->oracle_hits = 0;
    r->oracle_misses = 0;
}

void hk_rng_free(hk_rng *r)
{
    if (!r) return;
    if (r->mode == HK_RNG_ORACLE && misslog_level())
        fprintf(stderr, "[rng] oracle replay: %d hits, %d misses (independent per-site draws)\n", (int)r->oracle_hits, (int)r->oracle_misses);
    free(r->oracle_entries);
    free(r->oracle_buckets);
    free(r->oracle_next);
    r->oracle_entries = NULL;
    r->oracle_buckets = NULL;
    r->oracle_next = NULL;
    r->oracle_count = 0;
    r->oracle_cap = 0;
    r->oracle_buckets_cap = 0;
}

/* cite: analysis/open-questions.md#Q15 — Marsaglia xorshift128; verified on 599 consecutive
 * FRAME.rng pairs of r2_move.a.hktrace. */
uint32_t hk_rng_next(hk_rng *r)
{
    uint32_t t = r->x ^ (r->x << 11);
    r->x = r->y; r->y = r->z; r->z = r->w;
    r->w = r->w ^ (r->w >> 19) ^ t ^ (t >> 8);
    return r->w;
}

/* cite: Q15 closure — value = (float)(w & 0x7FFFFF) / 8388607.0f, 5/5 bit-exact (÷8388608 is NOT). */
float hk_rng_value(hk_rng *r)
{
    uint32_t w = hk_rng_next(r);
    return (float)(w & 0x7FFFFFu) / 8388607.0f;
}

/* cite: Q15e — Range(float lo, float hi) always draws (lo==hi and lo>hi included; 6/6) and
 * returns u*lo + (1-u)*hi in float32 ops: 205/205 odd-range draws bit-exact
 * (hi - u*(hi-lo) fails 58, lo + (1-u)*(hi-lo) fails 56). */
float hk_rng_range_f(hk_rng *r, float lo, float hi)
{
    float u = hk_rng_value(r);
    float a = u * lo;
    float b = (1.0f - u) * hi;
    return a + b;
}

/* cite: Q15 closure — Range(int lo, int hi): lo==hi returns lo WITHOUT drawing; lo<hi ->
 * lo + w % (hi-lo); lo>hi -> lo - w % (lo-hi); w unsigned.  7/7 + 67/67 bit-exact. */
int32_t hk_rng_range_i(hk_rng *r, int32_t lo, int32_t hi)
{
    if (lo == hi) return lo;
    uint32_t w = hk_rng_next(r);
    if (lo < hi) return (int32_t)((uint32_t)lo + w % ((uint32_t)hi - (uint32_t)lo));
    return (int32_t)((uint32_t)lo - w % ((uint32_t)lo - (uint32_t)hi));
}

static inline uint64_t fnv1a_step(uint64_t h, uint8_t b)
{
    return (h ^ (uint64_t)b) * 0x100000001b3ULL;
}

static inline uint64_t fnv1a_str(uint64_t h, const char *s)
{
    if (s) {
        while (*s) {
            h = fnv1a_step(h, (uint8_t)*s++);
        }
    }
    return h;
}

/* HKSIM_RNG_MISSLOG prints each distinct site name once ([rngsite]), each miss ([rngmiss]) and, at level 2,
 * each hit ([rnghit]).  The name set is process-wide and only a diagnostic, so a small fixed table suffices. */
#define HK_RNG_NAMED_CAP 8192
static uint64_t g_named[HK_RNG_NAMED_CAP];
static int g_named_n;
static int misslog_level(void)
{
    static int level = -1;   /* read once: a diagnostic switch set before the DLL is loaded */
    if (level < 0) {
        const char *e = getenv("HKSIM_RNG_MISSLOG");
        level = (!e || !*e || (e[0] == '0' && !e[1])) ? 0 : (atoi(e) >= 2 ? 2 : 1);
    }
    return level;
}
static int site_named(uint64_t h)
{
    for (int i = 0; i < g_named_n; i++) if (g_named[i] == h) return 1;
    if (g_named_n < HK_RNG_NAMED_CAP) g_named[g_named_n++] = h;
    return 0;
}

/* cite: port-rng.md S2 — FNV-1a 64-bit over owner|fsm|state|index with 0x1F separator.
 * Matches site_hash() in gate/rng_attrib.py.  For a draw made by C# code rather than a PlayMaker action the fields are
 * owner | Type.Method | "" | k (k = index of the Random call among that method's calls, IL order). */
uint64_t hk_rng_site(const char *owner_path, const char *fsm_name, const char *state_name, int32_t action_index)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    h = fnv1a_str(h, owner_path);
    h = fnv1a_step(h, 0x1f);
    h = fnv1a_str(h, fsm_name);
    h = fnv1a_step(h, 0x1f);
    h = fnv1a_str(h, state_name);
    h = fnv1a_step(h, 0x1f);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", (int)action_index);
    h = fnv1a_str(h, buf);
    if (misslog_level() && !site_named(h))
        fprintf(stderr, "[rngsite] %016llx %s|%s|%s|%d\n", (unsigned long long)h, owner_path ? owner_path : "", fsm_name ? fsm_name : "",
                state_name ? state_name : "", (int)action_index);
    return h;
}

size_t hk_rng_sizeof(void) { return sizeof(hk_rng); }

void hk_rng_set_mode(hk_rng *r, hk_rng_mode m)
{
    if (m != HK_RNG_GLOBAL && m != HK_RNG_ORACLE) HKSIM_UNIMPLEMENTED("hk_rng_mode %d (0 global, 2 oracle)", (int)m);
    if (r) r->mode = m;
}

/* --- Per-site occurrence tracking (open addressing, linear probing) --- */
static int32_t hk_rng_site_occurrence(hk_rng *r, uint64_t site)
{
    uint32_t h = (uint32_t)(site ^ (site >> 32)) * 0x9e3779b9u;
    for (uint32_t i = 0; i < HK_RNG_OCC_CAP; i++) {
        uint32_t slot = (h + i) & (HK_RNG_OCC_CAP - 1);
        if (!r->occ[slot].occupied) {
            r->occ[slot].occupied = 1;
            r->occ[slot].site = site;
            r->occ[slot].count = 1;
            r->n_occ++;
            return 0;
        }
        if (r->occ[slot].site == site) {
            uint32_t cur = r->occ[slot].count;
            r->occ[slot].count++;
            return (int32_t)cur;
        }
    }
    HKSIM_UNIMPLEMENTED("hk_rng per-site occurrence map full (cap %d)", HK_RNG_OCC_CAP);
    return 0;
}

/* --- SplitMix64 stateless word derivation --- */
static inline uint64_t splitmix64_stateless(uint64_t x)
{
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static inline uint32_t hk_rng_independent_word(int32_t master_seed, uint64_t site, int32_t occurrence)
{
    uint64_t s = site ^ ((uint64_t)(uint32_t)master_seed * 0x9e3779b97f4a7c15ULL);
    uint64_t site_seed = splitmix64_stateless(s);
    uint64_t state = site_seed + (uint64_t)(uint32_t)occurrence * 0x9e3779b97f4a7c15ULL;
    uint64_t word64 = splitmix64_stateless(state);
    return (uint32_t)word64;
}

/* --- Oracle table --- */
static inline uint32_t oracle_hash(uint64_t site, int32_t occurrence, uint32_t mask)
{
    uint64_t h = site ^ ((uint64_t)(uint32_t)occurrence * 0x9e3779b97f4a7c15ULL);
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ULL;
    return (uint32_t)(h & mask);
}

static int hk_rng_oracle_find(const hk_rng *r, uint64_t site, int32_t occurrence, uint32_t *out_word)
{
    if (!r || r->oracle_count == 0 || !r->oracle_buckets) return 0;
    uint32_t mask = (uint32_t)r->oracle_buckets_cap - 1;
    uint32_t b = oracle_hash(site, occurrence, mask);
    int32_t idx = r->oracle_buckets[b];
    while (idx >= 0 && idx < r->oracle_count) {
        if (r->oracle_entries[idx].site == site && r->oracle_entries[idx].occurrence == occurrence) {
            if (out_word) *out_word = r->oracle_entries[idx].word;
            return 1;
        }
        idx = r->oracle_next[idx];
    }
    return 0;
}

static void hk_rng_oracle_rehash(hk_rng *r, int32_t new_bucket_cap)
{
    int32_t *new_buckets = malloc((size_t)new_bucket_cap * sizeof(int32_t));
    HKSIM_ASSERT(new_buckets != NULL, "out of memory in hk_rng_oracle_rehash");
    for (int32_t i = 0; i < new_bucket_cap; i++) new_buckets[i] = -1;

    uint32_t mask = (uint32_t)new_bucket_cap - 1;
    for (int32_t i = 0; i < r->oracle_count; i++) {
        uint32_t b = oracle_hash(r->oracle_entries[i].site, r->oracle_entries[i].occurrence, mask);
        r->oracle_next[i] = new_buckets[b];
        new_buckets[b] = i;
    }
    free(r->oracle_buckets);
    r->oracle_buckets = new_buckets;
    r->oracle_buckets_cap = new_bucket_cap;
}

void hk_rng_oracle_clear(hk_rng *r)
{
    if (!r) return;
    r->oracle_count = 0;
    if (r->oracle_buckets && r->oracle_buckets_cap > 0) {
        for (int32_t i = 0; i < r->oracle_buckets_cap; i++) {
            r->oracle_buckets[i] = -1;
        }
    }
    r->oracle_hits = 0;
    r->oracle_misses = 0;
}

int32_t hk_rng_oracle_add(hk_rng *r, uint64_t site, int32_t occurrence, uint32_t word)
{
    if (!r) return -1;

    /* Check if already present -> update word */
    if (r->oracle_count > 0 && r->oracle_buckets) {
        uint32_t mask = (uint32_t)r->oracle_buckets_cap - 1;
        uint32_t b = oracle_hash(site, occurrence, mask);
        int32_t idx = r->oracle_buckets[b];
        while (idx >= 0 && idx < r->oracle_count) {
            if (r->oracle_entries[idx].site == site && r->oracle_entries[idx].occurrence == occurrence) {
                r->oracle_entries[idx].word = word;
                return r->oracle_count;
            }
            idx = r->oracle_next[idx];
        }
    }

    /* Grow entries array if needed */
    if (r->oracle_count >= r->oracle_cap) {
        int32_t new_cap = r->oracle_cap == 0 ? 64 : r->oracle_cap * 2;
        hk_rng_oracle_entry *new_entries = realloc(r->oracle_entries, (size_t)new_cap * sizeof(hk_rng_oracle_entry));
        int32_t *new_next = realloc(r->oracle_next, (size_t)new_cap * sizeof(int32_t));
        HKSIM_ASSERT(new_entries != NULL && new_next != NULL, "out of memory in hk_rng_oracle_add");
        r->oracle_entries = new_entries;
        r->oracle_next = new_next;
        r->oracle_cap = new_cap;
    }

    /* Grow hash buckets if load factor exceeds 0.75 */
    if (r->oracle_buckets_cap == 0 || (r->oracle_count + 1) * 4 >= r->oracle_buckets_cap * 3) {
        int32_t new_bcap = r->oracle_buckets_cap == 0 ? 128 : r->oracle_buckets_cap * 2;
        hk_rng_oracle_rehash(r, new_bcap);
    }

    int32_t idx = r->oracle_count++;
    r->oracle_entries[idx].site = site;
    r->oracle_entries[idx].occurrence = occurrence;
    r->oracle_entries[idx].word = word;

    uint32_t mask = (uint32_t)r->oracle_buckets_cap - 1;
    uint32_t b = oracle_hash(site, occurrence, mask);
    r->oracle_next[idx] = r->oracle_buckets[b];
    r->oracle_buckets[b] = idx;

    return r->oracle_count;
}

void hk_rng_oracle_stats(hk_rng *r, int32_t *hits, int32_t *misses, int32_t *entries)
{
    if (!r) {
        if (hits) *hits = 0;
        if (misses) *misses = 0;
        if (entries) *entries = 0;
        return;
    }
    if (hits) *hits = r->oracle_hits;
    if (misses) *misses = r->oracle_misses;
    if (entries) *entries = r->oracle_count;
}

/* --- Site-keyed draws ---
 * HK_RNG_GLOBAL (training, the calloc default): every draw steps the one shared stream.
 * HK_RNG_ORACLE (the gate): a draw keyed by (site, occurrence) returns the word the game recorded there,
 * else an independent per-site word.  Only each decision's distribution is a fidelity target, not the
 * count or order of draws (docs/porting.md).  HKSIM_RNG_MISSLOG lists misses. */
static uint32_t oracle_word(hk_rng *r, uint64_t site)
{
    int32_t occ = hk_rng_site_occurrence(r, site);
    uint32_t word = 0;
    if (hk_rng_oracle_find(r, site, occ, &word)) {
        r->oracle_hits++;
        if (misslog_level() >= 2) fprintf(stderr, "[rnghit] site=%016llx occ=%d\n", (unsigned long long)site, (int)occ);
    } else {
        r->oracle_misses++;
        if (misslog_level()) fprintf(stderr, "[rngmiss] site=%016llx occ=%d\n", (unsigned long long)site, (int)occ);
        word = hk_rng_independent_word(r->master_seed, site, occ);
    }
    return word;
}

static float word_unit(uint32_t word) { return (float)(word & 0x7FFFFFu) / 8388607.0f; }   /* as hk_rng_value */

float hk_rng_value_site(hk_rng *r, uint64_t site)
{
    if (r->mode == HK_RNG_GLOBAL) return hk_rng_value(r);
    return word_unit(oracle_word(r, site));
}

float hk_rng_range_f_site(hk_rng *r, uint64_t site, float lo, float hi)
{
    if (r->mode == HK_RNG_GLOBAL) return hk_rng_range_f(r, lo, hi);
    float u = word_unit(oracle_word(r, site));                     /* as hk_rng_range_f */
    float a = u * lo;
    float b = (1.0f - u) * hi;
    return a + b;
}

int32_t hk_rng_range_i_site(hk_rng *r, uint64_t site, int32_t lo, int32_t hi)
{
    if (lo == hi) return lo;
    if (r->mode == HK_RNG_GLOBAL) return hk_rng_range_i(r, lo, hi);
    uint32_t word = oracle_word(r, site);                          /* as hk_rng_range_i */
    if (lo < hi) return (int32_t)((uint32_t)lo + word % ((uint32_t)hi - (uint32_t)lo));
    return (int32_t)((uint32_t)lo - word % ((uint32_t)lo - (uint32_t)hi));
}
