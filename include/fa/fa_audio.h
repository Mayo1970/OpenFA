/*
 * fa_audio.h - software mixer, resampler and event layer (RRR-46)
 *
 * Parity basis: RRR-9 (the DirectSound streaming timer is not the sim clock),
 * RRR-30 (the five source PCM formats), and the audio disasm trace
 * RRR-46/audio-disasm.md (PL-114..120). The original drives a C_DirectSound
 * object with a FIXED channel layout and NO priority system:
 *
 *   - 200 sample SLOTS, load-on-demand, first free slot, no name table
 *     (PL-114/115). The caller owns the name -> slot id mapping.
 *   - 19 mixer channels (PL-116):
 *       0..15  resident SFX lanes - specific sounds go to specific lanes
 *       16     the one music stream, looping
 *       17     player / ambient voice, one-shot
 *       18     boss voice, one-shot
 *   - Playing on a busy channel REPLACES what is there (the original defers
 *     the swap by one timer tick through a single pending slot; the audible
 *     result is a replace). Different channels simply mix. There is no
 *     priority, no stealing, no queue. A player-hit SFX (lanes 0/1) mixes
 *     with a voice line on lane 17 (PL-117).
 *   - The source PCM format is passed straight to DirectSound (PL-120); a
 *     non-DirectSound port must convert the five formats itself. This module
 *     decodes + rate-converts to 44100 / stereo and mixes in software.
 *
 * The music channel is streamed from the source WAV (a 15 MB track would be
 * ~120 MB at 44100/stereo/16 - RRR-38 residency). SFX and voice clips are
 * short and are fully decoded.
 *
 * Timing: fa_audio_mix() is pulled once per RENDERED frame from the fa_app
 * audio callback. It is not a simulation input and does not affect the golden
 * sim hash. Single-threaded; all calls on the main loop thread.
 */
#ifndef FA_AUDIO_H
#define FA_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FA_AUDIO_RATE       44100

/* Channel layout (PL-116). */
#define FA_AUDIO_SFX_LANES  16          /* channels 0..15 */
#define FA_CH_MUSIC         16
#define FA_CH_VOICE         17
#define FA_CH_BOSS          18
#define FA_AUDIO_CHANNELS   19

/*
 * Game audio events. Each maps to {file, channel, loop} in the table in
 * fa_audio.c, lifted from RRR-46/audio-disasm.md section 5. Entries marked
 * TENTATIVE there (glide) or still unknown play nothing (logged once).
 */
typedef enum fa_snd_event {
    FA_SND_NONE = 0,

    /* music - channel 16, looping */
    FA_SND_MENU_MUSIC,     /* Start.wav       */
    FA_SND_MUSIC_W1,       /* Dschungel.wav   */
    FA_SND_MUSIC_W2,       /* Eis.wav         */
    FA_SND_MUSIC_W3,       /* Fabrik.wav      */
    FA_SND_MUSIC_W4,       /* Phantasie.wav   */
    FA_SND_MUSIC_BOSS,     /* endgegner.wav   */

    /* voice - channel 17, one-shot */
    FA_SND_SWAP_P2M,       /* pi0020.wav - penguin -> Fettalatte */
    FA_SND_SWAP_M2P,       /* ms0013.wav - Fettalatte -> penguin */

    /* SFX - fixed lanes */
    FA_SND_JUMP_P,         /* alsf01.wav lane 0 */
    FA_SND_JUMP_M,         /* alsf01.wav lane 1 */
    FA_SND_THROW_P,        /* alsf07.wav lane 0 */
    FA_SND_THROW_M,        /* alsf07.wav lane 1 */
    FA_SND_GLIDE,          /* alsf02.wav lane 0 - penguin flight, owner-confirmed */
    FA_SND_PICKUP,         /* alsf09.wav lane 2 - collectible (PL-134) */
    FA_SND_PUSH,           /* schieben.wav lane 6 */
    FA_SND_ENEMY_DEFEAT,   /* alsf04.wav lane 3 */
    FA_SND_ENEMY_KNOCK,    /* alsf05.wav lane 4 */
    FA_SND_ENEMY_THROW,    /* alsf07.wav lane 5 - kong/yeti/snowman/bear throw (ds:0x4DAC6E) */
    FA_SND_ENEMY_THROW_EGG,/* w3sf02.wav lane 5 - egg robot throw (ds:0x4DAC60, 0x41037F) */
    FA_SND_ENEMY_DIVE,     /* papagei.wav lane 8 - parrot dive (ds:0x4E0AA8, 0x41622C) */
    FA_SND_ENEMY_DIVE_EAGLE,/* w2sf01.wav lane 8 - eagle dive (ds:0x4E0A90, 0x40A931) */
    FA_SND_ENEMY_DIVE_BEE, /* w1sf03.wav lane 8 - bee (ds:0x4DAC3C, 0x40C30A) */

    /* The exe's three POSITIONAL loops. Each is started muted at world load
     * (0x412643) on a fixed slot and its volume is ridden per frame by the
     * distance from screen centre to the nearest emitter (emitter routine
     * 0x412EE0, volume pass 0x41139D). fa_slice posloop_update reproduces the
     * curve; the slot numbers here match the exe.
     *   w3sf11 slot 14  flying robot   (ObjNr 13, handler 0x4106C0)
     *   w3sf01 slot  8  electric floor (ObjNr 355/356, handler 0x415F10)
     *   w4sf03 slot 14  bee            (ObjNr 16, handler 0x40BBA0) */
    FA_SND_UFO,            /* w3sf11.wav lane 14 loop - flying robot   */
    FA_SND_AMBIENT_W3,     /* w3sf01.wav lane  8 loop - electric floor */
    FA_SND_BEE_LOOP,      /* w4sf03.wav lane 14 loop - bee            */
    FA_SND_HIT_P,          /* pi0005.wav lane 0 - also the fatal hit */
    FA_SND_HIT_M,          /* ms0007.wav lane 1 - also the fatal hit */
    FA_SND_MENU_HOVER,     /* alsf08.wav - world / option hover */
    /* RRR-61: the World-3 (FABBRICA) robot-boss cues (preload 0x4122F9..,
     * owner-confirmed in playtest). */
    FA_SND_W3_BUTTON,      /* w3sf07.wav lane 3 - a button is pushed         */
    FA_SND_W3_PIPE,        /* w3sf08.wav lane 4 - the pipe drops             */
    FA_SND_W3_BOSS_HIT,    /* w3sf09.wav lane 4 - the pipe hits the robot    */
    FA_SND_W3_BOSS_SHOT,   /* w3sf03.wav lane 6 - the robot fires a bolt     */
    FA_SND_W3_BOSS_CHARGE, /* "W3SF05 2.Alternative.wav" lane 9 - wind-up    */
    FA_SND_W3_BOSS_DEFEAT, /* w3sf04a.wav lane 9 - the robot is defeated     */
    FA_SND_W3_BOSS_KO,     /* w3sf04b.wav lane 10 - RBKO frame 56 (exe ch10) */

    /* RRR-62: the World-4 (VALLE) octopus/KRAKE boss cues (preload
     * 0x4123C1.., 0x40CD70 handler). Both on lane 9, one-shot. */
    FA_SND_W4_BOSS_DRINK,  /* w4sf01.wav lane 9 - the octopus drinks milk (vulnerable) */
    FA_SND_W4_BOSS_SHOT,   /* w4sf02.wav lane 9 - the octopus fires a milk particle    */

    /* RRR-60: the World-2 (MONTAGNA) yeti-boss cues (preload 0x4122C8..,
     * 0x40E350 handler). */
    FA_SND_W2_BOSS_LAND,   /* w2sf04.wav lane 9  - the yeti lands from a hop  */
    FA_SND_W2_BOSS_HURT,   /* w2sf05.wav lane 10 - the yeti takes a hit (fr69) */

    FA_SND__COUNT
} fa_snd_event;

