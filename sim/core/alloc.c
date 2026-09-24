/* The per-instance arena (alloc.h). */
#define HKS_ALLOC_IMPL 1
#include <string.h>
#include "alloc.h"
#include "trap.h"
#include "tls.h"

#if defined(_WIN32)
#  include <windows.h>
#else
#  include <sys/mman.h>
#endif

/* Address space is reserved up front and committed in chunks, so the base never moves; reserving costs
 * no memory -- committed pages are the resident set. */
#define HKS_RESERVE_DEFAULT ((size_t)128u << 20)
#define HKS_COMMIT_CHUNK    ((size_t)256u << 10)   /* the resident set rounds up to this */

/* Size classes: 16-byte granular to 4 KB, then 4 KB granular, so waste is under 4096 bytes per block.
 * Not power-of-two: the ~1.5 MB FSM world block would round to 2 MB. */
#define HKS_SMALL_MAX  4096u
#define HKS_SMALL_BINS 256u                        /* bins 1..256, capacity bin*16 */
#define HKS_LARGE_BINS 4096u                       /* bins 258.., capacity (bin-256)*4096, max 16 MB */
#define HKS_NBINS      (HKS_SMALL_BINS + HKS_LARGE_BINS + 2u)

#define HKS_BLK_HDR    16u                         /* keeps every payload 16-byte aligned */
#define HKS_MAGIC      0x484b4c56u                 /* live */
#define HKS_FREED      0x484b4644u                 /* on a free list */

/* Every block names its own arena, so free and realloc never consult the thread-local binding
 * (the header pads to 16 bytes anyway). */
typedef struct { uint32_t bin, magic; hks_arena *owner; } blk_t;

/* Head of the region: bump pointer and per-class free lists (offsets from base). */
typedef struct { size_t used; size_t bin[HKS_NBINS]; } hdr_t;

struct hks_arena {
    unsigned char *base;        /* the reserved region; hdr_t sits at offset 0 */
    size_t reserved, committed;
    size_t used_hi;             /* high-water mark of used */
};

/* Which arena a fresh allocation comes from, per thread (tls.h). */
static hks_tls_key g_cur_key;
HKS_CTOR hks_alloc_ctor(void) { g_cur_key = HKS_TLS_NEW(); }
#define g_cur ((hks_arena *)HKS_TLS_GET(g_cur_key))

/* Allocations made with no instance bound: the standalone unit-test entry points (hkfsm_world_create,
 * phys_create, hero_*) drive modules with no hksim around them.  An hksim instance never lands here. */
static hks_arena *g_default;
static hks_mutex  g_default_lock;
HKS_CTOR hks_default_ctor(void) { HKS_MUTEX_INIT(&g_default_lock); }

/* ------------------------------------------------------------------------------ system heap */
void *hks_sys_malloc(size_t n)            { return malloc(n); }
void *hks_sys_calloc(size_t n, size_t sz) { return calloc(n, sz); }
void *hks_sys_realloc(void *p, size_t n)  { return realloc(p, n); }
void  hks_sys_free(void *p)               { free(p); }

/* ------------------------------------------------------------------------------ region */
static void *vm_reserve(size_t n)
{
#if defined(_WIN32)
    return VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_READWRITE);
#else
    void *p = mmap(NULL, n, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}
static int vm_commit(void *p, size_t n)
{
#if defined(_WIN32)
    return VirtualAlloc(p, n, MEM_COMMIT, PAGE_READWRITE) != NULL;
#else
    return mprotect(p, n, PROT_READ | PROT_WRITE) == 0;
#endif
}
static void vm_release(void *p, size_t n)
{
#if defined(_WIN32)
    (void)n; VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, n);
#endif
}

hks_arena *hks_arena_create(size_t reserve)
{
    if (reserve == 0) reserve = HKS_RESERVE_DEFAULT;
    hks_arena *a = (hks_arena *)malloc(sizeof *a);
    if (!a) return NULL;
    a->base = (unsigned char *)vm_reserve(reserve);
    if (!a->base) { free(a); return NULL; }
    a->reserved = reserve;
    size_t need = (sizeof(hdr_t) + 15u) & ~(size_t)15u;
    size_t chunk = (need + HKS_COMMIT_CHUNK - 1u) & ~(HKS_COMMIT_CHUNK - 1u);
    if (!vm_commit(a->base, chunk)) { vm_release(a->base, reserve); free(a); return NULL; }
    a->committed = chunk;
    memset(a->base, 0, need);                      /* fresh pages are zero, but be explicit */
    ((hdr_t *)a->base)->used = need;
    a->used_hi = need;
    return a;
}

void hks_arena_destroy(hks_arena *a)
{
    if (!a) return;
    if (g_cur == a) HKS_TLS_SET(g_cur_key, NULL);
    vm_release(a->base, a->reserved);
    free(a);
}

hks_arena *hks_arena_bind(hks_arena *a)
{
    hks_arena *prev = g_cur;
    HKS_TLS_SET(g_cur_key, a);
    return prev;
}

size_t hks_arena_used(const hks_arena *a) { return a ? ((const hdr_t *)a->base)->used : 0; }
const unsigned char *hks_arena_base(const hks_arena *a) { return a ? a->base : NULL; }


