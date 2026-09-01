/*
 * fa_app.h - the desktop-slice application loop (RRR-41)
 *
 * Wires a platform backend (fa_platform.h) to the fixed-timestep loop
 * (fa_loop.h), the RGB565 framebuffer (fa_surface.h) and the input model
 * (fa_input.h). The game supplies a sim callback and a render callback; the
 * app owns the frame cadence, the clock read and the present.
 *
 * Frame:
 *   1. fa_input_begin_frame  (latch the previous key/pointer state)
 *   2. backend pump          (fill this frame's input; may request quit)
 *   3. read the backend clock, diff against the previous frame
 *   4. fa_loop_frame         (0..15 fixed sim ticks, then one render)
 *   5. backend present       (show the framebuffer)
 *   6. backend wait          (optional frame cap)
 *
 * The sim callback gets a pointer to a uint32 action bitmask (bit
 * (1u << fa_action) per pressed action, from the current binds). Every sim
 * tick in one frame sees the same latched mask (PL-031).
 */
#ifndef FA_APP_H
#define FA_APP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fa_platform_cfg;

/*
 * The per-frame input handed to every sim tick. `actions` is first, so old
 * consumers that do `*(const uint32_t *)input` still read the action mask.
 * The pointer / buttons are the menu input (RRR-40): framebuffer pixels,
 * latched once per frame, `btn_pressed` is the down edge.
 */
typedef struct fa_frame_input {
    uint32_t actions;        /* bit (1u << fa_action) per pressed action */
    int      ptr_x, ptr_y;
    uint32_t btn_down;       /* bit b per held mouse button (b = 0..2)   */
    uint32_t btn_pressed;    /* bit b per button that went down this frame */
    uint32_t dbg_pressed;    /* debug-key down edges, carried until a tick
                              * consumes them. bit 0 = FA_DBG_FREEMOVE (P). */
} fa_frame_input;

/* Debug-key edge bits in fa_frame_input.dbg_pressed (dev tooling only). */
#define FA_DBG_FREEMOVE  (1u << 0)   /* P: toggle free / no-clip movement */

typedef struct fa_app_cbs {
    /* One fixed 60 Hz tick. `input` points at a fa_frame_input. */
    void (*sim)(uint64_t tick, const void *input, void *user);

    /* One render. Draw into the RGB565 framebuffer `fb` (w*h, `pitch` bytes
     * per row). `alpha` is the interpolation factor in [0,1). */
    void (*render)(double alpha, uint16_t *fb, int w, int h, size_t pitch,
                   void *user);

    /* Optional: fill `buf` with up to `max_frames` interleaved S16 stereo
     * frames for this frame's audio, at `rate` Hz / `channels` channels.
     * Return the number of frames written (0 = silence). Called once per
     * rendered frame, after render. RRR-46 feeds the real mixer here; the
     * slice uses it for a test tone. */
    int  (*audio)(int16_t *buf, int max_frames, int rate, int channels,
                  void *user);

    /* Optional: called once after the backend and framebuffer exist, before
     * the first frame. Return non-zero to abort the run. */
    int  (*on_start)(void *user);

    /* Optional: checked once per frame after sim; non-zero ends the loop
     * (the game asking to quit, e.g. a menu ESCI button). */
    int  (*should_quit)(void *user);

    void *user;
} fa_app_cbs;

typedef struct fa_app_stats {
    uint64_t frames;
    uint64_t ticks;
    uint64_t clamped_frames;
    uint32_t final_frame_hash;   /* meaningful with the null backend */
    const char *backend;
} fa_app_stats;

/*
 * Run the loop. `max_frames` <= 0 runs until the backend reports quit;
 * > 0 stops after that many rendered frames (headless capture, tests).
 * `stats` may be NULL. Returns 0 on a clean exit, -1 on a setup failure.
 */
int fa_app_run(const struct fa_platform_cfg *cfg, const fa_app_cbs *cbs,
               long max_frames, fa_app_stats *stats);

#ifdef __cplusplus
}
#endif

#endif /* FA_APP_H */
