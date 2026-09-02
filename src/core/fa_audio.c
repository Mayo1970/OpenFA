/*
 * fa_audio.c - software mixer, resampler and event layer (RRR-46).
 * See fa_audio.h. Model: RRR-46/audio-disasm.md (PL-114..120).
 */
#include "fa/fa_audio.h"
#include "fa/fa_wav.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef FA_PI
#define FA_PI 3.14159265358979323846
#endif

/* ================================================================== *
 *  Polyphase integer-ratio upsampler (port of RRR-30 resample.py)
 *  Rates are 11025 / 22050 / 44100 -> 44100, so L is 1, 2 or 4.
 *  Prototype: Hann-windowed sinc, half = 8 lobes, unity DC gain.
 * ================================================================== */

#define RS_HALF   8
#define RS_NTAPS  (2 * RS_HALF + 1)

typedef struct { int valid; float phase[4][RS_NTAPS]; } rs_table;
static rs_table g_rs2, g_rs4;

static double sinc_pi(double z) { return (z == 0.0) ? 1.0 : sin(z) / z; }

static const rs_table *rs_get(int L)
{
    rs_table *t = (L == 2) ? &g_rs2 : &g_rs4;
    if (t->valid) return t;

    int n = 2 * RS_HALF * L + 1;
    int centre = RS_HALF * L;
    double proto[2 * RS_HALF * 4 + 1];
    double sum = 0.0;
    for (int i = 0; i < n; i++) {
        double x = (double)(i - centre);
        double s = sinc_pi(FA_PI * x / L);
        double w = 0.5 - 0.5 * cos(2.0 * FA_PI * i / (n - 1));
        proto[i] = s * w;
        sum += proto[i];
    }
    double g = sum / L;
    for (int i = 0; i < n; i++) proto[i] /= g;

    memset(t->phase, 0, sizeof t->phase);
    for (int p = 0; p < L; p++) {
        int cnt = (n - p + L - 1) / L;
        if (cnt > RS_NTAPS) cnt = RS_NTAPS;
        for (int k = 0; k < cnt; k++) t->phase[p][k] = (float)proto[p + k * L];
    }
    t->valid = 1;
    return t;
}

typedef struct {
    int             L, ch;
    const rs_table *tab;
    float           hist[2][RS_NTAPS];
} resampler;

static int rs_init(resampler *r, int src_rate, int ch)
{
    memset(r, 0, sizeof *r);
    if (src_rate <= 0 || FA_AUDIO_RATE % src_rate != 0) return -1;
    int L = FA_AUDIO_RATE / src_rate;
    if (L != 1 && L != 2 && L != 4) return -1;
    r->L = L;
    r->ch = ch;
    r->tab = (L == 1) ? NULL : rs_get(L);
    return 0;
}

static int16_t clamp16(double v)
{
    if (v > 32767.0) return 32767;
    if (v < -32768.0) return -32768;
    return (int16_t)lround(v);
}

/* One source frame in (`ch` samples) -> L output frames, each STEREO. */
static void rs_push(resampler *r, const int16_t *in, int16_t *out)
{
    if (r->L == 1) {
        out[0] = in[0];
        out[1] = (r->ch == 2) ? in[1] : in[0];
        return;
    }
    for (int c = 0; c < r->ch; c++) {
        for (int k = RS_NTAPS - 1; k > 0; k--) r->hist[c][k] = r->hist[c][k - 1];
        r->hist[c][0] = (float)in[c];
    }
    for (int p = 0; p < r->L; p++) {
        const float *tp = r->tab->phase[p];
        double a0 = 0.0, a1 = 0.0;
        for (int k = 0; k < RS_NTAPS; k++) {
            a0 += (double)tp[k] * r->hist[0][k];
            if (r->ch == 2) a1 += (double)tp[k] * r->hist[1][k];
        }
        int16_t s0 = clamp16(a0);
        out[p * 2 + 0] = s0;
        out[p * 2 + 1] = (r->ch == 2) ? clamp16(a1) : s0;
    }
}

