/*
 * fa_w01.c - .W01 container + frame decode (RRR-42)
 * Ports RRR-17/w01_header.c and RRR-18/w01_pixels.c into the engine core.
 */
#include "fa/fa_w01.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* --- pixel decode (RRR-18) ---------------------------------------- */

static int dec_store(const uint8_t *in, size_t avail, uint16_t *out, size_t px)
{
    if (avail < px * 2) return -1;
    for (size_t i = 0; i < px; i++) out[i] = rd16(in + i * 2);
    return 0;
}

static int dec_rle8(const uint8_t *in, size_t avail, uint16_t *out, size_t px)
{
    size_t out_bytes = px * 2, o = 0, ip = 0;
    uint8_t *ob = (uint8_t *)out;
    while (o < out_bytes) {
        if (ip + 2 > avail) return -1;
        uint16_t count = (uint16_t)(int8_t)in[ip];
        uint8_t  value = in[ip + 1];
        ip += 2;
        for (uint16_t k = 0; k < count && o < out_bytes; k++) ob[o++] = value;
    }
    for (size_t i = 0; i < px; i++) out[i] = rd16((uint8_t *)out + i * 2);
    return 0;
}

static int dec_rle16(const uint8_t *in, size_t avail, uint16_t *out, size_t px)
{
    size_t op = 0, ip = 0;
    while (op < px) {
        if (ip + 2 > avail) return -1;
        int16_t c = (int16_t)rd16(in + ip);
        ip += 2;
        if (c >= 0) {
            if (ip + 2 > avail) return -1;
            uint16_t v = rd16(in + ip);
            ip += 2;
            for (int16_t k = 0; k < c && op < px; k++) out[op++] = v;
        } else {
            int32_t n = -(int32_t)c;
            for (int32_t k = 0; k < n && op < px; k++) {
                if (ip + 2 > avail) return -1;
                out[op++] = rd16(in + ip);
                ip += 2;
            }
        }
    }
    return 0;
}

/* --- open / close ------------------------------------------------- */

static int parse(fa_w01 *o)
{
    if (o->len < FA_W01_HEADER_SIZE) return -1;
    const uint8_t *b = o->buf;

    if (rd32(b) != FA_W01_MAGIC) return -1;
    if (rd32(b + 0x103) != FA_W01_FAMILY_SPRITE) return -1;
    o->sig_ok = memcmp(b + 0x004, FA_W01_SIGNATURE,
                       sizeof(FA_W01_SIGNATURE) - 1) == 0;
    o->bpp = rd16(b + 0x115);

    int n = (int)rd16(b + 0x117);
    if (n <= 0 || n > 100000) return -1;
    o->frame_count = n;
    o->frames = (fa_w01_frame *)calloc((size_t)n, sizeof *o->frames);
    if (!o->frames) return -1;

    uint32_t off = rd32(b + 0x41D);
    for (int i = 0; i < n; i++) {
        if ((size_t)off + FA_W01_REC_SIZE > o->len) return -1;
        const uint8_t *r = b + off;
        fa_w01_frame *f = &o->frames[i];
        f->comp    = rd16(r + 0x00);
        f->w       = (int)rd16(r + 0x02);
        f->h       = (int)rd16(r + 0x04);
        uint32_t next = rd32(r + 0x06);
        f->pix_off = rd32(r + 0x0A);
        for (int k = 0; k < 4; k++) f->rect[k] = rd16(r + 0x0E + k * 2);

        if (f->comp > 2 || f->w <= 0 || f->h <= 0) return -1;
        if ((size_t)f->pix_off >= o->len) return -1;
        off = next;
    }

    /* table A: the fc*u32 that precede the fc*u32 table B at EOF (RRR-18).
     * entry = x | (y << 16), screen pixels (PL-079). */
    if (o->len >= (size_t)n * 8) {
        size_t ta = o->len - (size_t)n * 8;
        for (int i = 0; i < n; i++) {
            uint32_t e = rd32(b + ta + (size_t)i * 4);
            o->frames[i].origin_x = (int)(e & 0xffffu);
            o->frames[i].origin_y = (int)((e >> 16) & 0xffffu);
        }
    }
    return 0;
}

int fa_w01_open(fa_w01 *o, const void *data, size_t len, int copy)
{
    if (!o || !data) return -1;
    memset(o, 0, sizeof *o);

    if (copy) {
        o->buf = (uint8_t *)malloc(len ? len : 1);
        if (!o->buf) return -1;
        memcpy(o->buf, data, len);
        o->owns_buf = 1;
    } else {
        o->buf = (uint8_t *)(uintptr_t)data;
        o->owns_buf = 0;
    }
    o->len = len;

    if (parse(o) != 0) { fa_w01_close(o); return -1; }
    return 0;
}

int fa_w01_open_file(fa_w01 *o, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return -1; }

    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    int rc = fa_w01_open(o, buf, (size_t)n, 0);
    if (rc == 0) { o->owns_buf = 1; }   /* take ownership of buf */
    else free(buf);
    return rc;
}

void fa_w01_close(fa_w01 *o)
{
    if (!o) return;
    if (o->owns_buf) free(o->buf);
    free(o->frames);
    memset(o, 0, sizeof *o);
}

int fa_w01_count(const fa_w01 *o) { return o ? o->frame_count : 0; }

int fa_w01_frame_size(const fa_w01 *o, int i, int *w, int *h)
{
    if (!o || i < 0 || i >= o->frame_count) return -1;
    if (w) *w = o->frames[i].w;
    if (h) *h = o->frames[i].h;
    return 0;
}

int fa_w01_frame_origin(const fa_w01 *o, int i, int *x, int *y)
{
    if (!o || i < 0 || i >= o->frame_count) return -1;
    if (x) *x = o->frames[i].origin_x;
    if (y) *y = o->frames[i].origin_y;
    return 0;
}

int fa_w01_decode(const fa_w01 *o, int i, uint16_t *out)
{
    if (!o || !out || i < 0 || i >= o->frame_count) return -1;
    const fa_w01_frame *f = &o->frames[i];
    size_t px = (size_t)f->w * (size_t)f->h;
    const uint8_t *in = o->buf + f->pix_off;
    size_t avail = o->len - f->pix_off;

    switch (f->comp) {
    case 1:  return dec_rle8(in, avail, out, px);
    case 2:  return dec_rle16(in, avail, out, px);
    default: return dec_store(in, avail, out, px);
    }
}
