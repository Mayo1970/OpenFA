/*
 * fa_map.h - level (.W02 chunk 0) loader (RRR-42)
 *
 * Reverse-engineered from JR_FERRERO.exe (objdump -d): LoadMap fcn.00432cd0,
 * LoadMapInfo fcn.00432fe0, the render-runtime prep fcn.00432840, the grid
 * render loop fcn.00433110, the entry lookup fcn.004340c0 and the coarse
 * solid query fcn.00434180. Cross-checked against all 12 shipped
 * GData\Maps\*.W02. See RRR-42-report.md and grid-cell-disasm.md for the
 * evidence trail; PL-074..PL-077, PL-090..PL-094.
 *
 * CONFIRMED
 *   - The playable level is always chunk 0 of the MAPPOOL .W02 (LoadMap runs
 *     with a hard-coded index 0 at 0x4118a9). Chunks 1..N-1 are identical
 *     40x30 editor scratch maps.
 *   - Chunk 0 payload = [ 348-byte MapInfo ][ grid_bytes grid ][ tail ].
 *     LoadMapInfo does fread(hdr, 0x15c, 1, f) then reads u32 @ hdr+0x132
 *     (= +306) bytes of grid. The tail is entity data (RRR-50).
 *   - MapInfo little-endian fields:
 *       @0x104 (260)  u16[5]  background .W01 frame ids. ids 0..3 are the
 *                             tile ATLAS frames (each 640x480); id 4 is the
 *                             far backdrop frame (800x600), drawn first.
 *       @0x10E (270)  u16     tile width  = 32
 *       @0x110 (272)  u16     tile height = 32
 *       @0x112 (274)  u16     grid width  in tiles
 *       @0x114 (276)  u16     grid height in tiles
 *       @0x132 (306)  u32     grid byte count = grid_w * grid_h * 24
 *       @0x15B (347)  u8      plane count (8 in every shipped map)
 *
 * GRID LAYOUT (PL-090, confirmed against the render loop and all 12 files)
 *   The grid is PLANE-MAJOR, not an array of 24-byte cells:
 *       grid = plane[8] of (grid_h * grid_w) entries, each entry 3 bytes.
 *       entry(plane, x, y) = grid + plane*(3*grid_w*grid_h) + 3*(y*grid_w + x)
 *   The plane number is the draw / Z order (0 back .. 7 front); the render
 *   loop at 0x4332ec walks plane 0..plane_count-1.
 *
 * ENTRY (3 bytes, PL-091, confirmed at 0x4333d0 / 0x434200)
 *       +0  u8   attr
 *       +1  u16  packed  (little-endian)
 *   packed == 0xFFFF  -> empty. attr alone is NOT authoritative for
 *   emptiness (1716 empty entries in the shipped maps keep a stale attr).
 *       tile = packed & 0x01FF        (9-bit index into the chosen atlas)
 *       code = (packed >> 9) & 0x7F   collision / behaviour code, queried by
 *                                     fcn.00434200; code > 50 -> not drawn
 *   attr bits (PL-092):
 *       0x07  atlas frame select, 0..3 (loader rejects >= 4)
 *       0x08  flip X                 0x10  flip Y   (0x18 = both)
 *       0x20  coarse-solid bit  (the value fcn.00434180 returns)
 *       0x40  suppress: skip drawing AND make the collision queries bail
 *             (no shipped entry sets it)
 *       0x80  HAZARD tile (RRR-53): fcn.0041A290, run every frame from the
 *             player state-machine tail, queries plane 2 at the player origin
 *             - a non-empty cell with 0x80 set deals 20 damage + 120 i-frames
 *             (when not invulnerable), plays the character hit sound, and
 *             bounces the player up (vy = -20.0) every frame. Welt1 y=2336
 *             band, Welt2-4 hazard pools. (Earlier guess: "alt collision".)
 *   There is no attr animation bit; the grid is never written after load.
 *
 * SOURCE RECT (PL-093, confirmed at 0x4333d0..0x43341b)
 *       cols   = 640 / tile_w            (this+0x1733; normally 20)
 *       src_x  = (tile % cols) * tile_w
 *       src_y  = (tile / cols) * tile_h
 *   A tile whose row falls outside the 640x480 atlas (e.g. the marker value
 *   511) is a collision-only / invisible record; the renderer does not draw
 *   it (fa_render skips it; the original relied on the DirectDraw no-op).
 *
 * STILL UNKNOWN (carried; owner-side, needs the RRR-6 oracle)
 *   - the designer names of code bits 0/1 (bit 1 has a top-row-only path in
 *     the pixel query - probably a one-way platform). attr 0x80 = hazard
 *     (RRR-53).
 *   - MapInfo @0x116 u16[9] (nine 0x12-byte render-layer records at runtime),
 *     @0x126, @0x128.
 *   - the per-plane parallax / scroll factor (fa_render scrolls 1:1).
 */
