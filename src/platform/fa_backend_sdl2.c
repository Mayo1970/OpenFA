/*
 * fa_backend_sdl2.c - SDL2 desktop backend (RRR-41)
 *
 * Built only when the CMake option FA_WITH_SDL2 is on and SDL2 is found
 * (FA_HAVE_SDL2 defined). Without it the whole file compiles to two stubs so
 * the core still links and fa_platform_create falls back to the null backend.
 *
 * Maps the platform interface to SDL2:
 *   window     an 800x600 window (RRR-41 AC1); a letterboxed, optionally
 *              integer-scaled RGB565 streaming texture, so the pixel output
 *              matches the original surface exactly (RRR-13).
 *   input      SDL scancodes -> DirectInput (DIK) codes for the keyboard
 *              array; mouse motion / buttons -> the menu pointer (RRR-40).
 *   audio      SDL_AudioDeviceID + SDL_QueueAudio, S16 stereo at 44100
 *              (RRR-30 output format). No callback thread; the loop pushes.
 *   timing     SDL_GetPerformanceCounter for now_ns, independent of vsync
 *              (RRR-41 AC4); vsync is left off and the loop owns the cadence.
 */
#include "fa/fa_platform.h"
#include "fa/fa_input.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fa_backend_sdl2_available(void)
{
#if defined(FA_HAVE_SDL2)
    return 1;
#else
    return 0;
#endif
}

#if !defined(FA_HAVE_SDL2)

int fa_backend_sdl2_create(fa_platform *p, const fa_platform_cfg *cfg)
{
    (void)p; (void)cfg;
    return -1;   /* not compiled in */
}

#else /* FA_HAVE_SDL2 */

#include <SDL.h>

typedef struct {
    SDL_Window     *win;
    SDL_Renderer   *ren;
    SDL_Texture    *tex;
    int             tex_w, tex_h;
    SDL_AudioDeviceID audio;
    int             audio_ch;
    int             audio_rate;
    int             want_quit;
    Uint64          pf_freq;
    int             integer_scale;
} sdl_state;

/* SDL scancode -> DIK. Only the keys the port binds (RRR-40) plus the common
 * gameplay set; everything else is left 0. */
static int sc_to_dik(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_ESCAPE:      return FA_DIK_ESCAPE;
    case SDL_SCANCODE_RETURN:      return FA_DIK_RETURN;
    case SDL_SCANCODE_SPACE:       return FA_DIK_SPACE;
    case SDL_SCANCODE_LEFT:        return FA_DIK_LEFT;
    case SDL_SCANCODE_RIGHT:       return FA_DIK_RIGHT;
    case SDL_SCANCODE_UP:          return FA_DIK_UP;
    case SDL_SCANCODE_DOWN:        return FA_DIK_DOWN;
    case SDL_SCANCODE_A:           return FA_DIK_A;
    case SDL_SCANCODE_S:           return FA_DIK_S;
    case SDL_SCANCODE_D:           return FA_DIK_D;
    case SDL_SCANCODE_F:           return FA_DIK_F;
    case SDL_SCANCODE_P:           return FA_DIK_P;   /* free-move toggle (dev) */
    case SDL_SCANCODE_LCTRL:       return 29;   /* DIK_LCONTROL */
    case SDL_SCANCODE_LSHIFT:      return 42;   /* DIK_LSHIFT   */
    case SDL_SCANCODE_LALT:        return 56;   /* DIK_LMENU    */
    default:                       return 0;
    }
}

