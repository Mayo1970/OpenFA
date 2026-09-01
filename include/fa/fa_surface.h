/*
 * fa_surface.h - RGB565 software surface and blitter (RRR-35)
 *
 * Parity basis: RRR-13 "DirectDraw surface and pixel-format rules" (PL-029,
 * PL-030) and RRR-18 (PL-056, the colour key). The original composites every
 * frame with DirectDraw `BltFast` on 16 bpp RGB565 surfaces, back to front,
 * with no hardware clipper and no scaling. This module reproduces that on a
 * portable software target.
 *
 *   Pixel format   RGB565, R 0xF800 / G 0x07E0 / B 0x001F, no alpha.
 *   Surface        a pixel plane plus a byte pitch. Sprite pitch is w*2
 *                  ROUNDED UP to an alignment (RRR-13 saw 96 px -> 192,
 *                  122 px -> 248); the blitter never assumes pitch == w*2.
 *   Opaque blit    DDBLTFAST flags 0: a 1:1 copy of a source rect to a
 *                  destination point.
 *   Keyed blit     DDBLTFAST_SRCCOLORKEY: as opaque, but a source pixel whose
 *                  16-bit value EXACTLY equals the key is not written. No
 *                  range, no tolerance, no blend. The shipped key is 0x0000
 *                  (black) on every sprite (RRR-18 PL-056).
 *   Colour fill    DDBLT_COLORFILL: fill a rect with one RGB565 value.
 *   Clipping       the caller passes a clip rect. The original clips the
 *                  rectangles itself before calling BltFast; the port does the
 *                  same inside the blitter and never reads or writes outside
 *                  either surface, even if a caller passes a partly off-screen
 *                  rect or a negative destination point.
 *
 * Pixels live in memory as host-order uint16. The blitter is therefore
 * endianness-agnostic. Byte order only matters when moving pixels across the
 * boundary to or from an external little-endian buffer (a decoded .W01 frame,
 * a file, a present target): use fa_rgb565_load_le / fa_rgb565_store_le, which
 * assemble each pixel from bytes and are correct on any host.
 */
#ifndef FA_SURFACE_H
#define FA_SURFACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RGB565 channel masks (RRR-13 section 2). */
#define FA_R565_MASK   0xF800u
#define FA_G565_MASK   0x07E0u
#define FA_B565_MASK   0x001Fu

/* Default source colour key: black, exact match (RRR-18 PL-056). */
#define FA_COLORKEY    0x0000u

/* Default surface row alignment in bytes (RRR-13: pitch is a multiple of 8). */
#define FA_SURFACE_ALIGN  8u

typedef struct fa_surface {
    uint16_t *px;       /* row 0 of the plane */
    int       w;        /* width  in pixels, > 0 */
    int       h;        /* height in pixels, > 0 */
    size_t    pitch;    /* bytes per row, >= (size_t)w * 2 */
    int       owns_px;  /* fa_surface_free frees px only when this is set */
} fa_surface;

typedef struct fa_rect { int x, y, w, h; } fa_rect;

/* Build an RGB565 value from 8-bit channels (top bits kept). */
static inline uint16_t fa_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/* Expand an RGB565 value to 8:8:8, bit-replicated (RRR-13 section 2). */
static inline void fa_rgb565_to_rgb888(uint16_t p, uint8_t *r, uint8_t *g,
                                       uint8_t *b)
{
    unsigned r5 = (p >> 11) & 0x1Fu, g6 = (p >> 5) & 0x3Fu, b5 = p & 0x1Fu;
    *r = (uint8_t)((r5 << 3) | (r5 >> 2));
    *g = (uint8_t)((g6 << 2) | (g6 >> 4));
    *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

/* --- surface lifetime ------------------------------------------------- */

/*
 * Allocate a zero-filled surface. `align` is the row alignment in bytes; pass
 * 0 for FA_SURFACE_ALIGN. pitch becomes round_up(w * 2, align). Returns 0 on
 * success, -1 on a bad size or out of memory. On failure `s` is zeroed.
 */
int  fa_surface_alloc(fa_surface *s, int w, int h, size_t align);

/*
 * Wrap caller-owned pixel memory. `pitch` is bytes per row and must be at
 * least w*2; pass 0 to mean exactly w*2. owns_px is cleared, so
 * fa_surface_free will not free `px`. Returns 0 on success, -1 on bad args.
 */
int  fa_surface_wrap(fa_surface *s, uint16_t *px, int w, int h, size_t pitch);

/* Free the plane if the surface owns it, then zero the struct. Safe on a
 * zeroed struct and on a wrapped surface. */
void fa_surface_free(fa_surface *s);

/* --- pixel access --------------------------------------------------- */

static inline uint16_t *fa_surface_row(const fa_surface *s, int y)
{
    return (uint16_t *)((uint8_t *)s->px + (size_t)y * s->pitch);
}
static inline uint16_t fa_surface_get(const fa_surface *s, int x, int y)
{
    return fa_surface_row(s, y)[x];
}
static inline void fa_surface_put(const fa_surface *s, int x, int y, uint16_t p)
{
    fa_surface_row(s, y)[x] = p;
}

/* --- little-endian byte boundary ----------------------------------- */

/* Assemble `n` host-order pixels from 2*n little-endian bytes. Correct on
 * any host (reads bytes, shifts; never a raw uint16 alias). */
void fa_rgb565_load_le(uint16_t *dst, const void *src_le, size_t n);

/* Inverse: write `n` pixels as 2*n little-endian bytes. */
void fa_rgb565_store_le(void *dst_le, const uint16_t *src, size_t n);

/* Load a tightly packed w*h little-endian RGB565 frame (for example a decoded
 * .W01 frame, RRR-18) into `dst`, which must be at least src_w x src_h.
 * Copies row by row so the surface pitch is respected. Returns 0 / -1. */
int  fa_surface_load_le(const fa_surface *dst, const void *src_le,
                        int src_w, int src_h);

/* --- blit / fill --------------------------------------------------- */

/*
 * Opaque 1:1 copy: the `src_rect` region of `src` to `dst` at (dx, dy).
 *  src_rect  NULL means the whole source surface. It is first clamped to the
 *            source bounds.
 *  clip      NULL means the whole destination surface. The write is clipped
 *            to `clip` intersected with the destination bounds.
 * Never scaled. Returns the number of pixels written (>= 0), or -1 on a bad
 * argument (NULL surface / plane).
 */
long fa_blit(const fa_surface *dst, int dx, int dy,
             const fa_surface *src, const fa_rect *src_rect,
             const fa_rect *clip);

/*
 * Source colour-key copy: as fa_blit, but a source pixel whose value is
 * exactly `key` is skipped (RRR-13 section 5, DDCKEY_SRCBLT exact match).
 * Returns the number of pixels actually written, or -1 on a bad argument.
 */
long fa_blit_keyed(const fa_surface *dst, int dx, int dy,
                   const fa_surface *src, const fa_rect *src_rect,
                   const fa_rect *clip, uint16_t key);

/*
 * Colour fill (RRR-13 section 6, DDBLT_COLORFILL): set every pixel of `rect`
 * to `color`. rect NULL means the whole surface. The write is clipped to
 * `clip` intersected with the destination bounds. Returns pixels written, or
 * -1 on a bad argument.
 */
long fa_fill(const fa_surface *dst, const fa_rect *rect, const fa_rect *clip,
             uint16_t color);

#ifdef __cplusplus
}
#endif

#endif /* FA_SURFACE_H */
