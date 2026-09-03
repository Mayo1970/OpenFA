/*
 * fa_bmp.h - Windows BMP -> RGB565 surface loader
 *
 * The original brings the front-end background art (GData\Pics\*.bmp:
 * StartBG.bmp, HighscoreBG.bmp, Credit*.bmp) onto a DirectDraw surface once
 * at screen load with BitBlt / StretchDIBits through a memory DC. The port
 * replaces that with a load-time decode to an RGB565 fa_surface.
 *
 * Handles uncompressed (BI_RGB) 24-bit and 32-bit bottom-up or top-down
 * Windows BMPs with a BITMAPINFOHEADER - which is every shipped .bmp
 * (800x600, 24 bpp, bottom-up). 8-bit palettised and BI_RLE are not needed
 * and are rejected.
 */
#ifndef FA_BMP_H
#define FA_BMP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fa_surface;

/* Decode a BMP in memory into a freshly allocated RGB565 surface `out`
 * (caller frees with fa_surface_free). Returns 0, or -1 on a bad / unsupported
 * file. */
int fa_bmp_load_mem(struct fa_surface *out, const void *data, size_t len);

/* Read a file and decode it. Returns 0 or -1. */
int fa_bmp_load_file(struct fa_surface *out, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* FA_BMP_H */