#ifndef FA_MAP_H
#define FA_MAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fa_w02;

#define FA_MAP_INFO_SIZE    348u
#define FA_MAP_CELL_STRIDE   24u    /* logical bytes per (x,y): 8 planes x 3 */
#define FA_MAP_PLANES         8     /* hdr@0x15B; 8 in every shipped map     */
#define FA_MAP_ENTRY_BYTES    3
#define FA_MAP_ENTRY_EMPTY 0xFFFFu  /* packed value of an empty entry        */
#define FA_MAP_BG_LAYERS      5

/* packed-word fields */
#define FA_MAP_TILE_MASK   0x01FFu
#define FA_MAP_CODE_SHIFT       9
#define FA_MAP_CODE_MASK     0x7Fu
#define FA_MAP_CODE_NODRAW     50   /* code > this -> the entry is not drawn  */

/* attr bits */
#define FA_MAP_ATTR_ATLAS   0x07u
#define FA_MAP_ATTR_FLIPX   0x08u
#define FA_MAP_ATTR_FLIPY   0x10u
#define FA_MAP_ATTR_SOLID   0x20u
#define FA_MAP_ATTR_SUPPRESS 0x40u
#define FA_MAP_ATTR_HAZARD  0x80u   /* RRR-53: fcn.0041A290 - hurts the player */

typedef struct fa_map_info {
    int      tile_w, tile_h;       /* 32, 32                              */
    int      grid_w, grid_h;       /* tiles                               */
    uint32_t grid_bytes;           /* = grid_w * grid_h * 24              */
    int      plane_count;          /* @0x15B, 8                            */
    int      atlas_cols;           /* 640 / tile_w (render helper)         */
    int      bg_layer[FA_MAP_BG_LAYERS];   /* @260: [0..3] atlas, [4] backdrop */
    uint16_t u278[9];              /* @0x116, role UNKNOWN                 */
    uint16_t u296;                 /* @0x128, 2 real / 1 scratch, UNKNOWN  */
    uint8_t  raw[FA_MAP_INFO_SIZE];/* the whole 348-byte header verbatim   */
} fa_map_info;

typedef struct fa_map {
    fa_map_info info;
    uint8_t    *grid;              /* grid_bytes, owned, plane-major       */
    uint8_t    *tail;              /* entity data, owned (RRR-50)          */
    uint32_t    tail_size;
    int         world_w, world_h;  /* grid_w*tile_w, grid_h*tile_h (px)    */
} fa_map;

/* Parse a 348-byte MapInfo header from a chunk-0 payload. `chunk_size` must
 * be >= 348. Returns 0, or -1 (short buffer / inconsistent grid_bytes). */
int  fa_map_parse_info(const uint8_t *chunk, uint32_t chunk_size,
                       fa_map_info *out);

/* Load chunk 0 of an open pool into `m`. Returns 0 or -1. */
int  fa_map_load_w02(fa_map *m, const struct fa_w02 *pool);

/* Convenience: open the .W02 at `path` and load its chunk 0. Returns 0/-1. */
int  fa_map_load_file(fa_map *m, const char *w02_path);

void fa_map_free(fa_map *m);

int  fa_map_plane_count(const fa_map *m);   /* info.plane_count, clamped 0..8 */

