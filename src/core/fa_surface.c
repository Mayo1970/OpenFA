/*
 * fa_surface.c - RGB565 software surface and blitter (RRR-35)
 * See include/fa/fa_surface.h for the model and the RRR-13 / RRR-18 basis.
 */
#include "fa/fa_surface.h"

#include <stdlib.h>
#include <string.h>

/* --- surface lifetime ------------------------------------------------- */

int fa_surface_alloc(fa_surface *s, int w, int h, size_t align)
{
    size_t pitch, bytes;

    if (!s) return -1;
    memset(s, 0, sizeof(*s));
    if (w <= 0 || h <= 0) return -1;

    if (align == 0) align = FA_SURFACE_ALIGN;
    /* align must be a non-zero power of two */
    if (align & (align - 1)) return -1;

    /* guard the width*2 and pitch*height multiplies against overflow */
    if ((size_t)w > (SIZE_MAX / 2u)) return -1;
    pitch = (size_t)w * 2u;
    pitch = (pitch + (align - 1)) & ~(align - 1);
    if (pitch < (size_t)w * 2u) return -1;             /* rounding overflowed */
    if ((size_t)h > (SIZE_MAX / pitch)) return -1;
    bytes = pitch * (size_t)h;

    s->px = (uint16_t *)calloc(1, bytes);
    if (!s->px) return -1;
    s->w = w;
    s->h = h;
    s->pitch = pitch;
    s->owns_px = 1;
    return 0;
}

int fa_surface_wrap(fa_surface *s, uint16_t *px, int w, int h, size_t pitch)
{
    if (!s) return -1;
    memset(s, 0, sizeof(*s));
    if (!px || w <= 0 || h <= 0) return -1;
    if (pitch == 0) pitch = (size_t)w * 2u;
    if (pitch < (size_t)w * 2u) return -1;

    s->px = px;
    s->w = w;
    s->h = h;
    s->pitch = pitch;
    s->owns_px = 0;
    return 0;
}

void fa_surface_free(fa_surface *s)
{
    if (!s) return;
    if (s->owns_px) free(s->px);
    memset(s, 0, sizeof(*s));
}

/* --- little-endian byte boundary ----------------------------------- */

void fa_rgb565_load_le(uint16_t *dst, const void *src_le, size_t n)
{
    const uint8_t *b = (const uint8_t *)src_le;
    size_t i;
    for (i = 0; i < n; i++)
        dst[i] = (uint16_t)((unsigned)b[2 * i] | ((unsigned)b[2 * i + 1] << 8));
}

void fa_rgb565_store_le(void *dst_le, const uint16_t *src, size_t n)
{
    uint8_t *b = (uint8_t *)dst_le;
    size_t i;
    for (i = 0; i < n; i++) {
        b[2 * i]     = (uint8_t)(src[i] & 0xFFu);
        b[2 * i + 1] = (uint8_t)(src[i] >> 8);
    }
}

int fa_surface_load_le(const fa_surface *dst, const void *src_le,
                       int src_w, int src_h)
{
    const uint8_t *row = (const uint8_t *)src_le;
    int y;

    if (!dst || !dst->px || !src_le) return -1;
    if (src_w <= 0 || src_h <= 0) return -1;
    if (src_w > dst->w || src_h > dst->h) return -1;

    for (y = 0; y < src_h; y++) {
        fa_rgb565_load_le(fa_surface_row(dst, y), row, (size_t)src_w);
        row += (size_t)src_w * 2u;
    }
    return 0;
}

/* --- clipping ------------------------------------------------------- */

/*
 * Resolve a blit / fill to a clamped destination window and, for a blit, the
 * matching source origin. Returns 1 if anything remains to draw, 0 if fully
 * clipped away. All of dx0..dy1 and sx0/sy0 are valid, in-bounds coordinates
 * on return of 1.
 */
