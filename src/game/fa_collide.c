/*
 * fa_collide.c - swept AABB resolution against the map.
 * See fa_collide.h. Fixed-point 16.16, no float, one pixel per sub-step.
 */
#include "fa/fa_collide.h"

#define FIX_ONE 65536

static int px_of(int32_t v) { return (int)(v >> 16); }

/* --- edge probes -------------------------------------------------------
 * The box spans columns [cx - hw, cx + hw] and rows [cy - h + 1, cy],
 * where (cx, cy) is the bottom-centre pixel. */

static int wall_blocks(const fa_aabb_body *b, int sgn,
                       fa_solid_fn fn, void *ctx)
{
    int cx = px_of(b->x), cy = px_of(b->y);
    int col = cx + sgn * (b->hw + 1);          /* the column one px ahead */
    int top = cy - b->h + 1;
    for (int y = top; y <= cy; y++)
        if (fn(col, y, ctx) == FA_SOLID_FULL) return 1;
    return 0;
}

static int floor_blocks(const fa_aabb_body *b, int drop_through,
                        fa_solid_fn fn, void *ctx)
{
    int cx = px_of(b->x), cy = px_of(b->y);
    int row = cy + 1;                          /* the row one px below */
    int xs[3];
    xs[0] = cx - b->hw; xs[1] = cx; xs[2] = cx + b->hw;
    for (int i = 0; i < 3; i++) {
        int c = fn(xs[i], row, ctx);
        if (c == FA_SOLID_FULL) return 1;
        if (c == FA_SOLID_ONEWAY && !drop_through) {
            /* land only when the box is currently above the platform -
             * if this column already reads solid at the feet row we are
             * inside / climbing through it, so pass. */
            if (fn(xs[i], cy, ctx) == FA_SOLID_NONE) return 1;
        }
    }
    return 0;
}

static int ceil_blocks(const fa_aabb_body *b, fa_solid_fn fn, void *ctx)
{
    int cx = px_of(b->x), cy = px_of(b->y);
    int row = cy - b->h;                       /* the row one px above the top */
    int xs[3];
    xs[0] = cx - b->hw; xs[1] = cx; xs[2] = cx + b->hw;
    for (int i = 0; i < 3; i++)
        if (fn(xs[i], row, ctx) == FA_SOLID_FULL) return 1;
    return 0;
}

int fa_collide_grounded(const fa_aabb_body *b, int drop_through,
                        fa_solid_fn fn, void *ctx)
{
    if (!fn) return 0;
    return floor_blocks(b, drop_through, fn, ctx);
}

/* Slope handling. With per-pixel terrain the leading edge
 * hits the diagonal of a slope tile; without a step these read as stairs.
 * STEP_UP: on a grounded horizontal move, climb up to this many px over the
 *          obstruction and keep going.
 * STEP_DOWN: after a grounded move, stick to a surface up to this many px
 *          below so the box follows a descending slope instead of launching. */
#define FA_COLLIDE_STEP_UP    4
#define FA_COLLIDE_STEP_DOWN  8

/* Try to raise the box over a slope pixel blocking the sgn-ward edge. Leaves
 * the box raised and returns 1, or restores it and returns 0. */
static int step_up(fa_aabb_body *b, int sgn, fa_solid_fn fn, void *ctx)
{
    int32_t y0 = b->y;
    for (int up = 1; up <= FA_COLLIDE_STEP_UP; up++) {
        b->y -= FIX_ONE;
        if (!wall_blocks(b, sgn, fn, ctx) && !ceil_blocks(b, fn, ctx))
            return 1;
    }
    b->y = y0;
    return 0;
}

void fa_collide_move(fa_aabb_body *b, int drop_through,
                     fa_solid_fn fn, void *ctx)
{
    int was_ground = b->on_ground;
    b->hit_x = b->hit_up = b->hit_down = 0;

    if (!fn) {
        b->x += b->vx;
        b->y += b->vy;
        b->on_ground = 0;
        return;
    }

    /* ---------- horizontal (with a grounded slope / small-step climb) ------ */
    if (b->vx != 0) {
        int sgn = b->vx < 0 ? -1 : 1;
        int32_t mag = sgn * b->vx;             /* >= 0 */
        int whole = (int)(mag >> 16);
        int32_t frac = mag & (FIX_ONE - 1);

        for (int i = 0; i < whole; i++) {
            if (wall_blocks(b, sgn, fn, ctx) &&
                !(was_ground && step_up(b, sgn, fn, ctx))) {
                b->vx = 0; b->hit_x = 1; frac = 0;
                break;
            }
            b->x += sgn * FIX_ONE;
        }
        if (b->vx != 0 && frac) {
            if (wall_blocks(b, sgn, fn, ctx) &&
                !(was_ground && step_up(b, sgn, fn, ctx)))
                { b->vx = 0; b->hit_x = 1; }
            else b->x += sgn * frac;
        }
    }

    /* ---------- vertical ---------- */
    if (b->vy != 0) {
        int sgn = b->vy < 0 ? -1 : 1;
        int32_t mag = sgn * b->vy;
        int whole = (int)(mag >> 16);
        int32_t frac = mag & (FIX_ONE - 1);

        for (int i = 0; i < whole; i++) {
            int hit = (sgn > 0) ? floor_blocks(b, drop_through, fn, ctx)
                                : ceil_blocks(b, fn, ctx);
            if (hit) {
                b->vy = 0; frac = 0;
                if (sgn > 0) b->hit_down = 1; else b->hit_up = 1;
                break;
            }
            b->y += sgn * FIX_ONE;
        }
        if (b->vy != 0 && frac) {
            int hit = (sgn > 0) ? floor_blocks(b, drop_through, fn, ctx)
                                : ceil_blocks(b, fn, ctx);
            if (hit) {
                b->vy = 0;
                if (sgn > 0) b->hit_down = 1; else b->hit_up = 1;
            } else {
                b->y += sgn * frac;
            }
        }
    }

    /* landed / hit a ceiling flush at the end of the move (the loop checks
     * the row ahead, so a move that ends exactly on the surface is caught
     * here rather than next tick with a stale fall speed) */
    if (b->vy > 0 && floor_blocks(b, drop_through, fn, ctx)) {
        b->vy = 0; b->hit_down = 1;
    } else if (b->vy < 0 && ceil_blocks(b, fn, ctx)) {
        b->vy = 0; b->hit_up = 1;
    }

    /* ---------- slope stick: follow a descending surface ---------- */
    if (was_ground && b->vy >= 0 && !b->hit_up &&
        !floor_blocks(b, drop_through, fn, ctx)) {
        int32_t y0 = b->y;
        int stuck = 0;
        for (int d = 1; d <= FA_COLLIDE_STEP_DOWN; d++) {
            b->y += FIX_ONE;
            if (floor_blocks(b, drop_through, fn, ctx)) { stuck = 1; break; }
        }
        if (stuck) b->vy = 0;
        else       b->y = y0;
    }

    b->on_ground = floor_blocks(b, drop_through, fn, ctx);
}