typedef struct fa_audio fa_audio;

/* `gdata_dir` is the GData root (DIRECT GDATA LOADER, RRR-33). Returns NULL
 * on allocation failure. */
fa_audio *fa_audio_create(const char *gdata_dir);
void      fa_audio_destroy(fa_audio *a);

/*
 * Volume. Option.ini line 21 = music (global 0x45ECC0), line 22 = sound
 * (0x45EFE4), both 0..100 (PL-119; corrects RRR-23's swapped labels). The
 * option screen maps 0..100 onto roughly -40..0 dB (music) / -35..0 dB
 * (sound); `*_ini` reproduces that curve. `set_master` is an extra global
 * scale for the port (a --vol flag / mute), not an original control.
 */
void fa_audio_set_master(fa_audio *a, int vol_256);
void fa_audio_set_music_volume_ini(fa_audio *a, int v0_100);
void fa_audio_set_sfx_volume_ini(fa_audio *a, int v0_100);

/*
 * Fire a game event. Returns the channel it plays on (0..18), or -1 if the
 * event is out of range, TENTATIVE/unknown, or the file is missing.
 */
int  fa_audio_event(fa_audio *a, fa_snd_event ev);

/* Stop one channel, all SFX lanes (0..15), or the music channel. */
void fa_audio_stop(fa_audio *a, int channel);
void fa_audio_stop_sfx(fa_audio *a);
void fa_audio_stop_music(fa_audio *a);
/* Live gain (/256) on an SFX lane - for a distance-modulated loop. */
void fa_audio_set_channel_gain(fa_audio *a, int channel, int gain_256);

/*
 * 1 when `channel` is producing sound (or has a start pending). This is the
 * fa_audio_channel_busy / exe 0x4231E5 test the character-swap and boss state
 * machines poll (PL-117).
 */
int  fa_audio_channel_busy(const fa_audio *a, int channel);

/*
 * Produce `frames` interleaved stereo S16 frames into `out` (frames*2 int16).
 * Always fills the whole buffer (silence when idle). Returns `frames`. Call
 * once per rendered frame.
 */
int  fa_audio_mix(fa_audio *a, int16_t *out, int frames);

/* --- lower level, for tests and non-event playback ------------------- */

/* Load + decode + resample a clip to 44100 / stereo. Returns a cached clip id
 * (>= 0), or -1. Re-loading the same path returns the cached id. */
int  fa_audio_load(fa_audio *a, const char *rel_path);

/* Play a loaded clip on `channel` (0..15), replacing whatever is there.
 * `gain_256` is the per-call gain (256 = unity). Returns `channel` or -1. */
int  fa_audio_play_clip(fa_audio *a, int clip_id, int channel, int loop,
                        int gain_256);

/* Open a streamed file on `channel` (16..18). Returns `channel` or -1. */
int  fa_audio_play_stream(fa_audio *a, const char *rel_path, int channel,
                          int loop);

#ifdef __cplusplus
}
#endif

#endif /* FA_AUDIO_H */
