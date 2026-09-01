/*
 * fa_entity.h - level object / sprite layer (RRR-42 follow-up, precursor to RRR-50)
 *
 * A .W02 chunk-0 payload is [ 348 MapInfo ][ grid ][ TAIL ]. The tail is the
 * placed-object list: every tree, rock, plant, bush, enemy, pickup, lift and
 * moving platform in the level. The tile renderer (fa_render.h) draws only
 * the backdrop and the tile grid, so without this module every level is
 * missing its sprites.
 *
 * Reverse-engineered from JR_FERRERO.exe - see RRR-42/entity-tail-disasm.md
 * (PL-105..108). Summary:
 *   - tail header: 26 bytes { u32 record_bytes; u16 record_count; u8 x[20] }.
 *     Only record_count (@+4) is read; record_bytes == count*186 in every
 *     shipped map.
 *   - record: 186 bytes. @0 i16 ObjNr (-1 invalid); @2/@4 i16 world X/Y in
 *     SIGNED WHOLE PIXELS (not 20.12); @6 u16 active (0 = skip, 0xFF =
 *     infinite); @0xE i16 current frame; @0x16 anim mode (2 = reverse);
 *     @0x17 draw band 0..2; @0x18 DetailGroup 0..4; @0x19 tile plane;
 *     @0x2B flip X; @0xB9 hidden.
 *   - ObjNr resolves into a global ObjNr->AOM-definition table built from
 *     every SCRIPT_TYP="AOM" file in GData\Scripts\ (fa_aom.h). The def gives
 *     FileName (a .W01 sheet), FileAnimStart/End and DetailGroup.
 *   - drawn frame = clamp(record.frame, def.start, def.end) - def.start into
 *     the .W01, at (world + W01 frame origin - camera), keyed, mirrored on
 *     flip X.
 *   - draw order: buckets keyed by (tile_plane, draw_band). Per tile plane p:
 *     band 1 (behind the plane), the plane's tiles, band 0, band 2.
 *
 * WHAT THIS MODULE DOES (RRR-42 + RRR-50): parse the tail, resolve every
 * placement to its sprite, draw it in the right bucket order, and - RRR-50 -
 * run the generic per-object runtime every simulation tick:
 *   - animation advance: a [first,last] range clocked by a per-record timer,
 *     forward or reverse (@0x16 == 2), wrapping; a wrap decrements the
 *     lifetime word (@0x6) unless it is 0xFF (infinite) (fcn.004335A0).
 *   - generic patrol movement: @0x1C automove, @0x1D step px/tick, @0x1F..
 *     @0x25 min/max X/Y bounds (-1 = unbounded), @0x27 direction bits
 *     (1 L, 2 R, 4 U, 8 D). At a bound the object waits @0x28 ticks, reverses
 *     that axis, and XORs its facing (@0x2B) if @0x2C is set
 *     (fcn.00430CF0 + fcn.00430D90).
 *   - the on-screen update gate: an off-screen object is not ticked unless
 *     @0xB8 forces it (fcn.004335A0 @0x4336da).
 *   - the player start: the ObjNr 1000 (misc_start.jrs) record carries the
 *     level's spawn X/Y - the exe reads it at 0x4118f5 and places the player
 *     there, then deactivates the marker (PL-127).
 *   - player <-> object coupling: DetailGroup 0 lifts carry the player,
 *     DetailGroup 1/2 pickups deactivate on contact, DetailGroup 3 enemies
 *     report a hit (PL-129/131).
 *
 * WHAT IT DEFERS TO RRR-51: the per-ObjNr behaviour callbacks the exe
 * installs at record +0x5E (fcn.004311C0 / the ~60 calls at 0x4119F7..) -
 * enemy AI, Attack/Freeze/KO state switching, projectile <-> enemy, the
 * exact lift-landing sub-branches (jump table 0x412BD4), and pushables.
 * fa_entity_set_behaviour() is the seam those bind through.
 *
 * Reverse-engineered from JR_FERRERO.exe - RRR-50/entity-runtime-disasm.md
 * (PL-127..132), building on RRR-42/entity-tail-disasm.md (PL-105..108).
 */
