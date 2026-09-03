/*
 * fa_wav.h - PCM RIFF/WAVE reader
 *
 * Every shipped clip is uncompressed PCM RIFF/WAVE in one of five layouts:
 *
 *     rate    ch  bits  count
 *     22050    1   16    159
 *     44100    2   16     34
 *     22050    1    8     15
 *     22050    2   16      1
 *     11025    1    8      1
 *
 * The original hands each source WAVEFORMATEX straight to DirectSound
 * (CreateSoundBuffer), which resamples in hardware/software at mix time. The
 * port has no DirectSound on its targets, so it must decode and rate-convert
 * itself - this reader is step one: it parses the container and yields signed
 * 16-bit samples at the SOURCE rate and channel count. fa_audio.c does the
 * rate conversion and the mix.
 *
 * 8-bit PCM in WAV is UNSIGNED (midpoint 128); this reader expands it to
 * signed 16-bit as (s - 128) << 8. 16-bit PCM is little-endian signed and is
 * assembled byte-wise, so the reader is correct on a big-endian host too.
 *
 * The reader COPIES the data chunk into its own buffer at open time, so the
 * caller's file buffer can be freed immediately. A 15 MB music track is one
 * 15 MB copy - fa_audio streams from it rather than decoding the whole clip
 * to 44100/stereo (which would be ~8x larger).
 */
#ifndef FA_WAV_H
#define FA_WAV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fa_wav {
    int       rate;         /* source sample rate, Hz          */
    int       channels;     /* 1 or 2                          */
    int       bits;         /* 8 or 16 (source container bits) */
    size_t    frames;       /* total sample frames            */
    size_t    cursor;       /* next frame to read             */
    uint8_t  *data;         /* owned copy of the raw data chunk */
    size_t    data_len;     /* bytes in `data`                */
} fa_wav;

/*
 * Parse a WAV held in memory. `data`/`len` are not retained (the data chunk is
 * copied). Returns 0 on success, -1 on a bad or unsupported file (non-PCM,
 * bits not 8/16, channels not 1/2, missing fmt/data). On failure `w` is zeroed.
 */
int fa_wav_open_mem(fa_wav *w, const void *data, size_t len);

/* Read the whole file, then fa_wav_open_mem. Returns 0 or -1. */
int fa_wav_open_file(fa_wav *w, const char *path);

/*
 * Read up to `frames` sample frames from the cursor into `dst` as interleaved
 * signed 16-bit at the SOURCE channel count (dst needs frames*channels
 * int16). Advances the cursor. Returns the number of frames written
 * (0 at end of clip).
 */
size_t fa_wav_read_s16(fa_wav *w, int16_t *dst, size_t frames);

/* Frames left before the cursor hits the end. */
static inline size_t fa_wav_remaining(const fa_wav *w)
{
    return w->cursor < w->frames ? w->frames - w->cursor : 0;
}

/* Move the cursor back to the start. */
void fa_wav_rewind(fa_wav *w);

/* Free the owned buffer and zero the struct. Safe on a zeroed struct. */
void fa_wav_close(fa_wav *w);

#ifdef __cplusplus
}
#endif

#endif /* FA_WAV_H */
