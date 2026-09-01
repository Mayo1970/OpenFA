/*
 * fa_loop.h - fixed-timestep simulation loop (RRR-34)
 *
 * Parity reference (RRR-9, PL-033 / PL-034): the original game runs exactly
 * one simulation tick per rendered frame, held to the monitor vertical
 * refresh, 60 Hz on a standard display, 60 Hz fallback if the boot-time
 * measurement fails. Frame-locked behaviour (jump arcs, animation timing,
 * RNG cadence) is defined at 60 ticks per second.
 *
 * The port decouples the two. The simulation advances in fixed 60 Hz steps.
 * Rendering runs once per real frame at whatever rate the display allows.
 * A spiral-of-death guard bounds the work per frame.
 *
 * The loop is pure: it holds no clock. The caller feeds one real frame delta
 * in nanoseconds. This keeps the simulation deterministic and testable
 * without a platform clock.
 */
#ifndef FA_LOOP_H
#define FA_LOOP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed simulation rate. */
#define FA_TICK_HZ        60u

/* One tick, in nanoseconds, truncated. The loop does not use this value for
 * tick timing (it compares exact rational tick boundaries, see fa_loop.c);
 * it is exported for callers that need a nominal dt for their own maths. */
#define FA_TICK_DT_NS     (1000000000ull / FA_TICK_HZ)   /* 16666666 */

/*
 * Spiral-of-death guard.
 *
 * FA_MAX_FRAME_NS is exactly 15 ticks (0.25 s). A single frame delta is
 * clamped to it before it is added to the clock, so one stalled frame - a
 * breakpoint, a disk stall, a window drag - can never inject more than 15
 * ticks of catch-up work, and the clock can never run unbounded ahead of the
 * simulation. FA_MAX_STEPS is the same bound expressed as a tick count and is
 * enforced as a redundant hard stop inside the step loop.
 *
 * Cost: a real stall longer than 0.25 s loses wall-clock time (the simulation
 * does not fast-forward through it). That is the intended trade.
 */
#define FA_MAX_FRAME_NS   250000000ull                   /* 15 ticks, 0.25 s */
#define FA_MAX_STEPS      15u

/*
 * Simulation step. Called once per fixed tick.
 *  tick   - absolute tick index, 0-based, monotonic.
 *  input  - the frame input pointer passed to fa_loop_frame, unchanged.
 *           Every tick in one frame sees the same pointer (input is latched
 *           once per frame, as RRR-14 / RRR-9 describe: one poll per loop
 *           iteration).
 *  user   - the user pointer from fa_loop_init.
 * The step MUST NOT read a wall clock or a frame counter. Its only time
 * source is the fixed tick.
 */
typedef void (*fa_sim_fn)(uint64_t tick, const void *input, void *user);

/*
 * Render. Called once per fa_loop_frame call, after the ticks.
 *  alpha - interpolation factor in [0, 1): how far the render time is between
 *          the last completed tick and the next one. Use it to interpolate
 *          drawn positions so motion stays smooth when the render rate and
 *          the tick rate differ.
 */
typedef void (*fa_render_fn)(double alpha, void *user);

typedef struct fa_loop {
    fa_sim_fn    sim;
    fa_render_fn render;
    void        *user;

    uint64_t elapsed_ns;    /* total real time consumed, after clamping */
    uint64_t sim_tick;      /* ticks run so far */
    uint64_t frames;        /* render calls so far */

    uint64_t last_steps;    /* ticks run in the most recent fa_loop_frame */
    int      last_clamped;  /* 1 if the last frame delta hit FA_MAX_FRAME_NS */
} fa_loop;

/* Initialise. render may be NULL (headless). user may be NULL. */
void fa_loop_init(fa_loop *lp, fa_sim_fn sim, fa_render_fn render, void *user);

/*
 * Advance one real frame.
 *  frame_dt_ns - real nanoseconds since the previous frame.
 *  input       - opaque input pointer, forwarded to every sim tick this frame.
 * Runs 0..FA_MAX_STEPS sim ticks, then one render. Returns the tick count run.
 */
uint64_t fa_loop_frame(fa_loop *lp, uint64_t frame_dt_ns, const void *input);

/* Interpolation factor for the current state, [0, 1). */
double fa_loop_alpha(const fa_loop *lp);

/* Exact nanosecond timestamp of the end of tick `tick` (= start of tick+1).
 * tick * 1e9 / 60, computed without drift. Exposed for tests and tools. */
uint64_t fa_loop_tick_boundary_ns(uint64_t tick);

#ifdef __cplusplus
}
#endif

#endif /* FA_LOOP_H */
