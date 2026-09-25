/* Single-thread speed of one hksim build, driven the way train/sim_worker.py drives it: batch observation
 * only (HKSIM_OBS_BATCH), EpisodeStart's per-episode health at every episode start,
 * random actions, hksim_obs_batch after every step, reset on done.  Timed in C so the number is the sim's,
 * not ctypes'.  Built and run by tools/sim_bench.py.
 *
 *   simbench <dll> <level> <steps> <seed> [--invuln] [--core N] [--profile <out.samples>] [--no-retrap]
 *
 * A trap (a known sim gap under random play) replaces the instance with a fresh one and the timing goes on;
 * the count is printed (traps=).  --no-retrap stops at the first one instead.
 *
 * --profile samples the stepping thread's call stack every millisecond (suspend, RtlVirtualUnwind over the
 * DLL's unwind tables) and writes each stack as DLL-relative addresses; tools/sim_bench.py symbolises them. */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *level; uint32_t fpw; int32_t seed; uint32_t trace; uint32_t srf, fc0; float t0, tsll0; } cfg_t;
typedef struct { uint32_t done; float dl, ht, hh; uint32_t frame; } res_t;
typedef struct {
    int32_t n_sims, cap_c, cap_t;
    float *combat; int32_t *kind, *parent, *n_combat; float *terrain; int32_t *n_terrain; float *gs; float *step; uint8_t *done;
} batch_t;
typedef void *(*create_f)(const cfg_t *);
typedef int (*reset_f)(void *, int32_t);
typedef int (*step_f)(void *, const int32_t *, res_t *);
typedef int (*set_value_f)(void *, const char *, double);
typedef void (*set_obs_mode_f)(void *, int32_t);
typedef int (*obs_batch_f)(void *const *, int32_t, void *, batch_t *);
typedef void *(*vocab_create_f)(int32_t);
typedef const char *(*last_error_f)(void *);
typedef void (*destroy_f)(void *);

static HANDLE g_main;
static volatile LONG g_stop;
static FILE *g_prof;
static uintptr_t g_base, g_end;
static uint64_t g_nsamp;

static DWORD WINAPI sampler(LPVOID p)
{
    (void)p;
    while (!g_stop) {
        Sleep(1);
        if (SuspendThread(g_main) == (DWORD)-1) break;
        CONTEXT ctx; memset(&ctx, 0, sizeof ctx); ctx.ContextFlags = CONTEXT_FULL;
        uint64_t st[64]; int n = 0;
        if (GetThreadContext(g_main, &ctx)) {
            while (n < 64) {
                st[n++] = ctx.Rip;
                DWORD64 ib; PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &ib, NULL);
                if (!fe) {                                  /* leaf without unwind data: return address on top */
                    if (n > 1) break;
                    ctx.Rip = *(DWORD64 *)ctx.Rsp; ctx.Rsp += 8;
                    continue;
                }
                PVOID hd; DWORD64 ef;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, ctx.Rip, fe, &ctx, &hd, &ef, NULL);
                if (!ctx.Rip) break;
            }
        }
        ResumeThread(g_main);
        uint32_t buf[64]; int32_t m = 0;
        for (int i = 0; i < n; i++) if (st[i] >= g_base && st[i] < g_end) buf[m++] = (uint32_t)(st[i] - g_base);
        if (m == 0) buf[m++] = 0xffffffffu;
        fwrite(&m, 4, 1, g_prof); fwrite(buf, 4, (size_t)m, g_prof);
        g_nsamp++;
    }
    return 0;
}

