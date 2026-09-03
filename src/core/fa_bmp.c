/*
 * fa_bmp.c - Windows BMP -> RGB565 surface loader. See fa_bmp.h.
 */
#include "fa/fa_bmp.h"
#include "fa/fa_surface.h"

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
static int32_t rdi32(const uint8_t *p) { return (int32_t)rd32(p); }

int fa_bmp_load_mem(struct fa_surface *out, const void *data, size_t len)
{
    const uint8_t *b = (const uint8_t *)data;
    if (!out || !b || len < 54) return -1;
    if (b[0] != 'B' || b[1] != 'M') return -1;

    uint32_t pix_off  = rd32(b + 10);
    uint32_t hdr_size = rd32(b + 14);
    if (hdr_size < 40) return -1;                   /* BITMAPINFOHEADER+ only */

    int32_t  w   = rdi32(b + 18);
    int32_t  h   = rdi32(b + 22);
    uint16_t bpp = rd16(b + 28);
    uint32_t comp = rd32(b + 30);

    if (w <= 0 || w > 16384 || h == 0 || h < -16384 || h > 16384) return -1;
    if (comp != 0) return -1;                       /* BI_RGB only */
    if (bpp != 24 && bpp != 32) return -1;

    int top_down = h < 0;
    int H = top_down ? -h : h;
    int W = w;
    int bytes_pp = bpp / 8;
    size_t row_stride = ((size_t)W * bytes_pp + 3u) & ~(size_t)3u;
    if ((size_t)pix_off + row_stride * (size_t)H > len) return -1;

    if (fa_surface_alloc(out, W, H, 0) != 0) return -1;

    for (int y = 0; y < H; y++) {
        int src_row = top_down ? y : (H - 1 - y);
        const uint8_t *sp = b + pix_off + (size_t)src_row * row_stride;
        uint16_t *dp = fa_surface_row(out, y);
        for (int x = 0; x < W; x++) {
            uint8_t bl = sp[0], gr = sp[1], re = sp[2];   /* BMP is BGR */
            dp[x] = fa_rgb565(re, gr, bl);
            sp += bytes_pp;
        }
    }
    return 0;
}

int fa_bmp_load_file(struct fa_surface *out, const char *path)
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

    int rc = fa_bmp_load_mem(out, buf, (size_t)n);
    free(buf);
    return rc;
}
