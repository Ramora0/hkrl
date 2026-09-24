#pragma once
/* Trap: anything the sim does not implement or cannot cite ABORTS with a message naming it.
 *
 *   HKSIM_UNIMPLEMENTED("FsmStateAction %s in %s/%s state '%s'", type, owner, fsm, state);
 *   HKSIM_ASSERT(cond, "invariant: ...");
 *
 * Inside an ABI call (hksim_reset / hksim_step) the trap unwinds to the ABI boundary
 * via longjmp; the call returns HKSIM_ERR_UNIMPLEMENTED (or HKSIM_ERR_INTERNAL for HKSIM_ASSERT) and
 * hksim_last_error() carries "<file>:<line>: <message>".  Outside an ABI call (unit tests driving
 * internals directly) the trap prints the message to stderr and abort()s.  Never catch a trap.
 */
#include <setjmp.h>
#include <stdint.h>
#include "hksim.h"

typedef struct {
    jmp_buf  jb;
    int      armed;
    int      code;          /* HKSIM_ERR_* set by the trap */
    char     msg[512];
} hksim_trap_ctx;

/* Arm at the ABI boundary:  if (setjmp(ctx.jb) == 0) { ctx.armed = 1; ...work...; ctx.armed = 0; }
 * else { return ctx.code; }   — exactly one armed context per thread. */
HKSIM_API void        hksim_trap_arm(hksim_trap_ctx *ctx);      /* makes ctx the current one */
HKSIM_API void        hksim_trap_disarm(void);
HKSIM_API const char *hksim_trap_message(void);                 /* last trap message ("" if none) */

HKSIM_API void hksim_trap(int code, const char *file, int line, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((noreturn, format(printf, 4, 5)))
#endif
    ;

#define HKSIM_UNIMPLEMENTED(...)  hksim_trap(HKSIM_ERR_UNIMPLEMENTED, __FILE__, __LINE__, __VA_ARGS__)
#define HKSIM_UNKNOWN HKSIM_UNIMPLEMENTED   /* spelling used by sim/fsm action files */
#define HKSIM_ASSERT(cond, ...)   do { if (!(cond)) hksim_trap(HKSIM_ERR_INTERNAL, __FILE__, __LINE__, __VA_ARGS__); } while (0)