/* ------------------------------------------------------------------------------ size classes */
static uint32_t bin_of(size_t n)
{
    if (n <= HKS_SMALL_MAX) return (uint32_t)((n + 15u) >> 4);
    size_t pages = (n + 4095u) >> 12;
    HKSIM_ASSERT(pages <= HKS_LARGE_BINS,
                 "arena: single allocation of %lu bytes exceeds the 16 MB block limit", (unsigned long)n);
    return (uint32_t)(HKS_SMALL_BINS + pages);
}
static size_t cap_of(uint32_t bin)
{
    return bin <= HKS_SMALL_BINS ? (size_t)bin * 16u : (size_t)(bin - HKS_SMALL_BINS) * 4096u;
}

/* ------------------------------------------------------------------------------ alloc / free */
static void *arena_alloc(hks_arena *a, size_t n)
{
    uint32_t bin = bin_of(n);
    hdr_t *h = (hdr_t *)a->base;
    blk_t *b;
    if (h->bin[bin]) {                                     /* a block of this class was freed: take it */
        size_t off = h->bin[bin];
        b = (blk_t *)(a->base + off);
        HKSIM_ASSERT(b->magic == HKS_FREED, "arena: free list corrupt at offset %lu", (unsigned long)off);
        h->bin[bin] = *(size_t *)((unsigned char *)b + HKS_BLK_HDR);
    } else {
        size_t cap = cap_of(bin), off = h->used, end = off + HKS_BLK_HDR + cap;
        if (end > a->committed) {
            size_t want = (end + HKS_COMMIT_CHUNK - 1u) & ~(HKS_COMMIT_CHUNK - 1u);
            HKSIM_ASSERT(want <= a->reserved,
                         "arena: out of reserved address space (%lu MB); an instance is holding far more "
                         "state than the ~4 MB a scene needs -- look for a leak before raising the reserve",
                         (unsigned long)(a->reserved >> 20));
            HKSIM_ASSERT(vm_commit(a->base + a->committed, want - a->committed), "arena: commit failed");
            a->committed = want;
        }
        h->used = end;
        if (end > a->used_hi) a->used_hi = end;
        b = (blk_t *)(a->base + off);
    }
    b->bin = bin; b->magic = HKS_MAGIC; b->owner = a;
    return (unsigned char *)b + HKS_BLK_HDR;
}

void *hks_malloc(size_t n)
{
    if (n == 0) n = 1;
    hks_arena *a = g_cur;
    if (!a) {
        /* Only the standalone unit-test entry points reach this, and they are single-threaded. */
        HKS_LOCK(&g_default_lock);
        if (!g_default) g_default = hks_arena_create(HKS_RESERVE_DEFAULT);
        HKS_UNLOCK(&g_default_lock);
        HKSIM_ASSERT(g_default != NULL, "arena: cannot reserve the default arena");
        a = g_default;
    }
    return arena_alloc(a, n);
}

void *hks_calloc(size_t n, size_t sz)
{
    size_t total = n * sz;
    HKSIM_ASSERT(sz == 0 || total / sz == n, "arena: calloc overflow");
    void *p = hks_malloc(total);
    memset(p, 0, total ? total : 1);
    return p;
}

static blk_t *block_of(void *p, const char *what)
{
    blk_t *b = (blk_t *)((unsigned char *)p - HKS_BLK_HDR);
    HKSIM_ASSERT(b->magic == HKS_MAGIC, "arena: %s of a %s pointer -- per-instance memory must come "
                 "from this allocator, and shared memory must use hks_sys_free", what,
                 b->magic == HKS_FREED ? "already-freed" : "foreign or corrupt");
    return b;
}

void hks_free(void *p)
{
    if (!p) return;
    blk_t *b = block_of(p, "free");
    hdr_t *h = (hdr_t *)b->owner->base;
    *(size_t *)p = h->bin[b->bin];                         /* the free-list link lives in the payload */
    h->bin[b->bin] = (size_t)((unsigned char *)b - b->owner->base);
    b->magic = HKS_FREED;
}

void *hks_realloc(void *p, size_t n)
{
    if (!p) return hks_malloc(n);
    if (n == 0) { hks_free(p); return NULL; }
    blk_t *b = block_of(p, "realloc");
    size_t old = cap_of(b->bin);
    /* Still fits its size class: no move, so grow-by-one arrays are amortised O(1). */
    if (n <= old) return p;
    void *q = arena_alloc(b->owner, n);                    /* stays in ITS arena, not the bound one */
    memcpy(q, p, old);
    hks_free(p);
    return q;
}

char *hks_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = (char *)hks_malloc(n);
    memcpy(p, s, n);
    return p;
}

/* ------------------------------------------------------------------------------ checkpoints */
void hks_arena_save(const hks_arena *a, void **buf, size_t *cap, size_t *len)
{
    size_t used = hks_arena_used(a);
    if (used > *cap) {
        *buf = realloc(*buf, used);
        HKSIM_ASSERT(*buf != NULL, "checkpoint: out of memory");
        *cap = used;
    }
    memcpy(*buf, a->base, used);
    *len = used;
}

void hks_arena_load(hks_arena *a, const void *buf, size_t len)
{
    HKSIM_ASSERT(len <= a->committed, "checkpoint: restoring %lu bytes into an arena with %lu committed",
                 (unsigned long)len, (unsigned long)a->committed);
    memcpy(a->base, buf, len);
    /* The header came back with the copy, so nothing live points past len.  The tail a rollout dirtied is
     * cleared anyway (to used_hi, no further) so a restored instance is the same bytes whatever ran in
     * between: fresh blocks from malloc then hold what they held the first time. */
    if (a->used_hi > len) memset(a->base + len, 0, a->used_hi - len);
    a->used_hi = len;
}
