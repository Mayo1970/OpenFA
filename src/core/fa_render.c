/*
 * fa_render.c - scene camera + frame compositor (RRR-42). See fa_render.h.
 */
#include "fa/fa_render.h"
#include "fa/fa_surface.h"
#include "fa/fa_w01.h"
#include "fa/fa_map.h"
#include "fa/fa_entity.h"

#include <stdlib.h>
#include <string.h>

/* --- camera ------------------------------------------------------- */

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void fa_camera_init(fa_camera *c, int vw, int vh, int world_w, int world_h)
{
    c->x = c->y = 0;
    c->vw = vw; c->vh = vh;
    c->world_w = world_w; c->world_h = world_h;
    /* exe follow box: the rect stored at 0x411fd6 is {130,280,670,480} on an
     * 800x600 screen; the per-call X slew cap in 0x4349c0 is 0x0a = 10. */
    c->rail_l = 130;
    c->rail_r = vw - 130;
    c->band_t = 280;
    c->band_b = 480;
    c->step_x = 10;
    c->locked = 0;
}

static void cam_clamp(fa_camera *c)
{
    if (c->world_w <= c->vw) c->x = -(c->vw - c->world_w) / 2;
    else                     c->x = clampi(c->x, 0, c->world_w - c->vw);
    if (c->world_h <= c->vh) c->y = -(c->vh - c->world_h) / 2;
    else                     c->y = clampi(c->y, 0, c->world_h - c->vh);
}

void fa_camera_move(fa_camera *c, int dx, int dy)
{
    c->x += dx; c->y += dy;
    cam_clamp(c);
}

void fa_camera_center_on(fa_camera *c, int wx, int wy)
{
    c->x = wx - c->vw / 2;
    c->y = wy - c->vh / 2;
    cam_clamp(c);
}

void fa_camera_intro(fa_camera *c, int spawn_x, int spawn_y)
{
    /* exe 0x4119aa -> 0x434650: desired scroll = (spawn - 100, spawn - 400),
     * tile-aligned then clamped. The tile-align is <=32 px and cosmetic. */
    c->x = spawn_x - 100;
    c->y = spawn_y - 400;
    cam_clamp(c);
}

void fa_camera_boss(fa_camera *c, int world)
{
    /* exe: ds:0x4dabd4 = (world - 1) + 4 for a boss. The intro-cam table
     * 0x412880 fixes the scroll (idx 5 = world 2 -> origin; idx 4/6/7 ->
     * (32,224)); the lock switch 0x4128d0 then kills the follow for idx
     * 4/5/7 (worlds 1/2/4) and leaves it on for idx 6 (world 3). */
    if (world == 2) { c->x = 0;  c->y = 0;   }
    else            { c->x = 32; c->y = 224; }
    c->locked = (world != 3);
    cam_clamp(c);
}

void fa_camera_follow(fa_camera *c, int target_x, int target_y, int facing)
{
    if (c->locked) return;               /* exe follow gate ds:0x4e1018 */

    /* exe 0x4349c0. screen_* = player world pos minus the current scroll. */
    int screen_x = target_x - c->x;
    int screen_y = target_y - c->y;

    if (facing == 0 || facing == 1) {
        int anchor = facing ? c->rail_r : c->rail_l;
        c->x += clampi(screen_x - anchor, -c->step_x, c->step_x);
    }

    if (screen_y < c->band_t)      c->y += screen_y - c->band_t;
    else if (screen_y > c->band_b) c->y += screen_y - c->band_b;

    cam_clamp(c);
}

/* --- tile atlas -------------------------------------------------- */

#define FA_TS_MAX_FRAMES 4     /* attr & 7 selects 0..3 (loader rejects >=4) */

struct fa_tileset {
    int      count;
    int      tile_w, tile_h;
    int      cols;                       /* 640 / tile_w */
    struct { uint16_t *px; int w, h; } frame[FA_TS_MAX_FRAMES];
};

