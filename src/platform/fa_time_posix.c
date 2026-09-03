/*
 * fa_time_posix.c - POSIX clock backend.
 * CLOCK_MONOTONIC for the simulation clock, CLOCK_REALTIME for the
 * menu / attract clock. Used by the Linux / macOS desktop build and as the
 * base for later SDL / console backends.
 */
#if !defined(_WIN32)

#include "fa/fa_time.h"
#include <time.h>

uint64_t fa_time_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t fa_time_wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

#endif /* !_WIN32 */