#define SYM(T, name) T name = (T)(void *)GetProcAddress(dll, #name); if (!name) { fprintf(stderr, "no %s\n", #name); return 1; }

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: simbench dll level steps seed [--invuln] [--core N] [--profile out]\n"); return 2; }
    const char *prof = NULL; int invuln = 0, core = -1, retrap = 1; long traps = 0;
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--invuln")) invuln = 1;
        else if (!strcmp(argv[i], "--core") && i + 1 < argc) core = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) prof = argv[++i];
        else if (!strcmp(argv[i], "--no-retrap")) retrap = 0;
    }
    if (core >= 0) SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)1 << core);
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll) { fprintf(stderr, "LoadLibrary(%s) failed: %lu\n", argv[1], GetLastError()); return 1; }
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)dll + ((IMAGE_DOS_HEADER *)dll)->e_lfanew);
    g_base = (uintptr_t)dll; g_end = g_base + nt->OptionalHeader.SizeOfImage;
    SYM(create_f, hksim_create)
    SYM(reset_f, hksim_reset)
    SYM(step_f, hksim_step)
    SYM(set_value_f, hksim_set_value)
    SYM(set_obs_mode_f, hksim_set_obs_mode)
    SYM(obs_batch_f, hksim_obs_batch)
    SYM(vocab_create_f, hksim_vocab_create)
    SYM(last_error_f, hksim_last_error)
    SYM(destroy_f, hksim_destroy)

    long steps = atol(argv[3]); int32_t seed = atoi(argv[4]);
    cfg_t c = { argv[2], 1, seed, 0, 0, 0, 0.0f, 0.0f };
    void *s = hksim_create(&c);
    if (!s) { fprintf(stderr, "create: %s\n", hksim_last_error(NULL)); return 1; }
    hksim_set_obs_mode(s, 2);
    void *voc = hksim_vocab_create(4096);
    enum { CC = 64, CT = 128 };                    /* train/config.py cap_combat, cap_terrain */
    static float combat[CC * 14], terrain[CT * 8], gs[33], stp[3];
    static int32_t kind[CC], parent[CC], nc, ntr;
    static uint8_t dn;
    batch_t b = { 1, CC, CT, combat, kind, parent, &nc, terrain, &ntr, gs, stp, &dn };
    uint32_t x = (uint32_t)seed * 2654435761u + 12345u;
#define RND() (x = x * 1664525u + 1013904223u, x >> 8)
    long eps = 0;
    /* train/sim_worker.py EpisodeStart with train/config.py defaults (the sim has one configuration: no keys) */
#define EPISODE_START() do { \
        if (hksim_reset(s, seed + (int32_t)eps * 7919) != 0) { fprintf(stderr, "reset: %s\n", hksim_last_error(s)); return 1; } \
        int32_t m_ = 5 + (int32_t)(RND() % 11), h_ = (RND() % 4) ? m_ : 1 + (int32_t)(RND() % (uint32_t)m_); \
        hksim_set_value(s, "hero.pd.maxHealth", m_); hksim_set_value(s, "hero.pd.health", h_); hksim_set_value(s, "hp.resync", 1); \
        hksim_obs_batch(&s, 1, voc, &b); } while (0)
    EPISODE_START();
    if (prof) {
        g_prof = fopen(prof, "wb");
        if (!g_prof) { fprintf(stderr, "cannot write %s\n", prof); return 1; }
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_main, 0, FALSE, DUPLICATE_SAME_ACCESS);
        timeBeginPeriod(1);
    }
    HANDLE th = prof ? CreateThread(NULL, 0, sampler, NULL, 0, NULL) : NULL;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    for (long i = 0; i < steps; i++) {
        uint32_t r = RND();
        int32_t a[4] = { (int32_t)(r % 3), (int32_t)((r >> 4) % 3), (int32_t)((r >> 8) % 8), (int32_t)((r >> 12) % 2) };
        res_t res;
        if (invuln) hksim_set_value(s, "hero.cstate.invulnerable", 1);
        if (hksim_step(s, a, &res) != 0) {
            /* a trap (an unported branch): an instance is never reset past one, so replace it -- a known sim gap
             * must not stop the timing of the rest (--no-retrap reports it instead) */
            if (!retrap) { fprintf(stderr, "step %ld: %s\n", i, hksim_last_error(s)); return 1; }
            traps++;
            hksim_destroy(s);
            s = hksim_create(&c);
            if (!s) { fprintf(stderr, "create: %s\n", hksim_last_error(NULL)); return 1; }
            hksim_set_obs_mode(s, 2);
            eps++; EPISODE_START();
            continue;
        }
        if (res.done) { eps++; EPISODE_START(); }
        else hksim_obs_batch(&s, 1, voc, &b);
    }
    QueryPerformanceCounter(&t1);
    if (th) { InterlockedExchange(&g_stop, 1); WaitForSingleObject(th, INFINITE); fclose(g_prof); }
    double sec = (double)(t1.QuadPart - t0.QuadPart) / (double)f.QuadPart;
    printf("us_per_step=%.3f steps=%ld episodes=%ld traps=%ld samples=%llu\n", sec * 1e6 / (double)steps, steps, eps,
           traps, (unsigned long long)g_nsamp);
    return 0;
}