fa_tileset *fa_tileset_build(struct fa_w01 *bg, const struct fa_map *map)
{
    if (!bg || !map) return NULL;
    int tw = map->info.tile_w, th = map->info.tile_h;
    if (tw <= 0 || th <= 0) return NULL;

    fa_tileset *ts = (fa_tileset *)calloc(1, sizeof *ts);
    if (!ts) return NULL;
    ts->tile_w = tw;
    ts->tile_h = th;
    ts->cols = map->info.atlas_cols > 0 ? map->info.atlas_cols : 640 / tw;
    if (ts->cols <= 0) ts->cols = 1;

    int n = fa_w01_count(bg);
    if (n > FA_TS_MAX_FRAMES) n = FA_TS_MAX_FRAMES;
    for (int i = 0; i < n; i++) {
        int w = 0, h = 0;
        if (fa_w01_frame_size(bg, i, &w, &h) != 0 || w <= 0 || h <= 0)
            continue;
        uint16_t *px = (uint16_t *)malloc((size_t)w * h * sizeof(uint16_t));
        if (!px) continue;
        if (fa_w01_decode(bg, i, px) != 0) { free(px); continue; }
        ts->frame[i].px = px;
        ts->frame[i].w = w;
        ts->frame[i].h = h;
        ts->count = i + 1;
    }
    if (ts->count == 0) { free(ts); return NULL; }
    return ts;
}

void fa_tileset_free(fa_tileset *ts)
{
    if (!ts) return;
    for (int i = 0; i < FA_TS_MAX_FRAMES; i++) free(ts->frame[i].px);
    free(ts);
}

int fa_tileset_frame_count(const fa_tileset *ts)
{
    return ts ? ts->count : 0;
}

/* RRR-44 follow-up: per-pixel terrain collision. See fa_render.h. */
int fa_render_solid_px(const struct fa_map *m, const fa_tileset *ts,
                       int world_x, int world_y)
{
    if (!m) return 1;
    int tw = m->info.tile_w, th = m->info.tile_h;
    if (tw <= 0 || th <= 0) return 1;
    if (world_x < 0 || world_y < 0 ||
        world_x >= m->world_w || world_y >= m->world_h)
        return 1;                                   /* level edge -> wall */

    fa_map_entry e = fa_map_cell_entry(m, world_x / tw, world_y / th, 2);
    if (fa_map_entry_empty(e)) return 0;
    if (e.attr & FA_MAP_ATTR_SUPPRESS) return 0;
    if (fa_map_entry_code(e) & 2) return 2;         /* one-way platform */
    if (!(e.attr & FA_MAP_ATTR_SOLID)) return 0;

    /* no decoded atlas -> fall back to the coarse (whole-tile) answer */
    int atlas = fa_map_entry_atlas(e);
    if (!ts || atlas < 0 || atlas >= ts->count || !ts->frame[atlas].px)
        return 1;

    int tile = fa_map_entry_tile(e);
    int cols = ts->cols > 0 ? ts->cols : 1;
    int sx = (tile % cols) * tw;
    int sy = (tile / cols) * th;

    int ox = world_x % tw;
    int oy = world_y % th;
    if (fa_map_entry_flip_x(e)) ox = tw - 1 - ox;
    if (fa_map_entry_flip_y(e)) oy = th - 1 - oy;

    int px = sx + ox, py = sy + oy;
    if (px < 0 || py < 0 ||
        px >= ts->frame[atlas].w || py >= ts->frame[atlas].h)
        return 1;                                   /* off-atlas marker = solid */

    uint16_t v = ts->frame[atlas].px[(size_t)py * ts->frame[atlas].w + px];
    return v != FA_COLORKEY ? 1 : 0;
}

/* --- backdrop --------------------------------------------------- */

/* The far backdrop is .W01 frame 4 (the 800x600 one); pick the frame that
 * covers the viewport, else the largest. */
static int pick_backdrop_frame(const fa_w01 *bg, int vw, int vh)
{
    int best = 0, best_area = -1;
    for (int i = 0; i < fa_w01_count(bg); i++) {
        int w = 0, h = 0;
        if (fa_w01_frame_size(bg, i, &w, &h) != 0) continue;
        if (w >= vw && h >= vh) return i;
        if (w * h > best_area) { best_area = w * h; best = i; }
    }
    return best;
}

/* Far backdrop = frame 4 of the level .W01 (the 800x600 painted wall).
 * RRR-55: the exe draws it as a static screen-locked image - the 8 grid
 * planes all carry MapInfo+278 parallax factor 2 (== 1:1, no inter-plane
 * parallax; RRR-55/draw-order-disasm.md), and an 800-wide backdrop in an
 * 800-wide viewport has zero horizontal slack. So the backdrop pans only
 * across whatever slack it actually has, on BOTH axes, and NEVER wraps -
 * the old half-speed + horizontal wrap produced a moving vertical seam
 * (owner: "layer parts do not match" - the stray dark strip in Welt3 was
 * frame 4's left edge wrapping into view). */
