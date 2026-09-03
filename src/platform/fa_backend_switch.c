/*
 * fa_backend_switch.c - Nintendo Switch software-rendered backend.
 *
 * The engine already renders its original 800x600 RGB565 surface in software.
 * libnx owns the application window and compositor; this backend copies each
 * completed engine surface into a linear RGB565 framebuffer and presents it.
 * Controller input is translated into the platform-neutral pad model used by
 * fa_slice, so the existing menu and gameplay code remains target-independent.
 */
#include "fa/fa_platform.h"
#include "fa/fa_input.h"

#include <switch.h>
#include <SDL2/SDL.h>

#include "fa/fa_audio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SWITCH_AUDIO_ENGINE_RATE   FA_AUDIO_RATE
#define SWITCH_AUDIO_DEVICE_RATE   48000
#define SWITCH_AUDIO_CHANNELS      2
#define SWITCH_AUDIO_TARGET_MS     80

typedef struct {
    Framebuffer fb;
    PadState    pads[FA_INPUT_MAX_PADS];
    AppletHookCookie applet_cookie;
    AppletOperationMode operation_mode;

    SDL_AudioDeviceID audio_device;
    SDL_AudioStream   *audio_stream;
    int audio_sdl_initialized;
    int audio_initialized;
    int audio_enabled;
    int audio_output_rate;
    int audio_output_channels;
} switch_state;

typedef struct {
    u64 bit;
    fa_pad_button button;
} switch_button_map;

static const switch_button_map SWITCH_BUTTONS[] = {
    { HidNpadButton_A,             FA_PAD_A             },
    { HidNpadButton_B,             FA_PAD_B             },
    { HidNpadButton_X,             FA_PAD_X             },
    { HidNpadButton_Y,             FA_PAD_Y             },
    { HidNpadButton_Minus,         FA_PAD_BACK          },
    { HidNpadButton_Plus,          FA_PAD_START         },
    { HidNpadButton_StickL,        FA_PAD_LEFTSTICK     },
    { HidNpadButton_StickR,        FA_PAD_RIGHTSTICK    },
    { HidNpadButton_L,             FA_PAD_LEFTSHOULDER  },
    { HidNpadButton_R,             FA_PAD_RIGHTSHOULDER },
    { HidNpadButton_Up,            FA_PAD_DPAD_UP       },
    { HidNpadButton_Down,          FA_PAD_DPAD_DOWN     },
    { HidNpadButton_Left,          FA_PAD_DPAD_LEFT     },
    { HidNpadButton_Right,         FA_PAD_DPAD_RIGHT    },
};

static void switch_audio_cleanup(switch_state *s);

static float switch_stick_axis(s32 value)
{
    float out = (float)value / 32767.0f;
    if (out < -1.0f) out = -1.0f;
    if (out >  1.0f) out =  1.0f;
    return out;
}

static void switch_feed_pad(fa_input *in, int slot, const PadState *pad)
{
    int connected = padIsConnected(pad);
    u64 buttons = padGetButtons(pad);
    HidAnalogStickState left = padGetStickPos(pad, 0);
    HidAnalogStickState right = padGetStickPos(pad, 1);

    fa_input_set_pad_connected_slot(in, slot, connected);
    if (!connected) return;

    for (size_t i = 0; i < sizeof SWITCH_BUTTONS / sizeof SWITCH_BUTTONS[0]; i++)
        fa_input_set_pad_button_slot(in, slot, SWITCH_BUTTONS[i].button,
                                     (buttons & SWITCH_BUTTONS[i].bit) != 0);

    fa_input_set_pad_axis_slot(in, slot, FA_PAD_AXIS_LEFT_X,
                               switch_stick_axis(left.x));
    fa_input_set_pad_axis_slot(in, slot, FA_PAD_AXIS_LEFT_Y,
                               -switch_stick_axis(left.y));
    fa_input_set_pad_axis_slot(in, slot, FA_PAD_AXIS_RIGHT_X,
                               switch_stick_axis(right.x));
    fa_input_set_pad_axis_slot(in, slot, FA_PAD_AXIS_RIGHT_Y,
                               -switch_stick_axis(right.y));
}

static void switch_applet_hook(AppletHookType hook, void *param)
{
    switch_state *s = (switch_state *)param;
    if (!s) return;

    /* The framebuffer is a compositor layer, so its fixed logical 800x600
     * surface remains valid in handheld and docked modes. Still observe the
     * operation-mode transition so the backend never assumes a stale mode if
     * a future target changes the presentation policy. */
    if (hook == AppletHookType_OnOperationMode)
        s->operation_mode = appletGetOperationMode();
}

