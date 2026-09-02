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
 *              array; mouse motion / buttons -> the menu pointer (RRR-40);
 *              SDL_GameController -> the platform-neutral pad state.
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
    SDL_GameController *pads[FA_INPUT_MAX_PADS];
    SDL_JoystickID     pad_ids[FA_INPUT_MAX_PADS];
    int             controller_init;
    int             audio_ch;
    int             audio_rate;
    int             want_quit;
    Uint64          pf_freq;
    int             integer_scale;
} sdl_state;

static int pad_button_to_fa(SDL_GameControllerButton b)
{
    switch (b) {
    case SDL_CONTROLLER_BUTTON_A:             return FA_PAD_A;
    case SDL_CONTROLLER_BUTTON_B:             return FA_PAD_B;
    case SDL_CONTROLLER_BUTTON_X:             return FA_PAD_X;
    case SDL_CONTROLLER_BUTTON_Y:             return FA_PAD_Y;
    case SDL_CONTROLLER_BUTTON_BACK:          return FA_PAD_BACK;
    case SDL_CONTROLLER_BUTTON_GUIDE:         return FA_PAD_GUIDE;
    case SDL_CONTROLLER_BUTTON_START:         return FA_PAD_START;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return FA_PAD_LEFTSTICK;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return FA_PAD_RIGHTSTICK;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return FA_PAD_LEFTSHOULDER;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return FA_PAD_RIGHTSHOULDER;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return FA_PAD_DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return FA_PAD_DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return FA_PAD_DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return FA_PAD_DPAD_RIGHT;
    default:                                   return -1;
    }
}

static int pad_axis_to_fa(SDL_GameControllerAxis a)
{
    switch (a) {
    case SDL_CONTROLLER_AXIS_LEFTX:  return FA_PAD_AXIS_LEFT_X;
    case SDL_CONTROLLER_AXIS_LEFTY:  return FA_PAD_AXIS_LEFT_Y;
    case SDL_CONTROLLER_AXIS_RIGHTX: return FA_PAD_AXIS_RIGHT_X;
    case SDL_CONTROLLER_AXIS_RIGHTY: return FA_PAD_AXIS_RIGHT_Y;
    default:                         return -1;
    }
}

static void sdl_close_controller(sdl_state *s, int slot)
{
    if (slot < 0 || slot >= FA_INPUT_MAX_PADS) return;
    if (s->pads[slot]) SDL_GameControllerClose(s->pads[slot]);
    s->pads[slot] = NULL;
    s->pad_ids[slot] = (SDL_JoystickID)-1;
}

static int sdl_open_controller(sdl_state *s, int device_index, int slot)
{
    if (slot < 0 || slot >= FA_INPUT_MAX_PADS ||
        device_index < 0 || !SDL_IsGameController(device_index)) return -1;

    SDL_GameController *pad = SDL_GameControllerOpen(device_index);
    if (!pad) {
        fprintf(stderr, "controller: SDL_GameControllerOpen(%d) failed: %s\n",
                device_index, SDL_GetError());
        return -1;
    }

    SDL_JoystickID id =
        SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad));
    /* A controller that was already present during startup can still produce
     * a queued DEVICEADDED event on some SDL/platform combinations.  Do not
     * let that event open the same physical joystick in the other local-pad
     * slot: co-op uses the slot identity to split Penguin and Milchschnitte,
     * so a duplicate would make one controller drive both players. */
    for (int i = 0; i < FA_INPUT_MAX_PADS; i++) {
        if (i != slot && s->pads[i] && s->pad_ids[i] == id) {
            SDL_GameControllerClose(pad);
            return -1;
        }
    }

    s->pads[slot] = pad;
    s->pad_ids[slot] = id;
    fprintf(stderr, "controller %d: %s\n", slot + 1,
            SDL_GameControllerName(pad) ? SDL_GameControllerName(pad) : "?");
    return 0;
}

static void sdl_open_first_controller(sdl_state *s)
{
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        for (int slot = 0; slot < FA_INPUT_MAX_PADS; slot++) {
            if (s->pads[slot]) continue;
            if (sdl_open_controller(s, i, slot) == 0) break;
        }
    }
}

static float axis_normalize(Sint16 raw)
{
    return raw < 0 ? (float)raw / 32768.0f : (float)raw / 32767.0f;
}

static void sdl_sync_controller(sdl_state *s, struct fa_input *in)
{
    static const SDL_GameControllerButton buttons[] = {
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
        SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_GUIDE,
        SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_LEFTSTICK,
        SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_CONTROLLER_BUTTON_LEFTSHOULDER,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_DPAD_UP,
        SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
        SDL_CONTROLLER_BUTTON_DPAD_RIGHT
    };
    static const SDL_GameControllerAxis axes[] = {
        SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY,
        SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY
    };

    if (!in) return;
    for (int slot = 0; slot < FA_INPUT_MAX_PADS; slot++) {
        SDL_GameController *pad = s->pads[slot];
        if (!pad || !SDL_GameControllerGetAttached(pad)) {
            if (pad) sdl_close_controller(s, slot);
            fa_input_set_pad_connected_slot(in, slot, 0);
            continue;
        }

        fa_input_set_pad_connected_slot(in, slot, 1);
        for (unsigned i = 0; i < sizeof buttons / sizeof buttons[0]; i++) {
            int b = pad_button_to_fa(buttons[i]);
            if (b >= 0)
                fa_input_set_pad_button_slot(in, slot, (fa_pad_button)b,
                    SDL_GameControllerGetButton(pad, buttons[i]));
        }
        for (unsigned i = 0; i < sizeof axes / sizeof axes[0]; i++) {
            int a = pad_axis_to_fa(axes[i]);
            if (a >= 0)
                fa_input_set_pad_axis_slot(in, slot, (fa_pad_axis)a,
                    axis_normalize(SDL_GameControllerGetAxis(pad, axes[i])));
        }
    }

    /* Only pad 0 owns the menu pointer. Gameplay reads both slots directly. */
    fa_input_stick(in,
                   fa_input_pad_axis_slot(in, 0, FA_PAD_AXIS_LEFT_X),
                   fa_input_pad_axis_slot(in, 0, FA_PAD_AXIS_LEFT_Y));
}