static int clip_blit(const fa_surface *dst, int dx, int dy,
                     const fa_surface *src, const fa_rect *src_rect,
                     const fa_rect *clip,
                     int *dx0, int *dy0, int *dx1, int *dy1,
                     int *sx0, int *sy0)
{
    int sx = 0, sy = 0, sw = src->w, sh = src->h;
    int cx0 = 0, cy0 = 0, cx1 = dst->w, cy1 = dst->h;
    int ax0, ay0, ax1, ay1;

    if (src_rect) {
        sx = src_rect->x; sy = src_rect->y;
        sw = src_rect->w; sh = src_rect->h;
    }
    /*
     * Clamp the source rect to the source surface. The rect's top-left maps
     * to the destination point (dx, dy); trimming a negative origin therefore
     * shifts the destination point by the same amount, so the pixel that was
     * at (sx, sy) still lands at (dx, dy).
     */
    if (sx < 0)          { dx -= sx; sw += sx; sx = 0; }
    if (sy < 0)          { dy -= sy; sh += sy; sy = 0; }
    if (sx + sw > src->w) sw = src->w - sx;
    if (sy + sh > src->h) sh = src->h - sy;
    if (sw <= 0 || sh <= 0) return 0;

    /* intersect the clip rect with the destination surface */
    if (clip) {
        cx0 = clip->x; cy0 = clip->y;
        cx1 = clip->x + clip->w; cy1 = clip->y + clip->h;
        if (cx0 < 0) cx0 = 0;
        if (cy0 < 0) cy0 = 0;
        if (cx1 > dst->w) cx1 = dst->w;
        if (cy1 > dst->h) cy1 = dst->h;
    }
    if (cx0 >= cx1 || cy0 >= cy1) return 0;

    /* the nominal destination window, then clamp it to the clip window */
    ax0 = dx;       ay0 = dy;
    ax1 = dx + sw;  ay1 = dy + sh;
    if (ax0 < cx0) ax0 = cx0;
    if (ay0 < cy0) ay0 = cy0;
    if (ax1 > cx1) ax1 = cx1;
    if (ay1 > cy1) ay1 = cy1;
    if (ax0 >= ax1 || ay0 >= ay1) return 0;

    *dx0 = ax0; *dy0 = ay0; *dx1 = ax1; *dy1 = ay1;
    *sx0 = sx + (ax0 - dx);       /* shift the source origin by the clip */
    *sy0 = sy + (ay0 - dy);
    return 1;
}

/* --- blit / fill --------------------------------------------------- */

long fa_blit(const fa_surface *dst, int dx, int dy,
             const fa_surface *src, const fa_rect *src_rect,
             const fa_rect *clip)
{
    int dx0, dy0, dx1, dy1, sx0, sy0, y;
    long written;

    if (!dst || !dst->px || !src || !src->px) return -1;
    if (!clip_blit(dst, dx, dy, src, src_rect, clip,
                   &dx0, &dy0, &dx1, &dy1, &sx0, &sy0))
        return 0;

    written = (long)(dx1 - dx0) * (dy1 - dy0);
    for (y = 0; y < dy1 - dy0; y++) {
        const uint16_t *sp = fa_surface_row(src, sy0 + y) + sx0;
        uint16_t       *dp = fa_surface_row(dst, dy0 + y) + dx0;
        memcpy(dp, sp, (size_t)(dx1 - dx0) * sizeof(uint16_t));
    }
    return written;
}

long fa_blit_keyed(const fa_surface *dst, int dx, int dy,
                   const fa_surface *src, const fa_rect *src_rect,
                   const fa_rect *clip, uint16_t key)
{
    int dx0, dy0, dx1, dy1, sx0, sy0, x, y, span;
    long written = 0;

    if (!dst || !dst->px || !src || !src->px) return -1;
    if (!clip_blit(dst, dx, dy, src, src_rect, clip,
                   &dx0, &dy0, &dx1, &dy1, &sx0, &sy0))
        return 0;

    span = dx1 - dx0;
    for (y = 0; y < dy1 - dy0; y++) {
        const uint16_t *sp = fa_surface_row(src, sy0 + y) + sx0;
        uint16_t       *dp = fa_surface_row(dst, dy0 + y) + dx0;
        for (x = 0; x < span; x++) {
            uint16_t s = sp[x];
            if (s != key) { dp[x] = s; written++; }
        }
    }
    return written;
}

long fa_fill(const fa_surface *dst, const fa_rect *rect, const fa_rect *clip,
             uint16_t color)
{
    int rx0, ry0, rx1, ry1;
    int cx0 = 0, cy0 = 0, cx1, cy1, x, y;
    long written;

    if (!dst || !dst->px) return -1;
    cx1 = dst->w; cy1 = dst->h;

    rx0 = 0; ry0 = 0; rx1 = dst->w; ry1 = dst->h;
    if (rect) {
        rx0 = rect->x; ry0 = rect->y;
        rx1 = rect->x + rect->w; ry1 = rect->y + rect->h;
    }
    if (clip) {
        cx0 = clip->x; cy0 = clip->y;
        cx1 = clip->x + clip->w; cy1 = clip->y + clip->h;
    }
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > dst->w) cx1 = dst->w;
    if (cy1 > dst->h) cy1 = dst->h;

    if (rx0 < cx0) rx0 = cx0;
    if (ry0 < cy0) ry0 = cy0;
    if (rx1 > cx1) rx1 = cx1;
    if (ry1 > cy1) ry1 = cy1;
    if (rx0 >= rx1 || ry0 >= ry1) return 0;

    written = (long)(rx1 - rx0) * (ry1 - ry0);
    for (y = ry0; y < ry1; y++) {
        uint16_t *dp = fa_surface_row(dst, y);
        for (x = rx0; x < rx1; x++) dp[x] = color;
    }
    return written;
}
