/*
 * fa_loop.c - fixed-timestep simulation loop (RRR-34)
 * See include/fa/fa_loop.h for the model and the parity basis.
 */
#include "fa/fa_loop.h"

/*
 * Exact tick boundary. A tick fires when the consumed real time reaches
 * (tick + 1) / 60 second. Working in truncated nanoseconds would drop 40 ns
 * per second (60 * 16666666 != 1e9) and drift ~2.4 us per minute, so we
 * compare against the exact rational boundary instead:
 *
 *     boundary_ns(tick) = tick * 1000000000 / 60
 *
 * u64 holds this until tick ~= 1.8e10 (about 9700 years at 60 Hz).
 */
uint64_t fa_loop_tick_boundary_ns(uint64_t tick)
{
    return tick * 1000000000ull / (uint64_t)FA_TICK_HZ;
}

void fa_loop_init(fa_loop *lp, fa_sim_fn sim, fa_render_fn render, void *user)
{
    lp->sim          = sim;
    lp->render       = render;
    lp->user         = user;
    lp->elapsed_ns   = 0;
    lp->sim_tick     = 0;
    lp->frames       = 0;
    lp->last_steps   = 0;
    lp->last_clamped = 0;
}

uint64_t fa_loop_frame(fa_loop *lp, uint64_t frame_dt_ns, const void *input)
{
    uint64_t steps = 0;

    lp->last_clamped = 0;

    /* Spiral-of-death guard: clamp a long frame so it cannot inject an
     * unbounded backlog. FA_MAX_FRAME_NS is exactly 15 ticks. */
    if (frame_dt_ns > FA_MAX_FRAME_NS) {
        frame_dt_ns      = FA_MAX_FRAME_NS;
        lp->last_clamped = 1;
    }

    lp->elapsed_ns += frame_dt_ns;

    /* Run whole ticks up to the exact boundary. The step cap is a redundant
     * hard stop: the clamp above already bounds this to 15. */
    while (steps < FA_MAX_STEPS &&
           fa_loop_tick_boundary_ns(lp->sim_tick + 1) <= lp->elapsed_ns) {
        if (lp->sim) {
            lp->sim(lp->sim_tick, input, lp->user);
        }
        lp->sim_tick++;
        steps++;
    }

    if (lp->render) {
        lp->render(fa_loop_alpha(lp), lp->user);
    }
    lp->frames++;
    lp->last_steps = steps;
    return steps;
}

double fa_loop_alpha(const fa_loop *lp)
{
    uint64_t lo   = fa_loop_tick_boundary_ns(lp->sim_tick);
    uint64_t hi   = fa_loop_tick_boundary_ns(lp->sim_tick + 1);
    uint64_t span = hi - lo;               /* 16666666 or 16666667 */
    uint64_t into = lp->elapsed_ns - lo;   /* < span after fa_loop_frame */

    if (into >= span) {
        into = span - 1;                   /* defensive; should not trigger */
    }
    return (double)into / (double)span;
}
