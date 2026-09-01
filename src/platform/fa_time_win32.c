/*
 * fa_time_win32.c - Windows clock backend (RRR-33 timing layer).
 * QueryPerformanceCounter for the monotonic clock, timeGetTime for the
 * menu / attract clock - the same split the original uses (RRR-9).
 */
#if defined(_WIN32)

#include "fa/fa_time.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>   /* timeGetTime (winmm) */

uint64_t fa_time_now_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;

    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);

    /* now / freq * 1e9, split to avoid overflow on the multiply. */
    {
        uint64_t q = (uint64_t)now.QuadPart / (uint64_t)freq.QuadPart;
        uint64_t r = (uint64_t)now.QuadPart % (uint64_t)freq.QuadPart;
        return q * 1000000000ull + (r * 1000000000ull) / (uint64_t)freq.QuadPart;
    }
}

uint64_t fa_time_wall_ms(void)
{
    return (uint64_t)timeGetTime();
}

#endif /* _WIN32 */
