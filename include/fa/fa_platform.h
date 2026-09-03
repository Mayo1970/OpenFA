/*
 * fa_platform.h - the platform backend interface (RRR-41)
 *
 * Design basis: ENGINE-ARCH section 2 ("one backend per target") and section
 * 13 ("the core links zero external libraries"). The core and game layers
 * never call an OS API; they call this interface. A backend fills one
 * fa_platform struct at start-up and hands it to fa_app_run (fa_app.h).
 *
 * Exactly one job per method, matched to what the original does (RRR-6 /
 * RRR-13 / RRR-9):
 *
 *   pump      poll OS events once per frame, fill the fa_input for this frame
 *             (keyboard -> DIK array, mouse -> menu pointer), return 1 when
 *             the user asked to quit. One poll per loop iteration (PL-031).
 *   present   copy an 800x600 RGB565 framebuffer to the screen and show it -
 *             the DirectDraw Blt-to-primary + Flip(DDFLIP_WAIT) step (PL-029).
 *   audio_push  queue interleaved S16 stereo frames at audio_rate. The RRR-30
 *             pipeline already normalises every source to S16LE 44100 stereo.
 *   now_ns    the monotonic simulation clock (fa_time_now_ns on a real
 *             backend; a fixed synthetic step on the headless one).
 *   wait      optional frame-cap sleep; a no-op is allowed.
 *
 * Backends ship in the core so a test or tool needs no window:
 *   fa_backend_null   headless. now_ns advances a fixed 1/60 s per pump, so a
 *                     bounded run is deterministic and finite; present()
 *                     folds the framebuffer into an FNV-1a-32 hash.
 *   fa_backend_sdl2   the desktop backend (RRR-41). Built only when SDL2 is
 *                     found; otherwise fa_backend_sdl2_create returns -1 and
 *                     fa_platform_create falls back to the null backend.
 *   fa_backend_switch libnx's software framebuffer and Npad input for the
 *                     Nintendo Switch application target.
 */
#ifndef FA_PLATFORM_H
#define FA_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fa_input;
typedef struct fa_platform fa_platform;

/* The original surface: 800x600x16 RGB565 (RRR-13, PL-029). */
#define FA_FB_W  800
#define FA_FB_H  600

typedef struct fa_platform_cfg {
    const char *title;           /* window title; NULL -> APP_TITLE default   */
    int         width, height;   /* framebuffer size; 0,0 -> FA_FB_W/FA_FB_H  */
    int         want_audio;      /* open an audio device                      */
    int         audio_rate;      /* 0 -> 44100                                */
    int         audio_channels;  /* 0 -> 2                                    */
    int         fullscreen;      /* desktop backend only                      */
    int         integer_scale;   /* desktop backend: keep pixel-exact scaling */
    int         window_scale;    /* desktop backend: on-screen multiple of the
                                    framebuffer; 0/1 -> native. Does not touch
                                    the framebuffer, only the window size.     */
} fa_platform_cfg;

typedef struct fa_platform_vtbl {
    int      (*pump)(fa_platform *p, struct fa_input *in);
    void     (*present)(fa_platform *p, const uint16_t *px, int w, int h,
                        size_t pitch);
    /* how many interleaved S16 frames to generate this frame to keep the
     * device fed without piling up latency (0 = the queue is full). May be
     * NULL, in which case the caller uses one tick's worth. */
    int      (*audio_want)(fa_platform *p);
    int      (*audio_push)(fa_platform *p, const int16_t *frames, int n);
    uint64_t (*now_ns)(fa_platform *p);
    void     (*wait)(fa_platform *p, uint64_t ns);
    void     (*shutdown)(fa_platform *p);
} fa_platform_vtbl;

struct fa_platform {
    const fa_platform_vtbl *vt;
    void *impl;                  /* backend private state */
    int   width, height;
    int   audio_rate, audio_channels;
    const char *name;            /* "sdl2" / "null", for logs */
};

/* --- backend factories ------------------------------------------------- */

/* Headless. Always succeeds (bad args aside). Returns 0 or -1. */
int fa_backend_null_create(fa_platform *p, const fa_platform_cfg *cfg);

/* SDL2 desktop backend. Returns 0 on success, -1 if the core was built
 * without SDL2 or the device could not be opened. */
int fa_backend_sdl2_create(fa_platform *p, const fa_platform_cfg *cfg);

/* 1 if this build has the SDL2 backend compiled in, else 0. */
int fa_backend_sdl2_available(void);

/* Nintendo Switch/libnx backend. Returns 0 on success, -1 on init failure. */
int fa_backend_switch_create(fa_platform *p, const fa_platform_cfg *cfg);

/* Try the SDL2 backend, fall back to the null backend. Returns 0 or -1. */
int fa_platform_create(fa_platform *p, const fa_platform_cfg *cfg);

/* --- null backend introspection (tests / tools) ---------------------- */

/* Rolling FNV-1a-32 over every framebuffer passed to present(). */
uint32_t fa_backend_null_frame_hash(const fa_platform *p);

/* Frames presented so far. */
uint64_t fa_backend_null_frames(const fa_platform *p);

/* Feed a scripted per-frame action bitmask into pump(). `fn` gets the frame
 * index and `ctx`; its return value becomes the fa_input key state for the
 * default binds. Pass NULL to clear. */
void fa_backend_null_set_input_fn(fa_platform *p,
                                  uint32_t (*fn)(uint64_t frame, void *ctx),
                                  void *ctx);

/* Ask the null backend to report "quit" once it has pumped `n` frames
 * (0 = never quit on its own). */
void fa_backend_null_quit_after(fa_platform *p, uint64_t n);

#ifdef __cplusplus
}
#endif

#endif /* FA_PLATFORM_H */
