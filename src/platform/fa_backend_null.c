/*
 * fa_backend_null.c - headless platform backend (RRR-41)
 *
 * No window, no device. The simulation clock advances a fixed 1/60 s per
 * pump(), so a bounded run is deterministic on every host and finite without
 * a wall clock. present() folds the framebuffer into an FNV-1a-32 hash so a
 * test can assert the rendered output. Audio is counted and dropped.
 *
 * This backend is what tools/sim_replay, the CI job and every headless test
 * use, and it is the fallback when SDL2 is not built.
 */
#include "fa/fa_platform.h"
#include "fa/fa_input.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t pumps;                      /* pump() calls so far */
    uint64_t clock_ns;                   /* = pumps * 1e9 / 60, exact */
    uint64_t frames;                     /* present() calls so far */
    uint64_t audio_frames;
    uint32_t hash;

    uint64_t quit_after;                 /* 0 = never */
    uint32_t (*input_fn)(uint64_t, void *);
    void     *input_ctx;
} null_state;

/* Exact tick boundary (matches fa_loop_tick_boundary_ns), so N pumps consume
 * exactly N ticks of real time with no truncation drift. */
static uint64_t null_boundary_ns(uint64_t n)
{
    return n * 1000000000ull / 60ull;
}

static const int FA_ACT_DIK[FA_ACT_COUNT] = {
    FA_DIK_LEFT, FA_DIK_RIGHT, FA_DIK_UP, FA_DIK_DOWN,
    FA_DIK_A, FA_DIK_S, FA_DIK_F, FA_DIK_D
};

static int null_pump(fa_platform *p, struct fa_input *in)
{
    null_state *s = (null_state *)p->impl;

    if (in && s->input_fn) {
        uint32_t mask = s->input_fn(s->pumps, s->input_ctx);
        for (int a = 0; a < FA_ACT_COUNT; a++)
            fa_input_set_key(in, FA_ACT_DIK[a], (mask >> a) & 1u);
    }

    s->pumps++;
    s->clock_ns = null_boundary_ns(s->pumps);
    return (s->quit_after && s->pumps >= s->quit_after) ? 1 : 0;
}

static void null_present(fa_platform *p, const uint16_t *px, int w, int h,
                         size_t pitch)
{
    null_state *s = (null_state *)p->impl;
    uint32_t hsh = s->hash;

    for (int y = 0; y < h; y++) {
        const uint16_t *row = (const uint16_t *)((const uint8_t *)px +
                                                 (size_t)y * pitch);
        for (int x = 0; x < w; x++) {
            uint16_t v = row[x];
            hsh ^= (uint32_t)(v & 0xffu);      hsh *= 16777619u;
            hsh ^= (uint32_t)((v >> 8) & 0xffu); hsh *= 16777619u;
        }
    }
    s->hash = hsh;
    s->frames++;
}

static int null_audio_want(fa_platform *p)
{
    /* one 60 Hz tick's worth - deterministic, independent of any clock */
    return p->audio_rate > 0 ? p->audio_rate / 60 : 0;
}

static int null_audio_push(fa_platform *p, const int16_t *frames, int n)
{
    null_state *s = (null_state *)p->impl;
    (void)frames;
    if (n < 0) n = 0;
    s->audio_frames += (uint64_t)n;
    return n;
}

static uint64_t null_now_ns(fa_platform *p)
{
    return ((null_state *)p->impl)->clock_ns;
}

static void null_wait(fa_platform *p, uint64_t ns) { (void)p; (void)ns; }

static void null_shutdown(fa_platform *p)
{
    free(p->impl);
    p->impl = NULL;
}

static const fa_platform_vtbl NULL_VT = {
    null_pump, null_present, null_audio_want, null_audio_push, null_now_ns,
    null_wait, null_shutdown
};

int fa_backend_null_create(fa_platform *p, const fa_platform_cfg *cfg)
{
    if (!p) return -1;

    null_state *s = (null_state *)calloc(1, sizeof *s);
    if (!s) return -1;
    s->hash = 2166136261u;

    memset(p, 0, sizeof *p);
    p->vt   = &NULL_VT;
    p->impl = s;
    p->name = "null";
    p->width  = (cfg && cfg->width  > 0) ? cfg->width  : FA_FB_W;
    p->height = (cfg && cfg->height > 0) ? cfg->height : FA_FB_H;
    p->audio_rate     = (cfg && cfg->audio_rate)     ? cfg->audio_rate     : 44100;
    p->audio_channels = (cfg && cfg->audio_channels) ? cfg->audio_channels : 2;
    return 0;
}

uint32_t fa_backend_null_frame_hash(const fa_platform *p)
{
    return (p && p->impl) ? ((const null_state *)p->impl)->hash : 0u;
}

uint64_t fa_backend_null_frames(const fa_platform *p)
{
    return (p && p->impl) ? ((const null_state *)p->impl)->frames : 0u;
}

void fa_backend_null_set_input_fn(fa_platform *p,
                                  uint32_t (*fn)(uint64_t, void *), void *ctx)
{
    if (p && p->impl) {
        null_state *s = (null_state *)p->impl;
        s->input_fn  = fn;
        s->input_ctx = ctx;
    }
}

void fa_backend_null_quit_after(fa_platform *p, uint64_t n)
{
    if (p && p->impl) ((null_state *)p->impl)->quit_after = n;
}
