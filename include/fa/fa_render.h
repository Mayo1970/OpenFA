/*
 * fa_render.h - scene camera + frame compositor
 *
 * Composites one 800x600 RGB565 frame from a level: the far backdrop .W01
 * frame first, then the tile grid plane by plane (0 back .. N-1 front), with
 * DirectDraw-style BltFast (fa_surface.h). Mirrors the original render loop
 * fcn.00433110.
 *
 * What is faithful: the pixel format, the keyed blit, the plane draw order,
 * the tile source-rect math (tile = packed & 0x1FF; atlas = attr & 7;
 * src = (tile%cols, tile/cols) * tile_size; flip from attr & 0x18), and the
 * "code > 50 / attr & 0x40 / off-atlas tile -> do not draw" rules.
 *
 * The far backdrop (frame 4 of the level .W01) is screen-locked: it pans
 * only across its own slack on each axis and never wraps. The 8 grid planes
 * all scroll 1:1 - MapInfo+278 gives every plane parallax factor 2.
 *
 * Best-effort, not fully verified: the designer meaning of the collision code
 * bits and attr 0x80.
 *
 * A scene with no fa_tileset (tiles == NULL) falls back to the old
 * occupancy-colour overlay so the semantics tests run without GData.
 */
#ifndef FA_RENDER_H
#define FA_RENDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fa_surface;
struct fa_w01;
struct fa_map;
struct fa_entity_store;

typedef struct fa_camera {
    int x, y;              /* scroll: top-left world pixel of the viewport */
    int vw, vh;            /* viewport size (800x600)              */
    int world_w, world_h;  /* level extent in pixels, for the clamp */

    /* Follow behaviour, re-derived from the exe (follow routine
     * 0x4349c0; the box it reads is stored by 0x411fd6 = {130,280,670,480}
     * for an 800x600 screen). It is NOT a symmetric deadzone:
     *  - X: the player is pinned to screen column `rail_l` while facing
     *    right, or `rail_r` while facing left. The camera slews at most
     *    `step_x` px per follow call, so a turn-around pans the view
     *    across to the other rail.
     *  - Y: a push band [band_t, band_b] in screen space. The camera
     *    moves only when the player leaves the band, by the full
     *    overshoot (no per-call cap on Y).
     * Defaults: rail_l 130, rail_r vw-130, band 280..480, step_x 10. */
    int rail_l, rail_r;
    int band_t, band_b;
    int step_x;

    /* Boss arenas fix the camera (exe follow gate ds:0x4e1018). When set,
     * fa_camera_follow is a no-op. fa_camera_boss / fa_camera_init own it. */
    int locked;
} fa_camera;

void fa_camera_init(fa_camera *c, int vw, int vh, int world_w, int world_h);

/* Move by (dx,dy) then clamp so the viewport stays inside the world (or is
 * centred on an axis the world is smaller than). */
void fa_camera_move(fa_camera *c, int dx, int dy);

/* Put (wx,wy) at the viewport centre, then clamp. */
void fa_camera_center_on(fa_camera *c, int wx, int wy);

/* Point the camera at a fresh spawn (exe 0x4119aa -> 0x434650): scroll =
 * (spawn_x - 100, spawn_y - 400), then clamp. */
void fa_camera_intro(fa_camera *c, int spawn_x, int spawn_y);

/* Boss-arena camera (`world` 1..4). The exe fixes the scroll (intro-cam
 * table 0x412880) and, for worlds 1/2/4, disables the follow (lock switch
 * 0x4128d0); world 3's arena keeps following. Sets `locked` accordingly. */
void fa_camera_boss(fa_camera *c, int world);

/* Per-frame follow (exe 0x4349c0). `facing` is 0 (player faces
 * right) or 1 (faces left); any other value leaves X unchanged this call.
 * X slews toward the facing rail at most step_x px; Y snaps the player
 * back inside the push band; then the result clamps to the world bounds. */
void fa_camera_follow(fa_camera *c, int target_x, int target_y, int facing);

/* --- tile atlas ---------------------------------------------------- */

/*
 * The decoded tile atlases for a level: .W01 frames 0..3 (each 640x480),
 * held as RGB565 surfaces the grid compositor blits 32x32 regions from.
 * Build once per level, free with the level.
 */
typedef struct fa_tileset fa_tileset;

/* Decode the atlas frames of `bg` using the tile size / column count in
 * `map`. Returns a heap object (free with fa_tileset_free), or NULL. */
fa_tileset *fa_tileset_build(struct fa_w01 *bg, const struct fa_map *map);

void fa_tileset_free(fa_tileset *ts);

int fa_tileset_frame_count(const fa_tileset *ts);

/*
 * Per-pixel terrain collision, reproducing the exe's richer query
 * fcn.00434240: it maps a world pixel to a plane-2 tile, and
 * if that tile has attr & 0x20 it samples the actual decoded atlas pixel at
 * the flip-transformed sub-tile offset - a non-key pixel is solid. This is
 * what makes SLOPES follow their drawn diagonal instead of reading as a
 * full 32x32 block. Returns:
 *     0  passable          (empty tile, no attr&0x20, or a key pixel)
 *     1  solid              (a non-key atlas pixel under attr&0x20)
 *     2  one-way platform   (collision code & 2 - handled tile-level by the
 *                            caller; the top row only, per fcn.0x4343c4)
 * `ts` NULL (no GData) falls back to the coarse tile query
 * (fa_map_solid_class), so headless tests still work.
 */
int fa_render_solid_px(const struct fa_map *m, const fa_tileset *ts,
                       int world_x, int world_y);

/* --- scene ------------------------------------------------------- */

typedef struct fa_scene {
    struct fa_w01       *bg;    /* BACKGROUNDPOOL .W01 (may be NULL)  */
    const struct fa_map *map;   /* the level                          */
    const fa_tileset    *tiles; /* decoded atlases; NULL = overlay     */
    const struct fa_entity_store *ents;  /* placed sprites; NULL = none */
    int                  draw_grid_overlay;  /* 1 = bright cell edges  */

    /* Per-plane draw hook. Called once per tile plane, AFTER that plane's
     * tiles and its band-0 entities, BEFORE its band-2 entities - exactly
     * where the exe fires level[+0x16EB + plane*4] (0x433524). The game
     * installs the player renderer (0x41A780) as the PLANE 2 hook
     * (0x417150 -> 0x432820(2, ...)), so the kid draws mid-scene and the
     * foreground tile planes 3/4 (jungle spikes, factory pipes) and any
     * plane-2 band-2 entities occlude it. fa_slice uses this to draw the
     * kid + thrown snowballs at plane 2. NULL = no hook (headless tests). */
    void (*on_plane)(void *ud, const struct fa_surface *dst,
                     const struct fa_camera *cam, int plane);
    void  *on_plane_ud;
} fa_scene;

/*
 * Draw the scene for camera `cam` into `dst` (an 800x600 RGB565 surface).
 * Returns 0, or -1 on a bad argument. Deterministic: the same
 * (scene, camera, surface size) always produces the same pixels.
 */
int fa_render_scene(const struct fa_surface *dst, const fa_scene *sc,
                    const fa_camera *cam);

#ifdef __cplusplus
}
#endif

#endif /* FA_RENDER_H */