/* --- entries ----------------------------------------------------------- */

typedef struct fa_map_entry {
    uint8_t  attr;
    uint16_t packed;   /* 0xFFFF = empty */
} fa_map_entry;

/* Entry of plane `plane` (0..7) at tile (cx, cy). An out-of-range plane or
 * cell returns { 0, 0xFFFF } (empty). Plane-major addressing (PL-090). */
fa_map_entry fa_map_cell_entry(const fa_map *m, int cx, int cy, int plane);

/* 1 if any plane has a non-empty entry at (cx, cy). */
int fa_map_cell_occupied(const fa_map *m, int cx, int cy);

/* --- entry decode (PL-091..093) -------------------------------------- */

static inline int fa_map_entry_empty(fa_map_entry e)
{ return e.packed == FA_MAP_ENTRY_EMPTY; }

static inline int fa_map_entry_tile(fa_map_entry e)
{ return (int)(e.packed & FA_MAP_TILE_MASK); }

static inline int fa_map_entry_code(fa_map_entry e)
{ return (int)((e.packed >> FA_MAP_CODE_SHIFT) & FA_MAP_CODE_MASK); }

static inline int fa_map_entry_atlas(fa_map_entry e)
{ return (int)(e.attr & FA_MAP_ATTR_ATLAS); }

static inline int fa_map_entry_flip_x(fa_map_entry e)
{ return (e.attr & FA_MAP_ATTR_FLIPX) != 0; }

static inline int fa_map_entry_flip_y(fa_map_entry e)
{ return (e.attr & FA_MAP_ATTR_FLIPY) != 0; }

static inline int fa_map_entry_solid(fa_map_entry e)
{ return (e.attr & FA_MAP_ATTR_SOLID) != 0; }

static inline int fa_map_entry_drawn(fa_map_entry e)
{
    return !fa_map_entry_empty(e) &&
           !(e.attr & FA_MAP_ATTR_SUPPRESS) &&
           fa_map_entry_code(e) <= FA_MAP_CODE_NODRAW &&
           fa_map_entry_atlas(e) < 4;
}

/* --- collision (fcn.00434180, PL-094) ------------------------------- */

/*
 * The player/physics-facing coarse solid query (fcn.00434180 verbatim). Maps
 * a world pixel to its tile on plane `plane` (the original passes 2 for the
 * terrain plane) and returns:
 *     1   out of the map            (level edges read as solid)
 *    -1   empty, or attr & 0x40     (suppressed)
 *     1   an active entry, attr & 0x20 set
 *     0   an active entry, attr & 0x20 clear
 * so `> 0` means "stand on this". Returns -1 on a bad map.
 */
int fa_map_solid_at(const fa_map *m, int plane, int world_x, int world_y);

/* 1 if the world pixel sits on a ladder tile (plane-2 grid entry, collision
 * code bit 0 set - PL-102). Used by the climb state. */
int fa_map_ladder_at(const fa_map *m, int world_x, int world_y);

/*
 * Solidity class of the terrain (plane 2) at a world pixel, for RRR-44
 * (fa_collide.h). PL-094 / PL-110:
 *     0  FA_SOLID_NONE   - empty, suppressed, or a non-solid entry
 *     1  FA_SOLID_FULL   - attr & 0x20 set (fcn.00434180 "stand on this")
 *     2  FA_SOLID_ONEWAY - collision code & 2 set: the one-way / drop-through
 *                          platform (fcn.0x4343c4 top-row-only path). Checked
 *                          before the solid bit, since such tiles usually
 *                          carry attr & 0x20 too.
 * Out of the map reads as 1 (the level edges are walls), matching the
 * integrator's world-rect clamp (fcn.00431bf0).
 */
int fa_map_solid_class(const fa_map *m, int world_x, int world_y);

/* --- hashing ------------------------------------------------------- */

/* FNV-1a-32 over info.raw + grid + tail, for deterministic tests. */
uint32_t fa_map_hash(const fa_map *m);

#ifdef __cplusplus
}
#endif

#endif /* FA_MAP_H */
