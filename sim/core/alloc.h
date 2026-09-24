#pragma once
/* Per-instance arena allocator.
 *
 * Every allocation an hksim instance makes comes from its own arena (bound at each ABI call), which
 * hksim_destroy releases in one piece.  hks_sys_* is the process heap, for the two things that outlive
 * any one instance: the shared per-scene action cache and the caller-owned hksim_vocab.
 *
 * A .c that allocates includes this header LAST, after <stdlib.h>; the macros below redirect it.
 */
#include <stdlib.h>
#include <stddef.h>

typedef struct hks_arena hks_arena;

/* NULL if the address space could not be reserved.  Never traps: hksim_create has no trap armed. */
hks_arena *hks_arena_create(size_t reserve);
void       hks_arena_destroy(hks_arena *a);

/* Which arena hks_malloc serves.  Thread-local; set on entry to every ABI call that can allocate;
 * returns the previous binding to restore.  free/realloc need no binding: each block records its arena. */
hks_arena *hks_arena_bind(hks_arena *a);

void  *hks_malloc(size_t n);
void  *hks_calloc(size_t n, size_t sz);
void  *hks_realloc(void *p, size_t n);
void   hks_free(void *p);
char  *hks_strdup(const char *s);

/* Checkpoints (hksim_checkpoint_*): the base never moves, so [0, used) copied out and back to the same
 * address is the whole instance, interior pointers and all.  *buf is grown with the process heap and owned
 * by the caller; pass {NULL, 0} the first time. */
size_t hks_arena_used(const hks_arena *a);
const unsigned char *hks_arena_base(const hks_arena *a);
void   hks_arena_save(const hks_arena *a, void **buf, size_t *cap, size_t *len);
void   hks_arena_load(hks_arena *a, const void *buf, size_t len);

/* The process heap, for the two shared cases named above. */
void  *hks_sys_malloc(size_t n);
void  *hks_sys_calloc(size_t n, size_t sz);
void  *hks_sys_realloc(void *p, size_t n);
void   hks_sys_free(void *p);

#ifndef HKS_ALLOC_IMPL
#define malloc  hks_malloc
#define calloc  hks_calloc
#define realloc hks_realloc
#define free    hks_free
#define strdup  hks_strdup
#endif
