/*
 * fa_w02.c - .W02 pool container (RRR-42). Ports RRR-19/w02_pool.c.
 */
#include "fa/fa_w02.h"

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

static int parse(fa_w02 *o)
{
    if (o->len < FA_W02_HEADER_SIZE) return -1;
    const uint8_t *b = o->buf;

    if (rd32(b + 0x000) != FA_W02_FORMAT_ID) return -1;
    if (rd32(b + 0x103) != FA_W02_FAMILY_POOL) return -1;
    o->sig_ok = memcmp(b + 0x004, FA_W02_SIGNATURE,
                       sizeof(FA_W02_SIGNATURE) - 1) == 0;
    o->item_kind = rd16(b + 0x115);

    int n = (int)rd16(b + 0x117);
    if (n <= 0 || (size_t)n * 4 + FA_W02_HEADER_SIZE > o->len) return -1;
    o->count = n;

    /* Validate: N records forward from 0x119 must agree with the pointer
     * table and fill the file exactly (RRR-19 w02_walk). */
    const uint8_t *ptab = o->buf + o->len - (size_t)n * 4;
    uint32_t off = FA_W02_HEADER_SIZE;
    for (int i = 0; i < n; i++) {
        if ((size_t)off + FA_W02_REC_SIZE > o->len) return -1;
        uint32_t ds       = rd32(o->buf + off);
        uint32_t data_off = off + FA_W02_REC_SIZE;
        uint32_t entry    = rd32(ptab + (size_t)i * 4);
        if (entry != data_off) return -1;
        if ((size_t)data_off + ds > o->len - (size_t)n * 4) return -1;
        off = data_off + ds;
    }
    if (off != o->len - (size_t)n * 4) return -1;
    return 0;
}

int fa_w02_open(fa_w02 *o, const void *data, size_t len, int copy)
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
    }
    o->len = len;
    if (parse(o) != 0) { fa_w02_close(o); return -1; }
    return 0;
}

int fa_w02_open_file(fa_w02 *o, const char *path)
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

    int rc = fa_w02_open(o, buf, (size_t)n, 0);
    if (rc == 0) o->owns_buf = 1;
    else free(buf);
    return rc;
}

void fa_w02_close(fa_w02 *o)
{
    if (!o) return;
    if (o->owns_buf) free(o->buf);
    memset(o, 0, sizeof *o);
}

int fa_w02_count(const fa_w02 *o) { return o ? o->count : 0; }

int fa_w02_chunk(const fa_w02 *o, int i, const uint8_t **ptr, uint32_t *size,
                 uint16_t *kind)
{
    if (!o || !o->buf || o->count <= 0) return -1;
    if (i < 0) i = 0;
    if (i >= o->count) i = o->count - 1;      /* W02_SelectChunk clamp */

    const uint8_t *ptab = o->buf + o->len - (size_t)o->count * 4;
    uint32_t p = rd32(ptab + (size_t)i * 4);
    if (p < FA_W02_HEADER_SIZE + FA_W02_REC_SIZE || (size_t)p > o->len)
        return -1;

    uint32_t ds = rd32(o->buf + p - FA_W02_REC_SIZE);
    if ((size_t)p + ds > o->len) return -1;

    if (ptr)  *ptr  = o->buf + p;
    if (size) *size = ds;
    if (kind) *kind = rd16(o->buf + p - FA_W02_REC_SIZE + 4);
    return 0;
}