/* ------------------------------ audio ------------------------------ */

static int switch_audio_want(fa_platform *p)
{
    switch_state *s = (switch_state *)p->impl;
    if (!s || !s->audio_enabled) return 0;

    const uint64_t output_bpf = (uint64_t)s->audio_output_channels *
                                sizeof(int16_t);
    const uint64_t target = (uint64_t)s->audio_output_rate * output_bpf *
                            SWITCH_AUDIO_TARGET_MS / 1000u;
    uint64_t buffered = SDL_GetQueuedAudioSize(s->audio_device);
    int pending = SDL_AudioStreamAvailable(s->audio_stream);
    if (pending > 0) buffered += (uint64_t)pending;
    if (buffered >= target) return 0;

    uint64_t deficit = target - buffered;
    uint64_t output_frames = (deficit + output_bpf - 1) / output_bpf;
    uint64_t input_frames = (output_frames * SWITCH_AUDIO_ENGINE_RATE +
                             (uint64_t)s->audio_output_rate - 1) /
                            (uint64_t)s->audio_output_rate;
    return input_frames > 4096u ? 4096 : (int)input_frames;
}

static int switch_audio_drain(switch_state *s)
{
    int16_t converted[4096 * SWITCH_AUDIO_CHANNELS];

    for (;;) {
        int available = SDL_AudioStreamAvailable(s->audio_stream);
        if (available <= 0) return available < 0 ? -1 : 0;

        int bytes = available;
        if (bytes > (int)sizeof converted) bytes = (int)sizeof converted;
        int got = SDL_AudioStreamGet(s->audio_stream, converted, bytes);
        if (got <= 0) {
            printf("audio: SDL_AudioStreamGet failed: %s\n", SDL_GetError());
            return -1;
        }
        if (SDL_QueueAudio(s->audio_device, converted, (Uint32)got) != 0) {
            printf("audio: SDL_QueueAudio failed: %s\n", SDL_GetError());
            return -1;
        }
    }
}

static int switch_audio_push(fa_platform *p, const int16_t *frames, int n)
{
    switch_state *s = (switch_state *)p->impl;
    if (!s || !s->audio_enabled || !frames || n <= 0) return 0;

    int bytes = n * SWITCH_AUDIO_CHANNELS * (int)sizeof(int16_t);
    if (SDL_AudioStreamPut(s->audio_stream, frames, bytes) != 0) {
        printf("audio: SDL_AudioStreamPut failed: %s\n", SDL_GetError());
        return 0;
    }
    if (switch_audio_drain(s) != 0) return 0;
    return n;
}

static int switch_audio_init(switch_state *s)
{
    SDL_AudioSpec want, have;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        printf("audio: SDL_InitSubSystem failed: %s\n", SDL_GetError());
        return -1;
    }
    s->audio_sdl_initialized = 1;

    SDL_zero(want);
    want.freq = SWITCH_AUDIO_DEVICE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = SWITCH_AUDIO_CHANNELS;
    want.samples = 2048;
    want.callback = NULL;

    s->audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have,
                                          SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!s->audio_device) {
        printf("audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        switch_audio_cleanup(s);
        return -1;
    }
    s->audio_initialized = 1;
    s->audio_output_rate = have.freq;
    s->audio_output_channels = have.channels;
    printf("audio: SDL device %d Hz, %d channels, format 0x%x\n",
           have.freq, have.channels, have.format);

    if (have.freq <= 0 || have.channels != SWITCH_AUDIO_CHANNELS ||
        have.format != AUDIO_S16SYS) {
        printf("audio: unsupported SDL device format\n");
        switch_audio_cleanup(s);
        return -1;
    }

    s->audio_stream = SDL_NewAudioStream(AUDIO_S16SYS, SWITCH_AUDIO_CHANNELS,
                                          SWITCH_AUDIO_ENGINE_RATE,
                                          have.format, have.channels,
                                          have.freq);
    if (!s->audio_stream) {
        printf("audio: SDL_NewAudioStream failed: %s\n", SDL_GetError());
        switch_audio_cleanup(s);
        return -1;
    }

    SDL_PauseAudioDevice(s->audio_device, 0);
    s->audio_enabled = 1;
    printf("audio: SDL queue enabled (%d -> %d Hz, %d ms target)\n",
           SWITCH_AUDIO_ENGINE_RATE, have.freq, SWITCH_AUDIO_TARGET_MS);
    return 0;
}

