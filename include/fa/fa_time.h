/*
 * fa_time.h - timing abstraction (RRR-33 timing layer)
 *
 * Two clocks, kept separate exactly as the original does (RRR-9):
 *
 *  - fa_time_now_ns: monotonic high-resolution counter. Port of the
 *    QueryPerformanceCounter path that paces the main loop (PL-033). Feeds
 *    fa_loop_frame. Never wall-clock; unaffected by NTP steps or DST.
 *
 *  - fa_time_wall_ms: free-running millisecond clock. Port of the timeGetTime
 *    path that RRR-9 found driving only the menu / attract animations
 *    (PL-035). Kept apart from the simulation clock on purpose.
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