/* ================================================================== *
 *  Clips (fully decoded SFX / voice) and streams (music, voice lanes)
 * ================================================================== */

#define FA_MAX_CLIPS      96
#define STREAM_RING       16384          /* frames, ~0.37 s stereo */

typedef struct {
    char    *path;
    int16_t *pcm;                        /* interleaved stereo, 44100 */
    size_t   frames;
} fa_clip;

typedef struct {
    int       clip;                      /* -1 = idle */
    size_t    pos;
    int       loop;
    int       gain;                      /* /256, per-call */
} fa_sfx_chan;

typedef struct {
    int       active;
    fa_wav    wav;
    resampler rs;
    int       loop;
    int16_t   ring[STREAM_RING * 2];
    size_t    head, tail;
    int       drained;
} fa_stream;

struct fa_audio {
    char       gdata[512];
    int        master;                   /* /256, port-only global scale */
    int        music_gain;               /* /256, from line 21 */
    int        sfx_gain;                 /* /256, from line 22 */

    fa_clip    clips[FA_MAX_CLIPS];
    int        nclips;

    fa_sfx_chan sfx[FA_AUDIO_SFX_LANES];  /* channels 0..15 */
    fa_stream   str[3];                   /* [0]=ch16 music [1]=ch17 [2]=ch18 */

    uint32_t   warned;                   /* bitset of events already warned */
};

/* ---- gdata path with the fa_menu-style case-fold fallback ---------- */

