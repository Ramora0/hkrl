#include "trap.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tls.h"
#include "alloc.h"

/* Both PER THREAD: the armed context names a stack frame, and the last message belongs to the call
 * that produced it. */
static hks_tls_key g_ctx_key, g_last_key;
HKS_CTOR hks_trap_ctor(void) { g_ctx_key = HKS_TLS_NEW(); g_last_key = HKS_TLS_NEW(); }

static hksim_trap_ctx *ctx_get(void) { return (hksim_trap_ctx *)HKS_TLS_GET(g_ctx_key); }
static void ctx_set(hksim_trap_ctx *c) { HKS_TLS_SET(g_ctx_key, c); }

/* One buffer per thread, allocated on first use and never freed.  Process heap, not an instance
 * arena: a trap can fire with no instance bound, and the message outlives the instance. */
static char *last_buf(void)
{
    char *b = (char *)HKS_TLS_GET(g_last_key);
    if (!b) {
        b = (char *)hks_sys_calloc(1, 512);
        HKS_TLS_SET(g_last_key, b);
    }
    return b;
}

void hksim_trap_arm(hksim_trap_ctx *ctx)
{
    ctx->armed = 1;
    ctx->code = HKSIM_OK;
    ctx->msg[0] = 0;
    ctx_set(ctx);
}

void hksim_trap_disarm(void)
{
    hksim_trap_ctx *c = ctx_get();
    if (c) c->armed = 0;
    ctx_set(NULL);
}

const char *hksim_trap_message(void)
{
    const char *b = (const char *)HKS_TLS_GET(g_last_key);
    return b ? b : "";
}

void hksim_trap(int code, const char *file, int line, const char *fmt, ...)
{
    char body[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    /* strip directories so messages are stable across checkouts */
    const char *base = file;
    for (const char *p = file; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    char *g_last = last_buf();
    snprintf(g_last, 512, "%s:%d: %s", base, line, body);
    hksim_trap_ctx *c = ctx_get();
    if (c && c->armed) {
        c->code = code;
        memcpy(c->msg, g_last, sizeof c->msg);
        c->armed = 0;
        ctx_set(NULL);
        longjmp(c->jb, 1);
    }
    fprintf(stderr, "hksim TRAP (unarmed): %s\n", g_last);
    fflush(stderr);
    abort();
}
