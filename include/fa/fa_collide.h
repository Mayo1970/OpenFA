/*
 * fa_collide.h - swept AABB resolution against the map
 *
 * A small, deterministic, integer-only collision resolver. It moves one
 * axis-aligned box by its per-tick velocity and resolves it against a
 * solid probe, one pixel at a time, so nothing tunnels even at the
 * terminal fall speed (20 px/tick) or at snowball speed.
 *
 * PARITY
 *   The original resolves collision inside the per-state player code
 *   (JR_FERRERO.exe fcn.00417370 and friends). The shared pieces it leans
 *   on are:
 *     - the coarse solid query fcn.00434180 (IsTileSolid, plane 2): maps a
 *       world pixel to its tile and returns attr & 0x20 -> "stand on this".
 *       See fa_map.h.
 *     - the behaviour code (packed >> 9) & 0x7f: code & 2 has a top-row-only
 *       path in the pixel query fcn.0x4343c4 - a one-way / drop-through
 *       platform. code & 1 is the ladder bit (already used by fa_map).
 *     - the generic integrator fcn.00431bf0: X += vx; Y += vy with a
 *       per-axis velocity clamp and a world-rect position clamp that zeroes
 *       the blocked component. fa_collide reproduces the position clamp
 *       against tiles; the velocity clamp stays in fa_player.
 *   A foot check in the exe samples ONE point - the horizontal centre of
 *   the box at its bottom edge (fcn.004146c8..0x4146f0). fa_collide samples
 *   the centre plus the two bottom corners so a box cannot half-hang a
 *   ledge; every sample is tile-quantised by the probe, exactly as the exe.
 *
 *   The box size (fa_player_tuning.body_hw / body_h) is a first pass, like
 *   the physics constants: no exe symbol pins the player's collision extent.
 *   It is a tuning-only value.
 */
#ifndef FA_COLLIDE_H
#define FA_COLLIDE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Solidity class of the map at a world pixel:
 *   0  passable
 *   1  solid          - blocks from every side          (attr & 0x20)
 *   2  one-way platform- blocks only a downward crossing (code & 2)
 */
enum { FA_SOLID_NONE = 0, FA_SOLID_FULL = 1, FA_SOLID_ONEWAY = 2 };

/* Return the solidity class at world pixel (px, py). */
typedef int (*fa_solid_fn)(int px, int py, void *ctx);

typedef struct fa_aabb_body {
    int32_t x, y;      /* 16.16 world position; the box's bottom centre  */
    int32_t vx, vy;    /* 16.16 per tick                                 */
    int      hw, h;    /* half-width, full height, whole pixels          */
    int      on_ground;/* set on return: a solid pixel one row below     */
    int      hit_x;    /* set on return: a wall stopped horizontal move  */
    int      hit_up;   /* set on return: a ceiling stopped upward move   */
    int      hit_down; /* set on return: a floor stopped downward move   */
} fa_aabb_body;

/* Sub-step size: the box advances at most this many pixels between probes. */
#define FA_COLLIDE_STEP 1

/*
 * Advance `b` by (b->vx, b->vy) for one tick, resolving against `fn`.
 * X is resolved first, then Y. A blocked component is zeroed and its hit
 * flag set; `on_ground` reflects the resting state after the move.
 * `drop_through` != 0 makes one-way platforms passable downward (DOWN held).
 * `fn` NULL -> a plain integrate with no collision.
 * Pure integer, deterministic.
 */
void fa_collide_move(fa_aabb_body *b, int drop_through,
                     fa_solid_fn fn, void *ctx);

/* 1 if a solid or (unless drop_through) one-way pixel sits one row below the
 * box - i.e. the box is resting on ground. Does not move `b`. */
int fa_collide_grounded(const fa_aabb_body *b, int drop_through,
                        fa_solid_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* FA_COLLIDE_H */