static void draw_backdrop(const fa_surface *dst, const fa_w01 *bg, int frame,
                          const fa_camera *cam)
{
    int fw = 0, fh = 0;
    if (fa_w01_frame_size(bg, frame, &fw, &fh) != 0 || fw <= 0 || fh <= 0) {
        fa_fill(dst, NULL, NULL, fa_rgb565(24, 28, 40));
        return;
    }

    uint16_t *px = (uint16_t *)malloc((size_t)fw * fh * sizeof(uint16_t));
    if (!px) { fa_fill(dst, NULL, NULL, fa_rgb565(24, 28, 40)); return; }
    if (fa_w01_decode(bg, frame, px) != 0) {
        free(px);
        fa_fill(dst, NULL, NULL, fa_rgb565(24, 28, 40));
        return;
    }

    fa_surface src;
    fa_surface_wrap(&src, px, fw, fh, 0);

    /* pan across the frame's own slack on each axis, camera 0..world_slack
     * mapping to backdrop 0..frame_slack; clamp, never wrap. */
    int sx = 0, sy = 0;
    if (fw > dst->w && cam->world_w > cam->vw)
        sx = (int)((int64_t)cam->x * (fw - dst->w) / (cam->world_w - cam->vw));
    if (fh > dst->h && cam->world_h > cam->vh)
        sy = (int)((int64_t)cam->y * (fh - dst->h) / (cam->world_h - cam->vh));
    if (sx < 0) sx = 0;
    if (sx > fw - dst->w) sx = fw - dst->w;
    if (sy < 0) sy = 0;
    if (sy > fh - dst->h) sy = fh - dst->h;

    /* frame smaller than the viewport on an axis: pin at 0 and let the
     * fill show through the margin. */
    if (fw < dst->w || fh < dst->h)
        fa_fill(dst, NULL, NULL, fa_rgb565(20, 40, 30));

    int seg_w = fw < dst->w ? fw : dst->w;
    int seg_h = fh < dst->h ? fh : dst->h;
    fa_rect srect = { sx, sy, seg_w, seg_h };
    fa_blit(dst, 0, 0, &src, &srect, NULL);
    free(px);
}

/* --- grid: real tiles ----------------------------------------- */

#define FA_TILE_TMP 64      /* max tile side we mirror through a stack buffer */

/* Blit one tile region, honouring flip (attr & 0x18). */
static void blit_tile(const fa_surface *dst, int dx, int dy,
                      const fa_tileset *ts, int atlas,
                      int sx, int sy, int flip_x, int flip_y)
{
    if (atlas < 0 || atlas >= ts->count || !ts->frame[atlas].px) return;
    int fw = ts->frame[atlas].w, fh = ts->frame[atlas].h;
    int tw = ts->tile_w, th = ts->tile_h;
    if (sx < 0 || sy < 0 || sx + tw > fw || sy + th > fh) return;  /* off-atlas */

    fa_surface src;
    fa_surface_wrap(&src, ts->frame[atlas].px, fw, fh, 0);
    fa_rect sr = { sx, sy, tw, th };

    if (!flip_x && !flip_y) {
        fa_blit_keyed(dst, dx, dy, &src, &sr, NULL, FA_COLORKEY);
        return;
    }
    if (tw > FA_TILE_TMP || th > FA_TILE_TMP) {   /* too big to mirror: plain */
        fa_blit_keyed(dst, dx, dy, &src, &sr, NULL, FA_COLORKEY);
        return;
    }
    uint16_t tmp[FA_TILE_TMP * FA_TILE_TMP];
    for (int y = 0; y < th; y++) {
        int syy = flip_y ? th - 1 - y : y;
        const uint16_t *srow = ts->frame[atlas].px + (size_t)(sy + syy) * fw + sx;
        uint16_t *drow = tmp + (size_t)y * tw;
        for (int x = 0; x < tw; x++)
            drow[x] = srow[flip_x ? tw - 1 - x : x];
    }
    fa_surface m;
    fa_surface_wrap(&m, tmp, tw, th, 0);
    fa_blit_keyed(dst, dx, dy, &m, NULL, NULL, FA_COLORKEY);
}

