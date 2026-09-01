/*
 * fa_w01.h - .W01 sprite / background container + frame decode (RRR-42)
 *
 * A direct port of the reverse-engineered readers:
 *   RRR-17  the 1063-byte header and the 22-byte chained frame directory
 *           (W01_GetFileHeader fcn.0042bb1f, PrivateLoadW01 fcn.0042c6c1)
 *   RRR-18  the per-frame pixel decode: store / RLE8 / RLE16, little-endian
 *           RGB565 (PrivateReadImage fcn.0042c96c). Every shipped frame is
 *           bpp 16, comp 2 (RLE16).
 *
 * The engine layer loads a level's BACKGROUNDPOOL .W01 through this module and
 * hands decoded frames to the RGB565 blitter (fa_surface.h). Zero external
 * dependencies; stdio only, like fa_res / fa_script.
 */
#ifndef FA_W01_H
#define FA_W01_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FA_W01_HEADER_SIZE   0x427u
#define FA_W01_MAGIC         0x115u
#define FA_W01_FAMILY_SPRITE 1u
#define FA_W01_REC_SIZE      0x16u
#define FA_W01_SIGNATURE     "WESTKA Animation/Sprite File V 1.0"

typedef struct fa_w01_frame {
    int      w, h;
    unsigned comp;            /* 0 store / 1 RLE8 / 2 RLE16 */
    uint32_t pix_off;         /* file offset of the compressed block */
    uint16_t rect[4];         /* +0x0E crop/origin rect (RRR-18, editor data) */
    int      origin_x;        /* table-A blit origin, low  u16 (see below)     */
    int      origin_y;        /* table-A blit origin, high u16                 */
} fa_w01_frame;

/*
 * The last frame_count*8 bytes of a .W01 are two u32 tables: table A then
 * table B (RRR-18). Table B holds the frame-record offsets. Table A is read
 * by W01_LoadKorrektur into the per-frame origin the front-end positions
 * sprites with: each u32 = (x | (y << 16)) in screen pixels - confirmed
 * against the menu layout in JR_FERRERO.exe fcn.0040409x (GIUNGLA 37,96;
 * VALLE 8,218; MONTAGNA 12,342; FABBRICA 56,464; CLASSIFICA 523,535;
 * ESCI 700,531). PL-079.
 */

typedef struct fa_w01 {
    uint8_t       *buf;
    size_t         len;
    int            owns_buf;
    int            frame_count;
    uint16_t       bpp;
    int            sig_ok;
    fa_w01_frame  *frames;
} fa_w01;

/*
 * Open from memory. If `copy` is non-zero the bytes are duplicated and owned;
 * otherwise `data` must outlive the fa_w01. Returns 0, or -1 on a short buffer
 * / bad header / malformed frame chain.
 */
int  fa_w01_open(fa_w01 *o, const void *data, size_t len, int copy);

/* Open from a real filesystem path. Returns 0 or -1. */
int  fa_w01_open_file(fa_w01 *o, const char *path);

void fa_w01_close(fa_w01 *o);

int  fa_w01_count(const fa_w01 *o);

/* Frame `i` dimensions. Returns 0 or -1 (out of range). */
int  fa_w01_frame_size(const fa_w01 *o, int i, int *w, int *h);

/* Frame `i` table-A screen origin (x,y). Returns 0 or -1. */
int  fa_w01_frame_origin(const fa_w01 *o, int i, int *x, int *y);

/*
 * Decode frame `i` into `out`, which must hold w*h uint16 host-order RGB565
 * pixels (see fa_w01_frame_size). Returns 0, or -1 on a bad index or a
 * malformed stream.
 */
int  fa_w01_decode(const fa_w01 *o, int i, uint16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FA_W01_H */
