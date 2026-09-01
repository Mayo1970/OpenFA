/*
 * fa_player.c - the player controller (RRR-43). See fa_player.h.
 *
 * Fixed-point, one step per 60 Hz tick, no float. Movement lifted from
 * JR_FERRERO.exe fcn.00417370 / fcn.00431bf0 (PL-083/084). The original
 * stores position/velocity in 20.12 fixed point; this module keeps 16.16
 * internally - only the px/tick values below carry over, not the base.
 */
#include "fa/fa_player.h"
#include "fa/fa_collide.h"

#include <stdlib.h>
#include <string.h>

/* Units are px/tick (or px/tick^2). At 60 Hz, 1.0 px/tick = 60 px/s.
 * All from JR_FERRERO.exe except `gravity` (PL-084 estimate). */
const fa_player_tuning FA_PLAYER_DEFAULT_TUNING = {
    /* gravity          */ (FA_FIX(6)) / 10,   /* 0.6 px/tick^2  (PL-087: shared world const, float @0x452254) */
    /* jump_vel         */ -(FA_FIX(11)),      /* penguin: -11.0 px/tick (PL-084: exe 0x B000) */
    /* jump_vel_c1      */ -(FA_FIX(9)),       /* Fettalatte: lower jump (owner-tuned)   */
    /* jump_hold_gravity*/ (FA_FIX(2)) / 10,   /* 0.2 px/tick^2 while JUMP held + rising */
    /* jump_hold_ticks  */ 10,                 /* exe [0x4e10b0] init 10 in state 4   */
    /* run_speed      */ FA_FIX(5),            /* +/-5.0 px/tick, set directly (exe 0x5000) */
    /* ground_drag    */ FA_FIX(5),            /* instant stop off the ground        */
    /* air_accel      */ (FA_FIX(12)) / 10,    /* 1.2 px/tick^2  (exe 0x1333)        */
    /* air_max        */ FA_FIX(5),            /* air horizontal clamp               */
    /* crouch_max     */ FA_FIX(1),
    /* floor_y        */ FA_FIX(480),
    /* glide_max_vy   */ (FA_FIX(15)) / 10,    /* 1.5 px/tick slow sink (penguin) */

    /* climb_speed    */ FA_FIX(3),            /* PL-102: 3 px/tick on a ladder     */
    /* climb_jump_vx  */ FA_FIX(5),            /* PL-102: 0x5000 hop-off             */
    /* idle_delay     */ 300,                  /* PL-101: 0x12c on entering stand    */
    /* idle_repeat    */ 240,                  /* PL-101: 0xf0 between later rolls   */
    /* swap_ticks     */ 94,                   /* PL-104: pi0020.wav 1.57s @ 60 Hz   */
    /* swap_ticks_c1  */ 124,                  /* PL-104: ms0013.wav 1.75s + turn-back */
    /* swap_end_c1    */ 19,                   /* PL-104: MILCH 150..159 = 9 frames*2  */
    /* push_obj_vx    */ FA_FIX(7),            /* PL-103: 0x40e00000 shove speed    */

    /* throw_cooldown */ 4,                    /* small gap after the throw lock ends */
    /* throw_ticks    */ 54,                   /* penguin: (260-233)*2, PL-100        */
    /* throw_ticks_c1 */ 46,                   /* Fettalatte: (296-273)*2            */
    /* throw_release  */ 44,                   /* penguin: frame 255 = (255-233)*2   */
    /* throw_release_c1*/ 36,                  /* Fettalatte: frame 291 = (291-273)*2 */
    /* snow_vx      */ FA_FIX(16),             /* forward throw (PL-085: 0x41800000)  */
    /* snow_vy      */ -(FA_FIX(23)) / 2,      /* -11.5 (exe -11 or -12, randomised)  */
    /* snow_vx_up   */ FA_FIX(9),              /* up throw (Up held + Fire)           */
    /* snow_vy_up   */ -(FA_FIX(21)),          /* -21 (exe -20..-22, randomised)      */
    /* snow_gravity */ FA_FIX(1),             /* 1.0 px/tick^2 (PL-086)              */
    /* snow_term_vy */ FA_FIX(20),            /* 20.0 terminal (PL-086)             */
    /* snow_life    */ 120,
    /* snow_off_x   */ FA_FIX(32),            /* PL-085: player.X +/-32             */
    /* snow_off_y   */ -(FA_FIX(100)),        /* PL-085: player.Y -100             */
    /* body_hw      */ 10,                    /* RRR-44 / PL-109: first pass        */
    /* body_h       */ 44,                    /* feet at y, head ~44 px up          */
};

