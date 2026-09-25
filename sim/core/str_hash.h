#pragma once
/* FNV-1a over a NUL-terminated string: the hash of the sim's string indexes (the FSM world's strings, the
 * observation vocab).  Only equality lookups use it; nothing depends on the order it gives. */
#include <stdint.h>

static inline uint32_t hks_str_hash(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}
