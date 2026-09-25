/* hksim.h fast path: the vocab and the numeric emit.  See obs_batch.h. */
#include <stdlib.h>
#include <string.h>
#include "obs_batch.h"
#include "core/alloc.h"
#include "core/str_hash.h"

/* ------------------------------------------------------------------------------------- vocab
 * Reserved ids are added in the constructor in this order: 0 "unknown", 1 "terrain" (train/sim_worker.py's
 * VOCAB_RESERVED must match).  encode() maps NULL/"" to unknown, returns an existing id, and otherwise
 * appends -- unless the table is already at max_size, when it returns unknown instead of growing.  Ids
 * are therefore arrival-ordered and stable. */
struct hksim_vocab {
    char   **i2s;
    int32_t  n, cap, max_size;
    int32_t *ix; uint32_t mask;        /* open-addressing index of i2s by hks_str_hash, at most half full */
};

static void vocab_index_put(hksim_vocab *v, int32_t id)
{
    uint32_t k = hks_str_hash(v->i2s[id]) & v->mask;
    while (v->ix[k] >= 0) k = (k + 1) & v->mask;
    v->ix[k] = id;
}

/* Caller-owned and shared by every instance, so it uses the process heap, not an instance arena. */
hksim_vocab *hksim_vocab_create(int32_t max_size)
{
    hksim_vocab *v = hks_sys_calloc(1, sizeof *v);
    if (!v) return NULL;
    v->max_size = max_size > 0 ? max_size : 512;   /* max_size<=0 fallback; the trainer always passes kind_vocab_size (train/config.py) */
    v->cap = 64;
    v->i2s = hks_sys_calloc((size_t)v->cap, sizeof *v->i2s);
    if (!v->i2s) { hks_sys_free(v); return NULL; }
    hksim_vocab_intern(v, "unknown");
    hksim_vocab_intern(v, "terrain");
    return v;
}

void hksim_vocab_destroy(hksim_vocab *v)
{
    if (!v) return;
    for (int32_t i = 0; i < v->n; i++) hks_sys_free(v->i2s[i]);
    hks_sys_free(v->i2s);
    hks_sys_free(v->ix);
    hks_sys_free(v);
}

int32_t hksim_vocab_size(const hksim_vocab *v) { return v ? v->n : 0; }

const char *hksim_vocab_str(const hksim_vocab *v, int32_t id)
{
    return (v && id >= 0 && id < v->n) ? v->i2s[id] : NULL;
}

int32_t hksim_vocab_intern(hksim_vocab *v, const char *str)
{
    if (!v) return 0;
    if (!str || !str[0]) return 0;                      /* vocab.py:33-35 -> unknown */
    uint32_t h = hks_str_hash(str);
    if (v->ix)
        for (uint32_t k = h & v->mask; v->ix[k] >= 0; k = (k + 1) & v->mask)
            if (strcmp(v->i2s[v->ix[k]], str) == 0) return v->ix[k];
    if (v->n >= v->max_size) return 0;                  /* vocab.py:39-49 overflow sink */
    if (!v->ix || 2u * (uint32_t)(v->n + 1) > v->mask + 1u) {
        uint32_t size = 64;
        while (size < 2u * (uint32_t)(v->n + 1)) size *= 2;
        int32_t *ix = hks_sys_malloc(sizeof(int32_t) * size);
        if (!ix) return 0;
        hks_sys_free(v->ix);
        v->ix = ix; v->mask = size - 1;
        memset(v->ix, 0xff, sizeof(int32_t) * size);
        for (int32_t i = 0; i < v->n; i++) vocab_index_put(v, i);
    }
    if (v->n == v->cap) {
        int32_t cap = v->cap * 2;
        char **p = hks_sys_realloc(v->i2s, (size_t)cap * sizeof *p);
        if (!p) return 0;
        v->i2s = p; v->cap = cap;
    }
    size_t len = strlen(str) + 1;
    char *copy = hks_sys_malloc(len);
    if (!copy) return 0;
    memcpy(copy, str, len);
    v->i2s[v->n] = copy;
    vocab_index_put(v, v->n);
    return v->n++;
}

/* ------------------------------------------------------------------------------------- emit */
void nb_free(obs_numeric *nb)
{
    if (!nb) return;
    free(nb->terrain);
    nb->terrain = NULL;
    nb->terrain_cap = 0;
}

void nb_emit(const obs_numeric *nb, hksim_batch *out, int32_t slot, hksim_vocab *vocab)
{
    if (out->n_combat) out->n_combat[slot] = nb->n_combat;
    if (out->n_terrain) out->n_terrain[slot] = nb->n_terrain;
    if (out->done) out->done[slot] = nb->done;
    if (out->global_state)
        memcpy(out->global_state + (size_t)slot * NB_GLOBAL_DIM, nb->global_state, sizeof nb->global_state);
    if (out->step)
        memcpy(out->step + (size_t)slot * 3, nb->step, sizeof nb->step);

    int32_t nc = nb->n_combat < out->cap_combat ? nb->n_combat : out->cap_combat;
    if (out->combat && nc > 0)
        memcpy(out->combat + (size_t)slot * out->cap_combat * NB_COMBAT_FEAT,
               nb->combat, (size_t)nc * NB_COMBAT_FEAT * sizeof(float));
    for (int32_t i = 0; i < nc; i++) {
        /* obs-wire.md 3.4/3.5: the packer's own NULL handling is "unknown" for kind and "" for the
         * clip key, and vocab.py maps both of those to id 0, so a NULL here needs no special case. */
        if (out->combat_kind)
            out->combat_kind[(size_t)slot * out->cap_combat + i] = hksim_vocab_intern(vocab, nb->kind[i]);
        if (out->combat_parent)
            out->combat_parent[(size_t)slot * out->cap_combat + i] = hksim_vocab_intern(vocab, nb->parent[i]);
    }

    int32_t nt = nb->n_terrain < out->cap_terrain ? nb->n_terrain : out->cap_terrain;
    if (out->terrain && nt > 0 && nb->terrain)
        memcpy(out->terrain + (size_t)slot * out->cap_terrain * NB_TERRAIN_FEAT,
               nb->terrain, (size_t)nt * NB_TERRAIN_FEAT * sizeof(float));
}