/* SDL scancode -> DIK. The keys the port binds (RRR-40), the common gameplay
 * set, and the full A-Z / 0-9 / space / '-' / '.' / Backspace range needed by
 * the high-score name field; everything else is left 0. */
static int sc_to_dik(SDL_Scancode sc)
{
    /* SDL_SCANCODE_A..Z are contiguous 4..29; DIK letter codes are not. */
    static const unsigned char dik_az[26] = {
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25,
        0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F,
        0x11, 0x2D, 0x15, 0x2C
    };
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return dik_az[sc - SDL_SCANCODE_A];
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return 0x02 + (sc - SDL_SCANCODE_1);   /* DIK 1..9 */

    switch (sc) {
    case SDL_SCANCODE_0:           return 0x0B;
    case SDL_SCANCODE_MINUS:       return 0x0C;
    case SDL_SCANCODE_PERIOD:      return 0x34;
    case SDL_SCANCODE_BACKSPACE:   return FA_DIK_BACK;
    case SDL_SCANCODE_ESCAPE:      return FA_DIK_ESCAPE;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:    return FA_DIK_RETURN;
    case SDL_SCANCODE_SPACE:       return FA_DIK_SPACE;
    case SDL_SCANCODE_LEFT:        return FA_DIK_LEFT;
    case SDL_SCANCODE_RIGHT:       return FA_DIK_RIGHT;
    case SDL_SCANCODE_UP:          return FA_DIK_UP;
    case SDL_SCANCODE_DOWN:        return FA_DIK_DOWN;
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
        case SDL_CONTROLLERDEVICEADDED:
            if (s->controller_init) {
                for (int slot = 0; slot < FA_INPUT_MAX_PADS; slot++) {
                    if (s->pads[slot]) continue;
                    if (sdl_open_controller(s, e.cdevice.which, slot) == 0) break;
                }
            }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            for (int slot = 0; slot < FA_INPUT_MAX_PADS; slot++)
                if (s->pads[slot] && e.cdevice.which == s->pad_ids[slot])
                    sdl_close_controller(s, slot);
            break;
        default: break;
        }
    }
    sdl_sync_controller(s, in);
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
        for (int slot = 0; slot < FA_INPUT_MAX_PADS; slot++)
            sdl_close_controller(s, slot);
        if (s->audio) SDL_CloseAudioDevice(s->audio);
        if (s->tex)   SDL_DestroyTexture(s->tex);
        if (s->ren)   SDL_DestroyRenderer(s->ren);
        if (s->win)   SDL_DestroyWindow(s->win);
        free(s);
    }
    p->impl = NULL;
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS |
                      SDL_INIT_GAMECONTROLLER);
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
    for (int slot = 0; slot < FA_INPUT_MAX_PADS; slot++)
        s->pad_ids[slot] = (SDL_JoystickID)-1;
    s->integer_scale = cfg ? cfg->integer_scale : 0;

    Uint32 wflags = SDL_WINDOW_RESIZABLE |
                    ((cfg && cfg->fullscreen) ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    /* window_scale only enlarges the window; the framebuffer stays w x h and
     * SDL_RenderSetLogicalSize scales it up to fill. */
    int ws = (cfg && cfg->window_scale > 1) ? cfg->window_scale : 1;
    s->win = SDL_CreateWindow(cfg && cfg->title ? cfg->title : "Ferrero Jump&Run",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              w * ws, h * ws, wflags);
    if (!s->win) goto fail;

    /* No SDL_RENDERER_PRESENTVSYNC: the fixed-timestep loop owns cadence
     * and timing must not depend on the refresh (RRR-41 AC4). */
    s->ren = SDL_CreateRenderer(s->win, -1, SDL_RENDERER_ACCELERATED);
    if (!s->ren) s->ren = SDL_CreateRenderer(s->win, -1, 0);
    if (!s->ren) goto fail;
    /* No SDL_RenderSetLogicalSize: sdl_present / map_pointer do their own
     * letterbox math in real output pixels. Setting a logical size here made
     * SDL transform those coords a second time -> tiny, off-centre image. */

    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0) {
        s->controller_init = 1;
        sdl_open_first_controller(s);
    } else {
        fprintf(stderr, "controller: SDL_INIT_GAMECONTROLLER failed: %s\n",
                SDL_GetError());
    }

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
    for (int slot = 0; slot < FA_INPUT_MAX_PADS; slot++)
        sdl_close_controller(s, slot);
    if (s->ren) SDL_DestroyRenderer(s->ren);
    if (s->win) SDL_DestroyWindow(s->win);
    free(s);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS |
                      SDL_INIT_GAMECONTROLLER);
    return -1;
}

#endif /* FA_HAVE_SDL2 */