#ifndef FA_ENTITY_H
#define FA_ENTITY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fa_map;
struct fa_surface;
struct fa_camera;

#define FA_ENTITY_REC_BYTES  186
#define FA_ENTITY_BANDS        3
#define FA_ENTITY_PLANES       8    /* matches FA_MAP_PLANES */

/* One decoded placement (the fields this module uses; the raw 186 bytes are
 * kept for the RRR-50 runtime). */
typedef struct fa_entity_rec {
    int   obj_nr;
    int   x, y;              /* signed whole-pixel world position (mutated)  */
    int   frame;             /* current sheet frame (mutated by the tick)    */
    int   active;            /* @6: 0 = inactive, else drawn (0xFF = infinite) */
    int   band;              /* @0x17: 0..2 */
    int   detail_group;      /* @0x18: 0..4 */
    int   plane;             /* @0x19: 0..7 */
    int   flip_x;            /* @0x2B (mutated at a patrol endpoint)         */
    int   hidden;            /* @0xB9 */
    int   force_offscreen;   /* @0xB8 */

    /* --- RRR-50 runtime state (advanced by fa_entity_tick) ------------- */
    int   anim_first, anim_last;   /* @0xA/@0xC current playing range        */
    int   anim_mode;               /* @0x16: 2 = reverse, else forward       */
    int   anim_extra_delay;        /* @0x10: added to the base frame delay   */
    int   anim_timer;              /* @0x12: frame countdown                 */
    int   active_reset;            /* @0x08: lifetime reload value           */
    int   automove;                /* @0x1C                                  */
    int   move_step;               /* @0x1D: px/tick                         */
    int   min_x, min_y, max_x, max_y; /* @0x1F/@0x21/@0x23/@0x25 (-1 = none) */
    int   move_dir;                /* @0x27: bits 1 L, 2 R, 4 U, 8 D         */
    int   wait_reset, wait;        /* @0x28/@0x29: endpoint pause            */
    int   flip_at_endpoint;        /* @0x2C                                  */
    int   collision_enabled;       /* @0x1A                                  */
    int   collision_bottom_adjust; /* @0x2A                                  */
    int   dx, dy;                  /* movement this tick (for lift carry)    */
    uint64_t anim_cycles;          /* completed wraps of the active range    */

    /* classification + generic physics (RRR-50 follow-up) */
    int   is_lift;                 /* DetailGroup 0 stand-on platform / raft */
    int   deck_off;               /* lift: stand-on surface, px below y0    */
    int   is_block;                /* DetailGroup 0 pushable block (ObjNr 76/78) */
    int   gravity;                 /* generic physics: enemy snap / block fall */
    int   snapped;                 /* enemy: the one-time spawn ground-snap ran */
    int   vy_acc;                  /* 1/256 px fall-speed accumulator        */
    int   fall_px;                 /* block: continuous fall distance (px)   */

    /* RRR-51 per-object behaviour scratch, owned by fa_beh.c. The exe keeps
     * the same working set in spare record bytes (logic state at +0x62,
     * cooldown at +0x74, etc.); this module does not interpret it. */
    int32_t bs[12];

    uint8_t raw[FA_ENTITY_REC_BYTES];
} fa_entity_rec;

typedef struct fa_entity_store fa_entity_store;

/*
 * Parse the tail of `map` (already loaded) and build the sprite set. `gdata`
 * is the GData directory - Scripts\ for the AOM definitions, Animation\ for
 * the .W01 sheets. Returns a heap object (free with fa_entity_free) or NULL
 * (no tail / out of memory / no Scripts dir).
 */
fa_entity_store *fa_entity_load(const struct fa_map *map, const char *gdata);

void fa_entity_free(fa_entity_store *s);

