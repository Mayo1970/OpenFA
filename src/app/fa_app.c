/*
 * fa_app.c - the desktop-slice application loop (RRR-41)
 * See include/fa/fa_app.h for the frame model.
 */
#include "fa/fa_app.h"
#include "fa/fa_platform.h"
#include "fa/fa_loop.h"
#include "fa/fa_surface.h"
#include "fa/fa_input.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const fa_app_cbs *cbs;
    fa_frame_input    fi;            /* latched once per frame */
} app_ctx;

static void app_sim(uint64_t tick, const void *input, void *user)
{
    app_ctx *a = (app_ctx *)user;
    if (a->cbs->sim) a->cbs->sim(tick, input, a->cbs->user);
}

/* Build the uint32 action bitmask from the current input and binds. */
static uint32_t actions_of(const fa_input *in)
{
    uint32_t m = 0;
    for (int a = 0; a < FA_ACT_COUNT; a++)
        if (fa_input_action_down(in, (fa_action)a)) m |= (1u << a);
    return m;
}

int fa_app_run(const fa_platform_cfg *cfg, const fa_app_cbs *cbs,
               long max_frames, fa_app_stats *stats)
{
    if (!cbs) return -1;

    fa_platform pf;
    if (fa_platform_create(&pf, cfg) != 0) return -1;

    fa_surface fb;
    if (fa_surface_alloc(&fb, pf.width, pf.height, 0) != 0) {
        pf.vt->shutdown(&pf);
        return -1;
    }

    fa_input in;
    fa_input_init(&in);
    fa_input_set_pointer_bounds(&in, pf.width, pf.height);

    app_ctx actx;
    memset(&actx, 0, sizeof actx);
    actx.cbs = cbs;
    fa_loop lp;
    fa_loop_init(&lp, app_sim, NULL, &actx);

    if (cbs->on_start && cbs->on_start(cbs->user)) {
        fa_surface_free(&fb);
        pf.vt->shutdown(&pf);
        return -1;
    }

    uint64_t prev = pf.vt->now_ns(&pf);
    uint64_t rendered = 0, clamped = 0;
    int quit = 0;
    uint32_t carry_pressed = 0;   /* button down-edges held until a tick sees them */
    uint32_t carry_dbg = 0;       /* debug-key down-edges, same hold rule */

    const uint64_t frame_cap_ns = 1000000000ull / 240ull;  /* soft ceiling */

    while (!quit) {
        fa_input_begin_frame(&in);
        quit = pf.vt->pump(&pf, &in);

        actx.fi.actions = actions_of(&in);
        fa_input_pointer(&in, &actx.fi.ptr_x, &actx.fi.ptr_y);
        actx.fi.btn_down = 0;
        for (int b = 0; b < 3; b++) {
            if (fa_input_button_down(&in, b))    actx.fi.btn_down |= (1u << b);
            if (fa_input_button_pressed(&in, b)) carry_pressed |= (1u << b);
        }
        actx.fi.btn_pressed = carry_pressed;
        if (fa_input_key_pressed(&in, FA_DIK_P)) carry_dbg |= FA_DBG_FREEMOVE;
        actx.fi.dbg_pressed = carry_dbg;

        uint64_t now = pf.vt->now_ns(&pf);
        uint64_t dt  = now - prev;
        prev = now;

        fa_loop_frame(&lp, dt, &actx.fi);
        clamped += (unsigned)lp.last_clamped;
        if (lp.last_steps > 0) { carry_pressed = 0; carry_dbg = 0; }   /* a tick consumed the edge */

        if (cbs->should_quit && cbs->should_quit(cbs->user)) quit = 1;

        if (cbs->render)
            cbs->render(fa_loop_alpha(&lp), fb.px, fb.w, fb.h, fb.pitch,
                        cbs->user);

        pf.vt->present(&pf, fb.px, fb.w, fb.h, fb.pitch);
        rendered++;

        if (cbs->audio && pf.vt->audio_push) {
            int want = pf.vt->audio_want ? pf.vt->audio_want(&pf)
                                         : pf.audio_rate / 60;
            if (want > 4096) want = 4096;
            if (want > 0) {
                static int16_t abuf[4096 * 2];
                int got = cbs->audio(abuf, want, pf.audio_rate,
                                     pf.audio_channels, cbs->user);
                if (got > 0) pf.vt->audio_push(&pf, abuf, got);
            }
        }

        if (pf.vt->wait) {
            uint64_t spent = pf.vt->now_ns(&pf) - now;
            if (spent < frame_cap_ns) pf.vt->wait(&pf, frame_cap_ns - spent);
        }

        if (max_frames > 0 && rendered >= (uint64_t)max_frames) quit = 1;
    }

    if (stats) {
        stats->frames         = rendered;
        stats->ticks          = lp.sim_tick;
        stats->clamped_frames = clamped;
        stats->final_frame_hash =
            (strcmp(pf.name, "null") == 0) ? fa_backend_null_frame_hash(&pf) : 0u;
        stats->backend = pf.name;
    }

    fa_surface_free(&fb);
    pf.vt->shutdown(&pf);
    return 0;
}