static void map_pointer(fa_platform *p, sdl_state *s, struct fa_input *in,
                        int wx, int wy)
{
    /* window pixel -> framebuffer pixel, honouring the letterbox. */
    int ww, wh;
    SDL_GetRendererOutputSize(s->ren, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;

    double sx = (double)ww / p->width, sy = (double)wh / p->height;
    double sc = sx < sy ? sx : sy;
    if (s->integer_scale && sc >= 1.0) sc = (double)(int)sc;
    double ox = (ww - sc * p->width) * 0.5;
    double oy = (wh - sc * p->height) * 0.5;

    int fx = (int)((wx - ox) / sc);
    int fy = (int)((wy - oy) / sc);
    if (fx < 0) fx = 0; if (fx >= p->width)  fx = p->width - 1;
    if (fy < 0) fy = 0; if (fy >= p->height) fy = p->height - 1;
    fa_input_set_pointer(in, fx, fy);
}

static int sdl_pump(fa_platform *p, struct fa_input *in)
{
    sdl_state *s = (sdl_state *)p->impl;
    SDL_Event e;

    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            s->want_quit = 1;
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            if (e.key.repeat) break;
            int dik = sc_to_dik(e.key.keysym.scancode);
            if (dik && in)
                fa_input_set_key(in, dik, e.type == SDL_KEYDOWN);
            if (e.type == SDL_KEYDOWN &&
                e.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
                ; /* leave the quit decision to the game */
            break;
        }
        case SDL_MOUSEMOTION:
            if (in) map_pointer(p, s, in, e.motion.x, e.motion.y);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            int b = e.button.button == SDL_BUTTON_LEFT   ? 0 :
                    e.button.button == SDL_BUTTON_RIGHT  ? 1 :
                    e.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
            if (b >= 0 && in)
                fa_input_set_button(in, b, e.type == SDL_MOUSEBUTTONDOWN);
            break;
        }
        default: break;
        }
    }
    return s->want_quit;
}

static void sdl_present(fa_platform *p, const uint16_t *px, int w, int h,
                        size_t pitch)
{
    sdl_state *s = (sdl_state *)p->impl;

    if (!s->tex || s->tex_w != w || s->tex_h != h) {
        if (s->tex) SDL_DestroyTexture(s->tex);
        s->tex = SDL_CreateTexture(s->ren, SDL_PIXELFORMAT_RGB565,
                                   SDL_TEXTUREACCESS_STREAMING, w, h);
        s->tex_w = w; s->tex_h = h;
    }
    if (s->tex) {
        SDL_UpdateTexture(s->tex, NULL, px, (int)pitch);

        int ww, wh;
        SDL_GetRendererOutputSize(s->ren, &ww, &wh);
        double sx = (double)ww / w, sy = (double)wh / h;
        double sc = sx < sy ? sx : sy;
        if (s->integer_scale && sc >= 1.0) sc = (double)(int)sc;
        SDL_Rect dst;
        dst.w = (int)(sc * w); dst.h = (int)(sc * h);
        dst.x = (ww - dst.w) / 2; dst.y = (wh - dst.h) / 2;

        SDL_SetRenderDrawColor(s->ren, 0, 0, 0, 255);
        SDL_RenderClear(s->ren);
        SDL_RenderCopy(s->ren, s->tex, NULL, &dst);
        SDL_RenderPresent(s->ren);
    }
}

static int sdl_audio_want(fa_platform *p)
{
    sdl_state *s = (sdl_state *)p->impl;
    if (!s->audio || s->audio_rate <= 0) return 0;
    Uint32 bpf = (Uint32)s->audio_ch * (Uint32)sizeof(int16_t);
    if (bpf == 0) return 0;
    Uint32 target = (Uint32)s->audio_rate * bpf * 80u / 1000u;   /* 80 ms */
    Uint32 q = SDL_GetQueuedAudioSize(s->audio);
    if (q >= target) return 0;
    return (int)((target - q) / bpf);
}

static int sdl_audio_push(fa_platform *p, const int16_t *frames, int n)
{
    sdl_state *s = (sdl_state *)p->impl;
    if (!s->audio || n <= 0) return 0;
    Uint32 bpf = (Uint32)s->audio_ch * (Uint32)sizeof(int16_t);
    if (SDL_QueueAudio(s->audio, frames, (Uint32)n * bpf) != 0)
        return 0;
    return n;
}

static uint64_t sdl_now_ns(fa_platform *p)
{
    sdl_state *s = (sdl_state *)p->impl;
    Uint64 c = SDL_GetPerformanceCounter();
    Uint64 q = c / s->pf_freq, r = c % s->pf_freq;
    return (uint64_t)q * 1000000000ull +
           (uint64_t)r * 1000000000ull / (uint64_t)s->pf_freq;
}

static void sdl_wait(fa_platform *p, uint64_t ns)
{
    (void)p;
    if (ns >= 1000000ull) SDL_Delay((Uint32)(ns / 1000000ull));
}