int  fa_entity_count(const fa_entity_store *s);          /* records parsed   */
int  fa_entity_drawable(const fa_entity_store *s);       /* active, resolved */
int  fa_entity_def_count(const fa_entity_store *s);      /* AOM defs indexed */

/* The parsed AOM definition for an ObjNr (FileAnim + move/attack/freeze/ko
 * ranges), or NULL if it was not indexed. fa_beh.c reads the animation
 * ranges the exe's state selector (0x430B20) picks from def+0x39. */
struct fa_aom_def;
const struct fa_aom_def *fa_entity_def(const fa_entity_store *s, int obj_nr);

/* Mutable access to the i-th record, for the RRR-51 behaviour layer. */
fa_entity_rec *fa_entity_at_mut(fa_entity_store *s, int i);

/* World-space AABB of record i's current frame (sprite origin + size,
 * mirrored on flip_x). Returns 0 and fills the corners, or -1 if the record
 * has no resolved sprite. */
int fa_entity_frame_box(const fa_entity_store *s, int i,
                        int *x0, int *y0, int *x1, int *y1);

struct fa_w01;
/* The .W01 sheet an ObjNr draws from, or NULL. Enemy projectiles render from
 * the throwing enemy's own sheet (0x40AF80), e.g. kong (ObjNr 5) frame 16 is
 * a banana; the gorilla boss's coconut is its own frame 87. */
const struct fa_w01 *fa_entity_obj_sheet(const fa_entity_store *s, int obj_nr);

/* The i-th record (0..count-1), or NULL. For tests / tools. */
const fa_entity_rec *fa_entity_at(const fa_entity_store *s, int i);

/*
 * Draw the (plane, band) bucket into `dst` (an 800x600 RGB565 surface) for
 * camera `cam`. Records render in file order within the bucket; each is
 * skipped if inactive, hidden, unresolved, or fully off-screen. Returns the
 * number of records blitted, or -1 on a bad argument.
 *
 * The caller interleaves this with the tile planes:
 *   for p in 0..planes-1:
 *     fa_entity_draw_band(dst, s, cam, p, 1);   // behind tile plane p
 *     ... draw tile plane p ...
 *     fa_entity_draw_band(dst, s, cam, p, 0);   // in front
 *     fa_entity_draw_band(dst, s, cam, p, 2);   // in front
 */
int fa_entity_draw_band(const struct fa_surface *dst, const fa_entity_store *s,
                        const struct fa_camera *cam, int plane, int band);

/* ================================================================
 * RRR-50 runtime
 * ================================================================ */

/*
 * Advance every active object one 60 Hz simulation tick: the on-screen gate
 * (an object outside the viewport is skipped unless force_offscreen), the
 * animation frame timer, and the generic patrol move. `cam_*` / `view_*`
 * give the current viewport in world pixels for the gate. Records the
 * per-object (dx, dy) for fa_entity_ride. Returns the number of objects
 * ticked, or -1 on a bad argument.
 *
 * Call once per tick, after the player moves (so lift carry reads the
 * lift's fresh position).
 */
int fa_entity_tick(fa_entity_store *s, int cam_x, int cam_y,
                   int view_w, int view_h);

/*
 * Bind a terrain-solidity probe (0 none / 1 solid / 2 one-way, the same
 * fa_map_solid_class / fa_render_solid_px the player uses). Generic fallers
 * (enemies without a vertical patrol bound) use it to stop on the ground.
 * NULL = no gravity (headless / no GData). This is a stop-gap until the
 * per-ObjNr AI (RRR-51) gives each enemy type its real movement.
 */
typedef int (*fa_entity_solid_fn)(int px, int py, void *ctx);
void fa_entity_set_terrain(fa_entity_store *s, fa_entity_solid_fn fn, void *ctx);

/*
 * Solidity of an active DetailGroup-0 object at world pixel (px, py), for
 * the player's collision probe: a lift/raft is solid only in a thin band at
 * its top surface (you walk onto it, you do not clip its sides); a pushable
 * block is solid over its whole box. Returns 1 (solid) or 0.
 */