static void switch_audio_cleanup(switch_state *s)
{
    if (!s) return;

    s->audio_enabled = 0;
    if (s->audio_stream) {
        SDL_FreeAudioStream(s->audio_stream);
        s->audio_stream = NULL;
    }
    if (s->audio_initialized) {
        SDL_PauseAudioDevice(s->audio_device, 1);
        SDL_ClearQueuedAudio(s->audio_device);
        SDL_CloseAudioDevice(s->audio_device);
        s->audio_device = 0;
        s->audio_initialized = 0;
    }
    if (s->audio_sdl_initialized) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        s->audio_sdl_initialized = 0;
    }
    s->audio_output_rate = 0;
    s->audio_output_channels = 0;
}

static int switch_pump(fa_platform *p, struct fa_input *in)
{
    switch_state *s = (switch_state *)p->impl;
    if (!appletMainLoop()) return 1;

    padUpdate(&s->pads[0]);
    padUpdate(&s->pads[1]);
    switch_feed_pad(in, 0, &s->pads[0]);
    switch_feed_pad(in, 1, &s->pads[1]);

    /* Plus is the conventional homebrew exit control. */
    if ((padGetButtonsDown(&s->pads[0]) & HidNpadButton_Plus) ||
        (padGetButtonsDown(&s->pads[1]) & HidNpadButton_Plus))
        return 1;
    return 0;
}

static void switch_present(fa_platform *p, const uint16_t *px, int w, int h,
                           size_t pitch)
{
    switch_state *s = (switch_state *)p->impl;
    if (!s || !px) return;

    u32 stride = 0;
    uint16_t *dst = (uint16_t *)framebufferBegin(&s->fb, &stride);
    if (!dst) return;

    int copy_w = w < FA_FB_W ? w : FA_FB_W;
    int copy_h = h < FA_FB_H ? h : FA_FB_H;
    for (int y = 0; y < copy_h; y++) {
        memcpy((uint8_t *)dst + (size_t)y * stride,
               (const uint8_t *)px + (size_t)y * pitch,
               (size_t)copy_w * sizeof(uint16_t));
    }
    framebufferEnd(&s->fb);
}

static uint64_t switch_now_ns(fa_platform *p)
{
    (void)p;
    return (uint64_t)armTicksToNs(armGetSystemTick());
}

static void switch_wait(fa_platform *p, uint64_t ns)
{
    (void)p;
    if (ns > 0) svcSleepThread((s64)ns);
}

static void switch_shutdown(fa_platform *p)
{
    switch_state *s;
    if (!p || !p->impl) return;
    s = (switch_state *)p->impl;
    switch_audio_cleanup(s);
    appletUnhook(&s->applet_cookie);
    framebufferClose(&s->fb);
    free(s);
    p->impl = NULL;
}

static const fa_platform_vtbl SWITCH_VT = {
    switch_pump,
    switch_present,
    switch_audio_want,
    switch_audio_push,
    switch_now_ns,
    switch_wait,
    switch_shutdown
};

int fa_backend_switch_create(fa_platform *p, const fa_platform_cfg *cfg)
{
    (void)cfg;
    if (!p) return -1;

    switch_state *s = (switch_state *)calloc(1, sizeof *s);
    if (!s) return -1;

    padConfigureInput(FA_INPUT_MAX_PADS, HidNpadStyleSet_NpadStandard);
    padInitialize(&s->pads[0], HidNpadIdType_No1, HidNpadIdType_Handheld);
    padInitialize(&s->pads[1], HidNpadIdType_No2);

    s->operation_mode = appletGetOperationMode();
    appletHook(&s->applet_cookie, switch_applet_hook, s);

    Result rc = framebufferCreate(&s->fb, nwindowGetDefault(),
                                  FA_FB_W, FA_FB_H,
                                  PIXEL_FORMAT_RGB_565, 2);
    if (R_FAILED(rc)) goto fail;

    rc = framebufferMakeLinear(&s->fb);
    if (R_FAILED(rc)) goto fail;

    /* Audio is optional for boot: if the service is unavailable, keep the
     * working video/menu path alive and report the reason over nxlink stdout. */
    switch_audio_init(s);

    memset(p, 0, sizeof *p);
    p->vt = &SWITCH_VT;
    p->impl = s;
    p->width = FA_FB_W;
    p->height = FA_FB_H;
    p->audio_rate = SWITCH_AUDIO_ENGINE_RATE;
    p->audio_channels = SWITCH_AUDIO_CHANNELS;
    p->name = "switch";
    return 0;

fail:
    switch_audio_cleanup(s);
    appletUnhook(&s->applet_cookie);
    framebufferClose(&s->fb);
    free(s);
    return -1;
}