static void sdl_shutdown(fa_platform *p)
{
    sdl_state *s = (sdl_state *)p->impl;
    if (s) {
        if (s->audio) SDL_CloseAudioDevice(s->audio);
        if (s->tex)   SDL_DestroyTexture(s->tex);
        if (s->ren)   SDL_DestroyRenderer(s->ren);
        if (s->win)   SDL_DestroyWindow(s->win);
        free(s);
    }
    p->impl = NULL;
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
    if (!SDL_WasInit(0)) SDL_Quit();
}

static const fa_platform_vtbl SDL_VT = {
    sdl_pump, sdl_present, sdl_audio_want, sdl_audio_push, sdl_now_ns,
    sdl_wait, sdl_shutdown
};

int fa_backend_sdl2_create(fa_platform *p, const fa_platform_cfg *cfg)
{
    if (!p) return -1;

    int w  = (cfg && cfg->width  > 0) ? cfg->width  : FA_FB_W;
    int h  = (cfg && cfg->height > 0) ? cfg->height : FA_FB_H;
    int rate = (cfg && cfg->audio_rate)     ? cfg->audio_rate     : 44100;
    int chn  = (cfg && cfg->audio_channels) ? cfg->audio_channels : 2;

    /* The app owns main() (SDL_MAIN_HANDLED); tell SDL so before any init. */
    SDL_SetMainReady();

    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
        return -1;

    sdl_state *s = (sdl_state *)calloc(1, sizeof *s);
    if (!s) { SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS); return -1; }
    s->pf_freq = SDL_GetPerformanceFrequency();
    if (s->pf_freq == 0) s->pf_freq = 1;
    s->integer_scale = cfg ? cfg->integer_scale : 0;

    Uint32 wflags = SDL_WINDOW_RESIZABLE |
                    ((cfg && cfg->fullscreen) ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    s->win = SDL_CreateWindow(cfg && cfg->title ? cfg->title : "Ferrero Jump&Run",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              w, h, wflags);
    if (!s->win) goto fail;

    /* No SDL_RENDERER_PRESENTVSYNC: the fixed-timestep loop owns cadence
     * and timing must not depend on the refresh (RRR-41 AC4). */
    s->ren = SDL_CreateRenderer(s->win, -1, SDL_RENDERER_ACCELERATED);
    if (!s->ren) s->ren = SDL_CreateRenderer(s->win, -1, 0);
    if (!s->ren) goto fail;
    SDL_RenderSetLogicalSize(s->ren, w, h);

    if (cfg && cfg->want_audio) {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = rate;
        want.format = AUDIO_S16SYS;
        want.channels = (Uint8)chn;
        want.samples = 1024;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "audio: SDL_INIT_AUDIO failed: %s\n", SDL_GetError());
        } else {
            /* NULL device + ALLOW_ANY_CHANGE: take whatever the default
             * device offers and adapt to it (some WASAPI defaults are not
             * 44100 / 2ch). */
            s->audio = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                           SDL_AUDIO_ALLOW_FREQUENCY_CHANGE |
                                           SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
            if (!s->audio) {
                fprintf(stderr, "audio: SDL_OpenAudioDevice failed: %s\n",
                        SDL_GetError());
            } else {
                rate = have.freq;
                chn  = have.channels ? have.channels : 2;
                s->audio_rate = rate;
                s->audio_ch   = chn;
                SDL_PauseAudioDevice(s->audio, 0);
                fprintf(stderr, "audio: driver '%s', device open, %d Hz, "
                        "%d ch, fmt 0x%04x\n",
                        SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver()
                                                    : "?",
                        rate, chn, have.format);
            }
        }
    }

    memset(p, 0, sizeof *p);
    p->vt   = &SDL_VT;
    p->impl = s;
    p->name = "sdl2";
    p->width = w; p->height = h;
    p->audio_rate = rate; p->audio_channels = chn;
    return 0;

fail:
    if (s->ren) SDL_DestroyRenderer(s->ren);
    if (s->win) SDL_DestroyWindow(s->win);
    free(s);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS);
    return -1;
}

#endif /* FA_HAVE_SDL2 */