static int wav_open_rel(fa_wav *w, const char *gdata, const char *rel)
{
    char path[768], low[768];
    snprintf(path, sizeof path, "%s/%s", gdata, rel);
    if (fa_wav_open_file(w, path) == 0) return 0;

    size_t n = strlen(rel);
    if (n >= sizeof low) { memset(w, 0, sizeof *w); return -1; }
    for (size_t i = 0; i < n; i++) {
        char c = rel[i];
        low[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    low[n] = 0;
    snprintf(path, sizeof path, "%s/%s", gdata, low);
    return fa_wav_open_file(w, path);
}

/* ================================================================== *
 *  Event table  (RRR-46/audio-disasm.md section 5, PL-118)
 *  channel < 0  -> TENTATIVE / unknown; the event is a warned no-op.
 * ================================================================== */

typedef struct { const char *file; int channel; int loop; } snd_def;

static const snd_def SND_TBL[FA_SND__COUNT] = {
    [FA_SND_NONE]         = { NULL, -1, 0 },
    [FA_SND_MENU_MUSIC]   = { "SDat/Start.wav",             FA_CH_MUSIC, 1 },
    [FA_SND_MUSIC_W1]     = { "SDat/Dschungel.wav",         FA_CH_MUSIC, 1 },
    [FA_SND_MUSIC_W2]     = { "SDat/Eis.wav",               FA_CH_MUSIC, 1 },
    [FA_SND_MUSIC_W3]     = { "SDat/Fabrik.wav",            FA_CH_MUSIC, 1 },
    [FA_SND_MUSIC_W4]     = { "SDat/Phantasie.wav",         FA_CH_MUSIC, 1 },
    [FA_SND_MUSIC_BOSS]   = { "SDat/endgegner.wav",         FA_CH_MUSIC, 1 },
    [FA_SND_SWAP_P2M]     = { "SDat/voices/ita/pi0020.wav", FA_CH_VOICE, 0 },
    [FA_SND_SWAP_M2P]     = { "SDat/voices/ita/ms0013.wav", FA_CH_VOICE, 0 },
    [FA_SND_JUMP_P]       = { "SDat/alsf01.wav",  0, 0 },
    [FA_SND_JUMP_M]       = { "SDat/alsf01.wav",  1, 0 },
    [FA_SND_THROW_P]      = { "SDat/alsf07.wav",  0, 0 },
    [FA_SND_THROW_M]      = { "SDat/alsf07.wav",  1, 0 },
    /* penguin glide/flight: glide-state entry 0x41858F plays sample
     * [0x4dac3e] (preloaded at 0x41204d) on lane 0, once; stopped by
     * 0x422E04(0) on glide exit. The exe's string is "GData\SDat\alsf02.wav",
     * but the owner's playtest against the original picked ALSF02_old.wav as
     * the take that matches - the shipped alsf02.wav (Feb 2002, shorter) is a
     * later regressed rip. Owner decision 2026-08-30. */
    [FA_SND_GLIDE]        = { "SDat/ALSF02_old.wav",  0, 0 },
    /* RRR-50 owner playtest (PL-134): the collectible pickup jingle is
     * alsf09.wav on channel 2 (fcn.0x41153D, fired from the per-frame HUD
     * update when the pickup flag [0x4DAC48] is set) - NOT knusper.wav,
     * which RRR-46 PL-118 tentatively assigned. knusper (0x416D2D) is a
     * separate "eat" cue. */
    [FA_SND_PICKUP]       = { "SDat/alsf09.wav",   2, 0 },
    [FA_SND_PUSH]         = { "SDat/schieben.wav", 6, 0 },
    [FA_SND_ENEMY_DEFEAT] = { "SDat/alsf04.wav",   3, 0 },
    [FA_SND_ENEMY_KNOCK]  = { "SDat/alsf05.wav",   4, 0 },
    /* the enemy throw uses the same alsf07 as the kid's snowball throw
     * (ds:0x4DAC6E, loaded at 0x41208F); per-enemy grunts are a follow-up. */
    [FA_SND_ENEMY_THROW]     = { "SDat/alsf07.wav", 5, 0 },
    [FA_SND_ENEMY_THROW_EGG] = { "SDat/w3sf02.wav", 5, 0 },
    [FA_SND_ENEMY_DIVE]      = { "SDat/papagei.wav", 8, 0 },
    [FA_SND_ENEMY_DIVE_EAGLE]= { "SDat/w2sf01.wav", 8, 0 },
    [FA_SND_ENEMY_DIVE_BEE]  = { "SDat/w1sf03.wav", 8, 0 },
    /* the three positional loops - slot numbers verified against the exe
     * (preload dispatch 0x4121C7.., level-audio setup 0x412643). Each starts
     * muted; fa_slice posloop_update rides the gain by distance. */
    [FA_SND_UFO]             = { "SDat/w3sf11.wav", 14, 1 },  /* flying robot   */
    [FA_SND_AMBIENT_W3]      = { "SDat/w3sf01.wav",  8, 1 },  /* electric floor */
    [FA_SND_BEE_LOOP]        = { "SDat/w4sf03.wav", 14, 1 },  /* bee            */
    [FA_SND_HIT_P]        = { "SDat/voices/ita/pi0005.wav", 0, 0 },
    [FA_SND_HIT_M]        = { "SDat/voices/ita/ms0007.wav", 1, 0 },
    [FA_SND_MENU_HOVER]   = { "SDat/alsf08.wav",   2, 0 },
    [FA_SND_W3_BUTTON]      = { "SDat/w3sf07.wav",             3, 0 },
    [FA_SND_W3_PIPE]        = { "SDat/w3sf08.wav",             4, 0 },
    [FA_SND_W3_BOSS_HIT]    = { "SDat/w3sf09.wav",             4, 0 },
    [FA_SND_W3_BOSS_SHOT]   = { "SDat/w3sf03.wav",             6, 0 },
    [FA_SND_W3_BOSS_CHARGE] = { "SDat/W3SF05 2.Alternative.wav",9, 0 },
    [FA_SND_W3_BOSS_DEFEAT] = { "SDat/w3sf04a.wav",            9, 0 },
    [FA_SND_W3_BOSS_KO]     = { "SDat/w3sf04b.wav",           10, 0 },
    [FA_SND_W4_BOSS_DRINK]  = { "SDat/w4sf01.wav",             9, 0 },
    [FA_SND_W4_BOSS_SHOT]   = { "SDat/w4sf02.wav",             9, 0 },
    /* RRR-60: yeti boss. Preload 0x4122C8: w2sf05 handle -> ds:0x4DAC54
     * (hurt grunt, exe ch 10), w2sf04 handle -> ds:0x4E0AA4 (landing thud,
     * exe ch 9). */
    [FA_SND_W2_BOSS_LAND]   = { "SDat/w2sf04.wav",             9, 0 },
    [FA_SND_W2_BOSS_HURT]   = { "SDat/w2sf05.wav",            10, 0 },
};

/* ================================================================== *
 *  Lifetime + volume
 * ================================================================== */

fa_audio *fa_audio_create(const char *gdata_dir)
{
    fa_audio *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    if (gdata_dir) {
        strncpy(a->gdata, gdata_dir, sizeof a->gdata - 1);
        a->gdata[sizeof a->gdata - 1] = 0;
    }
    a->master = 256;
    a->music_gain = 256;
    a->sfx_gain = 256;
    for (int i = 0; i < FA_AUDIO_SFX_LANES; i++) a->sfx[i].clip = -1;
    return a;
}

static void stream_stop(fa_stream *s)
{
    if (s->active) fa_wav_close(&s->wav);
    memset(s, 0, sizeof *s);
}

void fa_audio_destroy(fa_audio *a)
{
    if (!a) return;
    for (int i = 0; i < a->nclips; i++) { free(a->clips[i].path); free(a->clips[i].pcm); }
    for (int i = 0; i < 3; i++) stream_stop(&a->str[i]);
    free(a);
}

void fa_audio_set_master(fa_audio *a, int v)
{
    if (!a) return;
    if (v < 0) v = 0; if (v > 256) v = 256;
    a->master = v;
}

/* 0..100 -> gain/256 on a dB curve: v=100 -> 0 dB, v=0 -> `mindb`. */
static int db_gain_256(int v, double mindb)
{
    if (v <= 0) v = 0; if (v >= 100) return 256;
    double db = mindb * (1.0 - v / 100.0);
    double g = pow(10.0, db / 20.0) * 256.0;
    int gi = (int)(g + 0.5);
    return gi < 0 ? 0 : (gi > 256 ? 256 : gi);
}

void fa_audio_set_music_volume_ini(fa_audio *a, int v0_100)
{
    if (a) a->music_gain = db_gain_256(v0_100, -40.0);
}
void fa_audio_set_sfx_volume_ini(fa_audio *a, int v0_100)
{
    if (a) a->sfx_gain = db_gain_256(v0_100, -35.0);
}

/* ================================================================== *
 *  Clip loading
 * ================================================================== */

int fa_audio_load(fa_audio *a, const char *rel_path)
{
    if (!a || !rel_path) return -1;
    for (int i = 0; i < a->nclips; i++)
        if (a->clips[i].path && strcmp(a->clips[i].path, rel_path) == 0)
            return i;
    if (a->nclips >= FA_MAX_CLIPS) return -1;

    fa_wav w;
    if (wav_open_rel(&w, a->gdata, rel_path) != 0) return -1;

    resampler r;
    if (rs_init(&r, w.rate, w.channels) != 0) { fa_wav_close(&w); return -1; }

    size_t out_frames = w.frames * (size_t)r.L;
    int16_t *pcm = malloc((out_frames ? out_frames : 1) * 2 * sizeof *pcm);
    if (!pcm) { fa_wav_close(&w); return -1; }

    enum { CHUNK = 1024 };
    int16_t src[CHUNK * 2], frag[4 * 2];
    size_t wp = 0;
    for (;;) {
        size_t got = fa_wav_read_s16(&w, src, CHUNK);
        if (got == 0) break;
        for (size_t f = 0; f < got; f++) {
            rs_push(&r, &src[f * (size_t)w.channels], frag);
            for (int p = 0; p < r.L; p++) {
                pcm[wp * 2 + 0] = frag[p * 2 + 0];
                pcm[wp * 2 + 1] = frag[p * 2 + 1];
                wp++;
            }
        }
    }
    fa_wav_close(&w);

    fa_clip *c = &a->clips[a->nclips];
    c->path = malloc(strlen(rel_path) + 1);
    if (!c->path) { free(pcm); return -1; }
    strcpy(c->path, rel_path);
    c->pcm = pcm;
    c->frames = wp;
    return a->nclips++;
}

/* ================================================================== *
 *  Playback
 * ================================================================== */

int fa_audio_play_clip(fa_audio *a, int clip_id, int channel, int loop, int gain)
{
    if (!a || channel < 0 || channel >= FA_AUDIO_SFX_LANES) return -1;
    if (clip_id < 0 || clip_id >= a->nclips || a->clips[clip_id].frames == 0)
        return -1;
    fa_sfx_chan *ch = &a->sfx[channel];          /* replace whatever is here */
    ch->clip = clip_id;
    ch->pos = 0;
    ch->loop = loop ? 1 : 0;
    ch->gain = (gain <= 0) ? 256 : (gain > 256 ? 256 : gain);
    return channel;
}

static void stream_fill(fa_stream *s);

int fa_audio_play_stream(fa_audio *a, const char *rel_path, int channel, int loop)
{
    if (!a || channel < FA_CH_MUSIC || channel > FA_CH_BOSS) return -1;
    fa_stream *s = &a->str[channel - FA_CH_MUSIC];
    stream_stop(s);
    if (wav_open_rel(&s->wav, a->gdata, rel_path) != 0) { memset(s, 0, sizeof *s); return -1; }
    if (rs_init(&s->rs, s->wav.rate, s->wav.channels) != 0) {
        fa_wav_close(&s->wav);
        memset(s, 0, sizeof *s);
        return -1;
    }
    s->active = 1;
    s->loop = loop ? 1 : 0;
    stream_fill(s);
    return channel;
}

int fa_audio_event(fa_audio *a, fa_snd_event ev)
{
    if (!a || ev <= FA_SND_NONE || ev >= FA_SND__COUNT) return -1;
    const snd_def *d = &SND_TBL[ev];
    if (!d->file || d->channel < 0) {
        if (!(a->warned & (1u << ev))) {
            a->warned |= (1u << ev);
            fprintf(stderr, "fa_audio: event %d unresolved "
                            "(RRR-46/audio-disasm.md)\n", (int)ev);
        }
        return -1;
    }
    if (d->channel >= FA_CH_MUSIC)
        return fa_audio_play_stream(a, d->file, d->channel, d->loop);
    int clip = fa_audio_load(a, d->file);
    if (clip < 0) return -1;
    return fa_audio_play_clip(a, clip, d->channel, d->loop, 256);
}

void fa_audio_stop(fa_audio *a, int channel)
{
    if (!a || channel < 0 || channel >= FA_AUDIO_CHANNELS) return;
    if (channel < FA_AUDIO_SFX_LANES) { a->sfx[channel].clip = -1; return; }
    stream_stop(&a->str[channel - FA_CH_MUSIC]);
}

void fa_audio_stop_sfx(fa_audio *a)
{
    if (!a) return;
    for (int i = 0; i < FA_AUDIO_SFX_LANES; i++) a->sfx[i].clip = -1;
}

/* live per-lane gain (/256), for a distance-modulated loop like the UFO. */
void fa_audio_set_channel_gain(fa_audio *a, int channel, int gain_256)
{
    if (!a || channel < 0 || channel >= FA_AUDIO_SFX_LANES) return;
    a->sfx[channel].gain = gain_256 < 0 ? 0 : (gain_256 > 256 ? 256 : gain_256);
}

void fa_audio_stop_music(fa_audio *a) { if (a) stream_stop(&a->str[0]); }

static size_t ring_count(const fa_stream *s)
{
    return (s->tail + STREAM_RING - s->head) % STREAM_RING;
}

int fa_audio_channel_busy(const fa_audio *a, int channel)
{
    if (!a || channel < 0 || channel >= FA_AUDIO_CHANNELS) return 0;
    if (channel < FA_AUDIO_SFX_LANES) return a->sfx[channel].clip >= 0;
    const fa_stream *s = &a->str[channel - FA_CH_MUSIC];
    return s->active && !(s->drained && ring_count(s) == 0);
}

/* ================================================================== *
 *  Stream refill
 * ================================================================== */

static void stream_fill(fa_stream *s)
{
    if (!s->active) return;
    const size_t cap = STREAM_RING - 1;
    int16_t src[512 * 2], frag[4 * 2];
    for (;;) {
        size_t used = ring_count(s);
        size_t room = (cap > used) ? cap - used : 0;
        size_t want = room / (size_t)s->rs.L;
        if (want < 64) break;
        if (want > 512) want = 512;
        size_t got = fa_wav_read_s16(&s->wav, src, want);
        if (got == 0) {
            if (s->loop) { fa_wav_rewind(&s->wav); continue; }
            s->drained = 1;
            break;
        }
        for (size_t f = 0; f < got; f++) {
            rs_push(&s->rs, &src[f * (size_t)s->wav.channels], frag);
            for (int p = 0; p < s->rs.L; p++) {
                s->ring[s->tail * 2 + 0] = frag[p * 2 + 0];
                s->ring[s->tail * 2 + 1] = frag[p * 2 + 1];
                s->tail = (s->tail + 1) % STREAM_RING;
            }
        }
    }
}

/* ================================================================== *
 *  Mix
 * ================================================================== */

int fa_audio_mix(fa_audio *a, int16_t *out, int frames)
{
    if (!a || !out || frames <= 0) return 0;

    for (int i = 0; i < 3; i++) stream_fill(&a->str[i]);

    enum { BLK = 512 };
    int done = 0;
    while (done < frames) {
        int n = frames - done;
        if (n > BLK) n = BLK;
        int32_t acc[BLK * 2];
        memset(acc, 0, (size_t)n * 2 * sizeof(int32_t));

        /* streams: music (str[0]) uses music_gain, voice lanes use sfx_gain */
        for (int si = 0; si < 3; si++) {
            fa_stream *s = &a->str[si];
            if (!s->active) continue;
            int g = (si == 0) ? a->music_gain : a->sfx_gain;
            for (int i = 0; i < n; i++) {
                if (ring_count(s) == 0) break;
                acc[i * 2 + 0] += s->ring[s->head * 2 + 0] * g >> 8;
                acc[i * 2 + 1] += s->ring[s->head * 2 + 1] * g >> 8;
                s->head = (s->head + 1) % STREAM_RING;
            }
            if (s->drained && ring_count(s) == 0) stream_stop(s);
        }

        /* SFX lanes */
        for (int li = 0; li < FA_AUDIO_SFX_LANES; li++) {
            fa_sfx_chan *ch = &a->sfx[li];
            if (ch->clip < 0) continue;
            const fa_clip *c = &a->clips[ch->clip];
            int g = ch->gain * a->sfx_gain >> 8;
            for (int i = 0; i < n; i++) {
                if (ch->pos >= c->frames) {
                    if (ch->loop && c->frames) ch->pos = 0;
                    else { ch->clip = -1; break; }
                }
                acc[i * 2 + 0] += c->pcm[ch->pos * 2 + 0] * g >> 8;
                acc[i * 2 + 1] += c->pcm[ch->pos * 2 + 1] * g >> 8;
                ch->pos++;
            }
        }

        int mv = a->master;
        for (int i = 0; i < n * 2; i++) {
            int32_t s = acc[i] * mv >> 8;
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            out[done * 2 + i] = (int16_t)s;
        }
        done += n;
    }
    return frames;
}
