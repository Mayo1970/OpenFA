/*
 * fa_time.h - timing abstraction
 *
 * Two clocks, kept separate as the original does:
 *
 *  - fa_time_now_ns: monotonic high-resolution counter (QueryPerformanceCounter
 *    path). Paces the main loop. Never wall-clock; immune to NTP steps or DST.
 *
 *  - fa_time_wall_ms: free-running millisecond clock (timeGetTime path). Drives
 *    only the menu / attract animations. Kept apart from the simulation clock.
 */
#ifndef FA_TIME_H
#define FA_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic nanoseconds since an unspecified origin. Strictly non-decreasing. */
uint64_t fa_time_now_ns(void);

/* Free-running milliseconds since an unspecified origin. Menu / attract only. */
uint64_t fa_time_wall_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* FA_TIME_H */