static int32_t approach(int32_t v, int32_t target, int32_t step)
{
    if (v < target) { v += step; if (v > target) v = target; }
    else if (v > target) { v -= step; if (v < target) v = target; }
    return v;
}

static void mix(fa_player *p, uint32_t w)
{
    for (int i = 0; i < 4; i++) {
        p->hash ^= (w & 0xffu);
        p->hash *= 16777619u;
        w >>= 8;
    }
}

void fa_player_init(fa_player *p, int spawn_x, int spawn_y)
{
    memset(p, 0, sizeof *p);
    p->t = FA_PLAYER_DEFAULT_TUNING;
    p->x = FA_FIX(spawn_x);
    p->y = FA_FIX(spawn_y);
    p->facing = FA_FACE_RIGHT;
    p->state = FA_PST_STAND;
    p->on_ground = 1;
    p->idle_timer = p->t.idle_delay;
    p->rng_state = 0x9e3779b9u ^ (uint32_t)(spawn_x * 2654435761u + spawn_y);
    if (p->rng_state == 0) p->rng_state = 1;
    p->hash = 2166136261u;
}

void fa_player_set_ground(fa_player *p, fa_ground_fn fn, void *ctx)
{
    p->ground_fn = fn;
    p->ground_ctx = ctx;
}

void fa_player_set_ladder(fa_player *p, fa_ladder_fn fn, void *ctx)
{
    p->ladder_fn = fn;
    p->ladder_ctx = ctx;
}

void fa_player_set_solid(fa_player *p, int (*fn)(int px, int py, void *ctx),
                         void *ctx)
{
    p->solid_fn = fn;
    p->solid_ctx = ctx;
}

/* RRR-44: a solid terrain pixel just below the feet (centre + both corners). */
static int feet_blocked(const fa_player *p)
{
    if (!p->solid_fn) return 0;
    int px = fa_player_px(p), py = fa_player_py(p);
    int hw = p->t.body_hw > 0 ? p->t.body_hw : 1;
    return p->solid_fn(px,      py + 1, p->solid_ctx) == FA_SOLID_FULL ||
           p->solid_fn(px - hw, py + 1, p->solid_ctx) == FA_SOLID_FULL ||
           p->solid_fn(px + hw, py + 1, p->solid_ctx) == FA_SOLID_FULL;
}

/* xorshift32 - a deterministic stand-in for the game RNG 0x4395b0 (RRR-52). */
static uint32_t prng(fa_player *p)
{
    uint32_t x = p->rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    p->rng_state = x;
    return x;
}

void fa_player_set_pushable(fa_player *p, fa_ladder_fn fn, void *ctx)
{
    p->pushable_fn = fn;
    p->pushable_ctx = ctx;
}

/* 1 if a ladder tile covers the world pixel (px, py). */
static int ladder_at(fa_player *p, int px, int py)
{
    return p->ladder_fn && p->ladder_fn(px, py, p->ladder_ctx);
}

/*
 * PL-102 / exe fcn.0041a830 (IsLadderAtOrNear): the climb grab has a long
 * UPWARD reach - the original probes the feet, feet-10 and feet-160, so a
 * vine that hangs up to ~160 px overhead can still be grabbed from the
 * ground (the kid then climbs up through the air to reach it). We scan the
 * whole column so a vine anywhere in that band counts.
 */
#define FA_CLIMB_REACH 160
/* 1 if a vine tile sits between world Y `y` and `y - FA_CLIMB_REACH`, at the
 * player's column. */
