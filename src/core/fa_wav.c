/*
 * fa_wav.c - PCM RIFF/WAVE reader (RRR-46). See fa_wav.h.
 */
#include "fa/fa_wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int fa_wav_open_mem(fa_wav *w, const void *data, size_t len)
{
    if (w) memset(w, 0, sizeof *w);
    if (!w || !data || len < 44) return -1;

    const uint8_t *b = (const uint8_t *)data;
    if (memcmp(b, "RIFF", 4) != 0 || memcmp(b + 8, "WAVE", 4) != 0)
        return -1;

    int have_fmt = 0;
    int fmt_tag = 0, ch = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *pcm = NULL;
    size_t pcm_len = 0;

    /* Walk the chunk list starting after "RIFF????WAVE". */
    size_t off = 12;
    while (off + 8 <= len) {
        const uint8_t *ch_id = b + off;
        uint32_t ch_sz = rd_u32le(b + off + 4);
        size_t body = off + 8;
        if (body + ch_sz > len) ch_sz = (uint32_t)(len - body); /* be lenient */

        if (memcmp(ch_id, "fmt ", 4) == 0 && ch_sz >= 16) {
            fmt_tag = rd_u16le(b + body + 0);
            ch      = rd_u16le(b + body + 2);
            rate    = rd_u32le(b + body + 4);
            bits    = rd_u16le(b + body + 14);
            have_fmt = 1;
        } else if (memcmp(ch_id, "data", 4) == 0) {
            pcm = b + body;
            pcm_len = ch_sz;
        }
        off = body + ch_sz + (ch_sz & 1u);   /* chunks are word-aligned */
    }

    if (!have_fmt || !pcm) return -1;
    /* PCM only. 1 = WAVE_FORMAT_PCM; 0xFFFE = EXTENSIBLE, accepted only when
     * the fields still describe plain integer PCM. */
    if (fmt_tag != 1 && fmt_tag != 0xFFFE) return -1;
    if (ch != 1 && ch != 2) return -1;
    if (bits != 8 && bits != 16) return -1;
    if (rate == 0) return -1;

    size_t frame_bytes = (size_t)ch * (size_t)(bits / 8);
    size_t frames = frame_bytes ? pcm_len / frame_bytes : 0;
    size_t keep = frames * frame_bytes;

    uint8_t *copy = (uint8_t *)malloc(keep ? keep : 1);
    if (!copy) return -1;
    memcpy(copy, pcm, keep);

    w->rate = (int)rate;
    w->channels = ch;
    w->bits = bits;
    w->frames = frames;
    w->cursor = 0;
    w->data = copy;
    w->data_len = keep;
    return 0;
}

int fa_wav_open_file(fa_wav *w, const char *path)
{
    if (w) memset(w, 0, sizeof *w);
    if (!path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return -1; }
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    int rc = (got == (size_t)sz) ? fa_wav_open_mem(w, buf, got) : -1;
    free(buf);
    return rc;
}

size_t fa_wav_read_s16(fa_wav *w, int16_t *dst, size_t frames)
{
    if (!w || !w->data || !dst) return 0;
    size_t avail = fa_wav_remaining(w);
    if (frames > avail) frames = avail;
    if (frames == 0) return 0;

    int ch = w->channels;
    if (w->bits == 16) {
        const uint8_t *src = w->data + w->cursor * (size_t)ch * 2;
        for (size_t i = 0; i < frames * (size_t)ch; i++) {
            uint16_t u = rd_u16le(src + i * 2);
            dst[i] = (int16_t)u;   /* two's complement, host-order */
        }
    } else { /* 8-bit unsigned -> signed 16 */
        const uint8_t *src = w->data + w->cursor * (size_t)ch;
        for (size_t i = 0; i < frames * (size_t)ch; i++)
            dst[i] = (int16_t)(((int)src[i] - 128) << 8);
    }
    w->cursor += frames;
    return frames;
}

void fa_wav_rewind(fa_wav *w) { if (w) w->cursor = 0; }

void fa_wav_close(fa_wav *w)
{
    if (!w) return;
    free(w->data);
    memset(w, 0, sizeof *w);
}