int fa_entity_solid_at(const fa_entity_store *s, int px, int py);

/*
 * Fettalatte's shove (PL-103). Find the pushable block the probe point
 * (px + facing*reach, feet_y - 16) lands in and move it up to `step` px in
 * `facing` (+1 right / -1 left), stopping it at solid terrain or another
 * block. Returns the pixels actually moved (0 if nothing pushable or fully
 * blocked); add the same delta to the player.
 */
int fa_entity_shove(fa_entity_store *s, int px, int feet_y, int facing,
                    int reach, int step);

/* 1 if a pushable block sits at world pixel (px, py) - bind through
 * fa_player_set_pushable so Fettalatte enters FA_PST_PUSH. */
int fa_entity_pushable_at(const fa_entity_store *s, int px, int py);

/*
 * The level spawn point: the ObjNr 1000 (misc_start.jrs) record's world X/Y
 * (PL-127). Returns 1 and fills x and y if the marker is present, else 0.
 * fa_entity_load has already deactivated the marker so it never renders.
 */
int fa_entity_player_start(const fa_entity_store *s, int *x, int *y);

/* --- the RRR-51 behaviour seam --------------------------------- */

/*
 * Per-object callback, installed at the exe's record +0x5E by ObjNr. Called
 * from fa_entity_tick after the animation advances, before the generic
 * patrol. `wrapped` is 1 on the tick the animation range looped. Return
 * non-zero to also run the generic patrol move (the exe default), 0 to
 * suppress it (the callback owns movement).
 */
typedef int (*fa_entity_behaviour)(fa_entity_rec *e, int wrapped, void *ctx);

/* Bind a behaviour to every record with this ObjNr. `fn` NULL clears it.
 * Returns 0, or -1 on a bad store. */
int fa_entity_set_behaviour(fa_entity_store *s, int obj_nr,
                            fa_entity_behaviour fn, void *ctx);

/* --- player <-> object coupling -------------------------------- */

/*
 * Does the player rest on a DetailGroup-0 lift? `px` is the player centre X,
 * `feet_y` the world Y of the player's feet, `half_w` its half-width.
 * A lift matches when its sprite top surface is within FA_ENTITY_RIDE_SLOP
 * pixels of the feet and its X span overlaps the player box. On a match
 * returns 1 and fills lift_top (snap the feet here) and carry_dx / carry_dy
 * (add to the player position this tick). Returns 0 otherwise.
 */
#define FA_ENTITY_RIDE_SLOP 8
int fa_entity_ride(const fa_entity_store *s, int px, int feet_y, int half_w,
                   int *lift_top, int *carry_dx, int *carry_dy);

/* Pickup callback: the collected object's ObjNr and DetailGroup (1 BONUS /
 * 2 POWERUP). */
typedef void (*fa_entity_pickup_cb)(int obj_nr, int detail_group, void *ctx);

/*
 * Collect every active DetailGroup 1/2 object whose sprite AABB overlaps the
 * player box (centre px,py; half extents half_w/half_h). Each match is
 * deactivated (active = 0, matching the exe's rec[+6] = 0 pickup path) and
 * reported through `cb`. Returns the number collected, or -1 on a bad arg.
 */
int fa_entity_collect(fa_entity_store *s, int px, int py,
                      int half_w, int half_h,
                      fa_entity_pickup_cb cb, void *ctx);

/*
 * Is the player box touching an active DetailGroup-3 enemy? Fills *idx with
 * the record index of the first overlap. Returns 1 on a hit, 0 otherwise.
 * The stomp-vs-damage resolution is RRR-51.
 */
int fa_entity_hazard(const fa_entity_store *s, int px, int py,
                     int half_w, int half_h, int *idx);

#ifdef __cplusplus
}
#endif

#endif /* FA_ENTITY_H */