static int ladder_near_y(fa_player *p, int y)
{
    if (!p->ladder_fn) return 0;
    int px = fa_player_px(p);
    for (int dy = 0; dy <= FA_CLIMB_REACH; dy += 4)
        if (p->ladder_fn(px, y - dy, p->ladder_ctx)) return 1;
    return 0;
}
static int ladder_near(fa_player *p)
{
    return ladder_near_y(p, fa_player_py(p));
}
static int pushable_at(fa_player *p, int px, int py)
{
    return p->pushable_fn && p->pushable_fn(px, py, p->pushable_ctx);
}

static void finalize(fa_player *p, int jump_raw, int swit, int fire, int in_throw)
{
    p->jump_held_prev   = jump_raw;
    p->switch_held_prev = swit;
    p->fire_held_prev   = fire;
    p->tick++;

    mix(p, (uint32_t)p->x);
    mix(p, (uint32_t)p->y);
    mix(p, (uint32_t)((p->throw_anim << 2) | (p->throw_up << 1) | in_throw));
    mix(p, (uint32_t)p->vx);
    mix(p, (uint32_t)p->vy);
    mix(p, (uint32_t)((p->state << 8) | (p->gliding << 5) | (p->character << 4) |
                      (p->on_ground << 1) | (p->facing == FA_FACE_RIGHT)));
    mix(p, (uint32_t)((p->swap_timer << 8) | (p->idle_kind << 4) |
                      (p->on_ladder << 2) | (p->climb_moving << 1)));
    mix(p, (uint32_t)fa_player_live_snowballs(p));
}

static int32_t ground_at(const fa_player *p, int32_t x)
{
    if (p->ground_fn) {
        int32_t g = p->ground_fn(x, p->ground_ctx);
        if (g != INT32_MAX) return g;
    }
    return p->t.floor_y;
}

/* PL-086: X += vx; Y += vy; vy += gravity (clamped to snow_term_vy). Killed
 * on lifetime, on the ground, or off the world (collision is RRR-44). */
static void step_snowballs(fa_player *p)
{
    for (int i = 0; i < FA_MAX_SNOWBALLS; i++) {
        fa_snowball *s = &p->snow[i];
        if (!s->alive) continue;
        s->x  += s->vx;
        s->y  += s->vy;
        s->vy += p->t.snow_gravity;
        if (s->vy > p->t.snow_term_vy) s->vy = p->t.snow_term_vy;
        s->age++;
        int32_t g = ground_at(p, s->x);
        int hit_tile = p->solid_fn &&
            p->solid_fn((int)(s->x >> 16), (int)(s->y >> 16), p->solid_ctx)
                == FA_SOLID_FULL;
        if (s->age >= p->t.snow_life || s->y >= g || hit_tile) s->alive = 0;
    }
}

/* PL-085: forward throw = vx +/-16, vy ~-11.5; with UP held = vx +/-9,
 * vy ~-21. Spawn at player.X +/-32, player.Y -100. */
static void spawn_snowball(fa_player *p, int up)
{
    for (int i = 0; i < FA_MAX_SNOWBALLS; i++) {
        if (p->snow[i].alive) continue;
        fa_snowball *s = &p->snow[i];
        s->alive = 1;
        s->age = 0;
        s->dir = p->facing;
        s->x = p->x + (int32_t)p->facing * p->t.snow_off_x;
        s->y = p->y + p->t.snow_off_y;
        s->vx = (int32_t)p->facing * (up ? p->t.snow_vx_up : p->t.snow_vx);
        s->vy = up ? p->t.snow_vy_up : p->t.snow_vy;
        return;
    }
    /* pool full (10) - the shot is dropped, exactly as the original does */
}