/* Draw one grid plane over the viewport. */
static void draw_grid_plane(const fa_surface *dst, const fa_map *m,
                            const fa_tileset *ts, const fa_camera *cam,
                            int plane)
{
    int tw = m->info.tile_w, th = m->info.tile_h;
    if (tw <= 0 || th <= 0) return;

    int cx0 = cam->x / tw, cy0 = cam->y / th;
    int cx1 = (cam->x + cam->vw) / tw + 1;
    int cy1 = (cam->y + cam->vh) / th + 1;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 > m->info.grid_w) cx1 = m->info.grid_w;
    if (cy1 > m->info.grid_h) cy1 = m->info.grid_h;

    for (int cy = cy0; cy < cy1; cy++) {
        for (int cx = cx0; cx < cx1; cx++) {
            fa_map_entry e = fa_map_cell_entry(m, cx, cy, plane);
            if (!fa_map_entry_drawn(e)) continue;

            int tile = fa_map_entry_tile(e);
            int atlas = fa_map_entry_atlas(e);
            int sx = (tile % ts->cols) * tw;
            int sy = (tile / ts->cols) * th;

            blit_tile(dst, cx * tw - cam->x, cy * th - cam->y, ts, atlas,
                      sx, sy, fa_map_entry_flip_x(e), fa_map_entry_flip_y(e));
        }
    }
}

/* --- grid: occupancy overlay (no tileset) --------------------- */

static void draw_grid_overlay(const fa_surface *dst, const fa_map *m,
                              const fa_camera *cam, int debug)
{
    int tw = m->info.tile_w, th = m->info.tile_h;
    if (tw <= 0 || th <= 0) return;
    int cx0 = cam->x / tw, cy0 = cam->y / th;
    int cx1 = (cam->x + cam->vw) / tw + 1;
    int cy1 = (cam->y + cam->vh) / th + 1;

    for (int cy = cy0; cy < cy1; cy++) {
        for (int cx = cx0; cx < cx1; cx++) {
            if (!fa_map_cell_occupied(m, cx, cy)) continue;
            fa_map_entry e = { 0, FA_MAP_ENTRY_EMPTY };
            for (int pl = 0; pl < FA_MAP_PLANES; pl++) {
                e = fa_map_cell_entry(m, cx, cy, pl);
                if (!fa_map_entry_empty(e)) break;
            }
            int sx = cx * tw - cam->x, sy = cy * th - cam->y;
            unsigned k = (unsigned)fa_map_entry_tile(e);
            uint8_t r = (uint8_t)(70 + (k * 37u) % 120);
            uint8_t g = (uint8_t)(90 + (k * 61u) % 130);
            uint8_t b = (uint8_t)(50 + (k * 23u + e.attr * 40u) % 90);
            fa_rect cell = { sx, sy, tw, th };
            fa_fill(dst, &cell, NULL, fa_rgb565(r, g, b));
            if (debug) {
                fa_rect top  = { sx, sy, tw, 2 };
                fa_rect left = { sx, sy, 2, th };
                fa_fill(dst, &top, NULL, fa_rgb565(255, 240, 120));
                fa_fill(dst, &left, NULL, fa_rgb565(255, 240, 120));
            }
        }
    }
}

int fa_render_scene(const fa_surface *dst, const fa_scene *sc,
                    const fa_camera *cam)
{
    if (!dst || !dst->px || !sc || !cam) return -1;

    if (sc->bg && fa_w01_count(sc->bg) > 0) {
        int frame = pick_backdrop_frame(sc->bg, cam->vw, cam->vh);
        draw_backdrop(dst, sc->bg, frame, cam);
    } else {
        fa_fill(dst, NULL, NULL, fa_rgb565(24, 28, 40));
    }

    if (sc->map) {
        int planes = fa_map_plane_count(sc->map);
        int have_tiles = sc->tiles && fa_tileset_frame_count(sc->tiles) > 0;
        const char *dbg = getenv("FA_DBG_PLANEMASK");
        unsigned mask = dbg ? (unsigned)strtoul(dbg, NULL, 0) : 0xFFFFFFFFu;
        /* per plane: entities behind, the tile plane, entities in front
         * (RRR-42/entity-tail-disasm.md: band 1, tiles, band 0, band 2) */
        for (int pl = 0; pl < planes; pl++) {
            if (!(mask & (1u << pl))) continue;
            if (sc->ents) fa_entity_draw_band(dst, sc->ents, cam, pl, 1);
            if (have_tiles) draw_grid_plane(dst, sc->map, sc->tiles, cam, pl);
            if (sc->ents) fa_entity_draw_band(dst, sc->ents, cam, pl, 0);
            /* the exe's per-plane hook (level[+0x16EB+pl*4], 0x433524): the
             * player renderer is installed here for plane 2. */
            if (sc->on_plane) sc->on_plane(sc->on_plane_ud, dst, cam, pl);
            if (sc->ents) fa_entity_draw_band(dst, sc->ents, cam, pl, 2);
        }
        if (!have_tiles)
            draw_grid_overlay(dst, sc->map, cam, sc->draw_grid_overlay);
    }
    return 0;
}