void fa_player_tick(fa_player *p, uint32_t in)
{
    const fa_player_tuning *t = &p->t;

    int left  = (in & FA_PI_LEFT)  != 0;
    int right = (in & FA_PI_RIGHT) != 0;
    int up    = (in & FA_PI_UP)    != 0;
    int down  = (in & FA_PI_DOWN)  != 0;
    int jump  = (in & FA_PI_JUMP)  != 0;
    int fire  = (in & FA_PI_FIRE)  != 0;
    int swit  = (in & FA_PI_SWITCH) != 0;

    int jump_edge   = jump && !p->jump_held_prev;
    int switch_edge = swit && !p->switch_held_prev;
    int fire_edge   = fire && !p->fire_held_prev;
    int jump_raw    = jump;

    /* ---- SWAP (PL-104): all input locked; toggle the kid when it ends ---- */
    if (p->swap_timer > 0) {
        p->vx = p->vy = 0;
        if (--p->swap_timer == 0) {
            p->character ^= 1;
            p->state = FA_PST_STAND;
            p->idle_timer = t->idle_delay;
        } else {
            p->state = FA_PST_SWAP;
        }
        finalize(p, jump_raw, swit, fire, 0);
        return;
    }

    /* ---- CLIMB (PL-102, exe state 13 fcn.004186dc): no gravity while on a
     * vine. UP climbs toward a vine anywhere within FA_CLIMB_REACH overhead
     * (the kid rises through the air to grab it) and stops at its top; DOWN
     * descends while a vine covers the feet, then steps onto the ground. ---- */
    if (p->state == FA_PST_CLIMB) {
        int px = fa_player_px(p);
        int step = t->climb_speed >> 16; if (step < 1) step = 1;
        int moved = 0;
        p->vx = p->vy = 0;
        p->on_ground = 0;

        if (jump_edge) {                    /* hop off the ladder */
            p->state = FA_PST_JUMP;
            p->vy = p->character ? t->jump_vel_c1 : t->jump_vel;
            p->jump_hold = t->jump_hold_ticks;
            p->on_ladder = 0;
            if (left || right)
                p->vx = (left ? -1 : 1) * t->climb_jump_vx;
            finalize(p, jump_raw, swit, fire, 0);
            return;
        }

        if (up) {
            for (int i = 0; i < step; i++) {
                int ny = fa_player_py(p) - 1;
                if (!ladder_near_y(p, ny)) break;   /* reached the vine top */
                if (p->solid_fn && p->solid_fn(px, ny - t->body_h,
                        p->solid_ctx) == FA_SOLID_FULL) break;  /* ceiling */
                p->y -= FA_FIX(1); moved = 1;
            }
        } else if (down) {
            int descended = 0;
            for (int i = 0; i < step; i++) {
                int32_t g = ground_at(p, p->x);
                if (ladder_at(p, px, fa_player_py(p) + 1) ||
                    (p->solid_fn && feet_blocked(p))) {
                    p->y += FA_FIX(1); moved = descended = 1;
                } else if (!p->solid_fn && p->y < g && p->y + FA_FIX(1) >= g) {
                    p->y = g; moved = descended = 1; break;  /* onto the floor */
                } else break;
            }
            if (!descended && !feet_blocked(p) &&
                !(!p->solid_fn && p->y >= ground_at(p, p->x))) {
                p->state = FA_PST_FALL;        /* let go of the vine bottom */
                p->on_ladder = 0;
                finalize(p, jump_raw, swit, fire, 0);
                return;
            }
        }
        int fy = fa_player_py(p) - 1;
        if (left)  { p->facing = FA_FACE_LEFT;
            for (int i = 0; i < step && ladder_at(p, px - i - 1, fy); i++)
                { p->x -= FA_FIX(1); moved = 1; } }
        if (right) { p->facing = FA_FACE_RIGHT;
            for (int i = 0; i < step && ladder_at(p, px + i + 1, fy); i++)
                { p->x += FA_FIX(1); moved = 1; } }
        p->climb_moving = moved;

        px = fa_player_px(p);
        int32_t gnd = ground_at(p, p->x);
        int on_floor = (!p->solid_fn && p->y >= gnd) || feet_blocked(p);
        int has_vine = ladder_near(p) ||
                       ladder_at(p, px, fa_player_py(p)) ||
                       ladder_at(p, px, fa_player_py(p) + 1);
        if (on_floor) {
            if (!p->solid_fn && p->y >= gnd) p->y = gnd;
            p->on_ground = 1;
            p->state = FA_PST_STAND;
            p->on_ladder = 0;
            p->idle_timer = t->idle_delay;
        } else if (!has_vine) {
            p->state = FA_PST_FALL;             /* climbed off the top */
            p->on_ladder = 0;
        } else {
            p->on_ladder = 1;
        }
        finalize(p, jump_raw, swit, fire, 0);
        return;
    }

    /* ---- PUSH (PL-135, Fettalatte only): state 33 is a COMMITTED clip
     * (MILCHSCHNITTE 172..190, ~38 ticks). The exe does NOT test the
     * direction input during the clip - releasing LEFT/RIGHT does not abort
     * it. It exits early only on loss of grounding or the kind-5 probe
     * (0x431CE0) at (body_x +/- 32, body_y - 100). The shove impulse
     * (block vx = +/-7.0) is delivered by the caller on frame 176 (~tick 8);
     * the player is vx-locked and never rides the block. ---- */
    if (p->state == FA_PST_PUSH) {
        p->vx = 0;
        int face = (p->facing == FA_FACE_RIGHT) ? 1 : -1;
        p->push_timer++;
        int lost = !p->on_ground ||
            !pushable_at(p, fa_player_px(p) + face * (t->body_hw + 8),
                         fa_player_py(p) - 100);
        if (lost || p->push_timer >= 38) {
            p->state = FA_PST_STAND;
            p->idle_timer = t->idle_delay;
            p->push_timer = 0;
        }
        finalize(p, jump_raw, swit, fire, 0);
        return;
    }

    /* the throw is a committed animation (PL-100): while it runs the player
     * holds still and faces the way it did on the press - movement, jump,
     * crouch and switch inputs are ignored until it finishes. */
    int in_throw = p->throw_anim > 0;
    if (in_throw) { left = right = up = down = jump = 0; jump_edge = switch_edge = 0; }

    int crouching = down && p->on_ground && !left && !right;

    /* ---- SWAP start: SWITCH edge while standing still on the ground ---- */
    if (switch_edge && p->on_ground && !in_throw &&
        p->state == FA_PST_STAND && p->vx == 0) {
        p->swap_timer = p->character ? t->swap_ticks_c1 : t->swap_ticks;
        p->idle_kind = p->idle_play = 0;
        p->state = FA_PST_SWAP;
        finalize(p, jump_raw, swit, fire, 0);
        return;
    }

    /* ---- CLIMB entry (exe fcn.00417e41 STAND, fcn.0041839d JUMP, ...):
     * UP with a vine within reach overhead - works while standing, running,
     * jumping or falling, since the exe checks IsLadderAtOrNear in all of
     * those states. DOWN also grabs one at the feet (standing atop a vine). */
    if (!in_throw &&
        ((up && ladder_near(p)) ||
         (down && p->on_ground &&
          ladder_at(p, fa_player_px(p), fa_player_py(p) + 1)))) {
        p->state = FA_PST_CLIMB;
        p->vx = p->vy = 0;
        p->on_ground = 0;
        p->on_ladder = 1;
        p->climb_moving = 0;
        p->idle_kind = p->idle_play = 0;
        finalize(p, jump_raw, swit, fire, 0);
        return;
    }

    /* ---- PUSH entry (PL-135): Fettalatte in WALK, on the ground, facing
     * exactly LEFT or RIGHT and holding that same direction, with an active
     * flag-2 pushable box at (body_x +/- 32, body_y - 100). The exe has NO
     * player-velocity requirement. ---- */
    if (p->character == 1 && p->on_ground && !in_throw) {
        int face = (p->facing == FA_FACE_RIGHT) ? 1 : -1;
        if (((face > 0 && right) || (face < 0 && left)) &&
            pushable_at(p, fa_player_px(p) + face * (t->body_hw + 2),
                        fa_player_py(p) - 100)) {
            p->state = FA_PST_PUSH;
            p->vx = 0;
            p->push_timer = 0;
            p->idle_kind = p->idle_play = 0;
            finalize(p, jump_raw, swit, fire, 0);
            return;
        }
    }

    /* horizontal intent */
    int dir = right - left;
    if (dir > 0) p->facing = FA_FACE_RIGHT;
    else if (dir < 0) p->facing = FA_FACE_LEFT;

    /* horizontal (PL-084): on the ground vx is SET to +/-run_speed, no ramp;
     * in the air it accelerates by air_accel toward +/-air_max. */
    if (p->on_ground) {
        int32_t cap = crouching ? t->crouch_max : t->run_speed;
        if (crouching || dir == 0)
            p->vx = approach(p->vx, 0, t->ground_drag);
        else
            p->vx = (int32_t)dir * cap;
    } else if (dir != 0) {
        p->vx = approach(p->vx, (int32_t)dir * t->air_max, t->air_accel);
        if (p->vx >  t->air_max) p->vx =  t->air_max;
        if (p->vx < -t->air_max) p->vx = -t->air_max;
    }

    /* jump: a hold-higher variable jump. On the press edge from the ground,
     * apply the character's jump_vel ONCE and open a jump_hold_ticks window.
     * While JUMP stays held and the player is still rising, gravity is softened
     * (below) - the arc keeps decelerating, so there is no mid-air float. The
     * window closes on release or at the apex. No double jump. */
    if (jump_edge && p->on_ground && !crouching) {
        p->vy = p->character ? t->jump_vel_c1 : t->jump_vel;
        p->jump_hold = t->jump_hold_ticks;
        p->on_ground = 0;
    } else if (p->jump_hold > 0) {
        if (jump && p->vy < 0) p->jump_hold--;
        else                   p->jump_hold = 0;
    }

    /* gravity (PL-087: vy += 0.6 each tick, clamped to +20.0 terminal - the
     * shared fall integrator, float @0x452254 / @0x452250). While the jump
     * hold window is open the softer jump_hold_gravity applies instead. */
    if (!p->on_ground) {
        p->vy += (p->jump_hold > 0) ? t->jump_hold_gravity : t->gravity;
        if (p->vy > FA_FIX(20)) p->vy = FA_FIX(20);   /* terminal */
    }

    /* penguin glide: past the apex, JUMP + a direction caps the descent so
     * the penguin sinks slowly and drifts far. No lift. Character 1 cannot. */
    p->gliding = (!p->on_ground && p->character == 0 && jump && dir != 0 &&
                  p->vy > 0);
    if (p->gliding && p->vy > t->glide_max_vy)
        p->vy = t->glide_max_vy;

    /* integrate + resolve. With a map probe (RRR-44) this is a swept AABB
     * against fa_collide: solid tiles stop every axis, one-way platforms
     * stop a landing only, nothing tunnels at the terminal fall speed.
     * Without one it is the RRR-43 flat floor + optional raised-surface
     * probe. */
    if (p->solid_fn) {
        fa_aabb_body bd;
        bd.x = p->x; bd.y = p->y; bd.vx = p->vx; bd.vy = p->vy;
        bd.hw = p->t.body_hw > 0 ? p->t.body_hw : 1;
        bd.h  = p->t.body_h  > 0 ? p->t.body_h  : 1;
        bd.on_ground = p->on_ground;
        fa_collide_move(&bd, 0, p->solid_fn, p->solid_ctx);
        p->x = bd.x; p->y = bd.y; p->vx = bd.vx; p->vy = bd.vy;
        p->on_ground = bd.on_ground;
        if (bd.hit_down) { p->vy = 0; p->jump_hold = 0; }
        if (bd.hit_up)   p->vy = 0;
    } else {
        p->x += p->vx;
        p->y += p->vy;
        int32_t g = ground_at(p, p->x);
        if (p->y >= g) {
            p->y = g;
            p->vy = 0;
            p->on_ground = 1;
            p->jump_hold = 0;
        } else if (p->y < g && p->on_ground && p->vy >= 0) {
            /* walked off a ledge */
            p->on_ground = 0;
        }
    }

    /* throw: on the press edge (ground, not crouching, not already throwing)
     * start the locked animation; the snowball leaves the hand throw_release
     * ticks in, not now (PL-100). */
    if (p->throw_timer > 0) p->throw_timer--;
    if (p->throw_anim > 0) {
        int release = p->character ? t->throw_release_c1 : t->throw_release;
        p->throw_anim--;
        if (p->throw_total - p->throw_anim == release)
            spawn_snowball(p, p->throw_up);
        if (p->throw_anim == 0) p->throw_timer = t->throw_cooldown;
    } else if (fire_edge && p->throw_timer == 0 && p->on_ground && !crouching) {
        p->throw_total = p->character ? t->throw_ticks_c1 : t->throw_ticks;
        p->throw_anim  = p->throw_total;
        p->throw_up    = (in & FA_PI_UP) != 0;
    }
    step_snowballs(p);

    /* resolve the animation state */
    if (!p->on_ground)
        p->state = (p->vy < 0) ? FA_PST_JUMP : FA_PST_FALL;
    else if (crouching)
        p->state = FA_PST_CROUCH;
    else if (p->vx > FA_FIX(1) / 8 || p->vx < -(FA_FIX(1) / 8))
        p->state = FA_PST_WALK;
    else
        p->state = FA_PST_STAND;

    /* idle (PL-101): only while standing still. idle_timer counts down; at 0
     * roll rng%3 -> 0 nothing / 1 idle A / 2 idle B, then it plays once for
     * idle_play ticks. The penguin has both A and B; Fettalatte only B. */
    if (p->state == FA_PST_STAND && !in_throw) {
        if (p->idle_play > 0) {
            if (--p->idle_play == 0) p->idle_kind = 0;
        } else if (p->idle_timer > 0) {
            p->idle_timer--;
        } else {
            p->idle_timer = t->idle_repeat;
            unsigned r = prng(p) % 3u;
            if (r == 0) {
                p->idle_kind = 0;
            } else if (p->character) {           /* Fettalatte: 137..159 */
                p->idle_kind = 2; p->idle_play = 46;
            } else if (r == 1) {                 /* penguin A: 65..69 */
                p->idle_kind = 1; p->idle_play = 10;
            } else {                             /* penguin B: 91..115 */
                p->idle_kind = 2; p->idle_play = 50;
            }
        }
    } else {
        p->idle_timer = t->idle_delay;
        p->idle_kind = p->idle_play = 0;
    }

    finalize(p, jump_raw, swit, fire, in_throw);
}

int fa_player_px(const fa_player *p) { return (int)(p->x >> 16); }
int fa_player_py(const fa_player *p) { return (int)(p->y >> 16); }

int fa_player_live_snowballs(const fa_player *p)
{
    int n = 0;
    for (int i = 0; i < FA_MAX_SNOWBALLS; i++) n += p->snow[i].alive;
    return n;
}

const char *fa_player_state_name(fa_player_state s)
{
    switch (s) {
    case FA_PST_STAND:  return "stand";
    case FA_PST_WALK:   return "walk";
    case FA_PST_CROUCH: return "crouch";
    case FA_PST_JUMP:   return "jump";
    case FA_PST_FALL:   return "fall";
    case FA_PST_CLIMB:  return "climb";
    case FA_PST_PUSH:   return "push";
    case FA_PST_SWAP:   return "swap";
    default:            return "?";
    }
}

void fa_player_jump_probe_run(const fa_player_tuning *t, uint32_t hold_input,
                              int hold_ticks, int max_ticks,
                              fa_player_jump_probe *out)
{
    fa_player p;
    fa_player_init(&p, 0, 480);
    if (t) p.t = *t;
    p.y = p.t.floor_y;

    memset(out, 0, sizeof *out);
    int32_t start_y = p.y;
    int lifted = 0, peak_up = 0;

    for (int i = 0; i < max_ticks; i++) {
        uint32_t in = (i < hold_ticks) ? hold_input
                                       : (hold_input & ~(uint32_t)FA_PI_JUMP);
        fa_player_tick(&p, in);

        if (!p.on_ground) {
            if (!lifted) { lifted = 1; out->air_ticks = 0; }
            out->air_ticks++;
            int up = (int)((start_y - p.y) >> 16);
            if (up > peak_up) { peak_up = up; out->apex_tick = out->air_ticks; }
        } else if (lifted) {
            break;   /* landed */
        }
    }
    out->apex_height_px = peak_up;
    out->distance_px = (int)(p.x >> 16);
}
