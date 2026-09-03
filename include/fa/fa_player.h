/*
 * fa_player.h - the player controller (RRR-43)
 *
 * Walk, crouch, jump, throw and character switch, as a fixed-point state
 * machine that runs one step per 60 Hz simulation tick (fa_loop.h). All state
 * that affects behaviour is integer 16.16 fixed point - no float in the
 * simulation (ENGINE-ARCH section 3.4 determinism contract).
 *
 * PARITY STATUS
 *   The movement model and most constants are lifted from JR_FERRERO.exe
 *   fcn.00417370 (state machine) + fcn.00431bf0 (integrator), PL-083/084:
 *     - run/walk speed is SET to +/-5.0 px/tick on the ground (no accel ramp)
 *     - air horizontal accel is +/-1.2 px/tick^2, clamped to 5.0
 *     - the jump applies vy = jump_vel once; while JUMP stays held and the
 *       player is still rising, gravity is softened (jump_hold_gravity) for up
 *       to jump_hold_ticks - a hold-higher variable jump that still arcs, so
 *       there is no mid-air float. Release or the apex ends the assist. No
 *       double jump. jump_vel is per-character: character 0 (penguin) jumps
 *       higher than character 1 (Fettalatte). PL-084 read the original as a
 *       per-tick impulse re-apply; that pinned velocity and floated, so the
 *       owner retuned it to the softer-gravity form.
 *     - GLIDE (penguin only): once past the jump apex (vy > 0), holding JUMP
 *       plus a direction caps the descent at glide_max_vy - the penguin sinks
 *       slowly and drifts far. No height gain. Releasing JUMP or the
 *       direction, or landing, ends it. Character 1 cannot glide.
 *   GRAVITY = 0.6 px/tick^2, TERMINAL fall = 20.0 px/tick (PL-087): the engine
 *   integrates entity position as float and does `vy += 0.6` each tick clamped
 *   to +20.0 in a shared fall step (`fadd ds:0x452254` in ~18 entity state
 *   handlers; clamp const ds:0x452250 = 20.0f). This is a world-global
 *   constant, so the player uses it too; 0.6 replaces the earlier 0.5 guess.
 *   A corpus C07 oracle jump-arc pass would still be the owner-side check -
 *   but the Wine oracle FREEZES once in a level (no dynamic capture), so the
 *   binary constants are the only usable source.
 *   fa_player_jump_probe reports the apex tick / height / airborne ticks.
 *
 * COLLISION (RRR-44)
 *   With fa_player_set_solid the controller resolves movement against the
 *   map through fa_collide.h (swept AABB, box = body_hw x body_h, one pixel
 *   per sub-step, no tunnelling at the terminal fall speed). Solid tiles
 *   stop the box on every side; one-way platforms (collision code & 2) stop
 *   a downward landing only, and DOWN held drops through them.
 *   With no solid probe the module falls back to the flat floor at
 *   tuning.floor_y plus the optional ground probe (fa_player_set_ground) for
 *   a raised surface Y under a world X - the RRR-43 behaviour, kept for the
 *   headless jump-arc tuning aid and the no-GData tests.
 */
#ifndef FA_PLAYER_H
#define FA_PLAYER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FA_FIX_ONE   65536
#define FA_FIX(n)    ((int32_t)((n) * FA_FIX_ONE))

/* Input bitmask the controller reads. Matches (1u << fa_action) from
 * fa_input.h, so fa_app's latched action mask feeds straight in. */
enum {
    FA_PI_LEFT   = 1u << 0,
    FA_PI_RIGHT  = 1u << 1,
    FA_PI_UP     = 1u << 2,
    FA_PI_DOWN   = 1u << 3,
    FA_PI_JUMP   = 1u << 4,   /* A */
    FA_PI_FIRE   = 1u << 5,   /* S - throw a snowball */
    FA_PI_ACTION = 1u << 6,   /* F */
    FA_PI_SWITCH = 1u << 7    /* D - swap the active kid (tentative bind) */
};

typedef enum {
    FA_PST_STAND = 0,
    FA_PST_WALK,
    FA_PST_CROUCH,
    FA_PST_JUMP,      /* moving up */
    FA_PST_FALL,      /* moving down / off a ledge */
    FA_PST_CLIMB,     /* on a ladder (PL-102)                    */
    FA_PST_PUSH,      /* Fettalatte shoving an object (PL-103)   */
    FA_PST_SWAP       /* character swap in progress (PL-104)     */
} fa_player_state;

typedef enum { FA_FACE_LEFT = -1, FA_FACE_RIGHT = 1 } fa_facing;

typedef struct fa_player_tuning {
    int32_t gravity;        /* + added to vy each tick (PL-087: 0.6)          */
    int32_t jump_vel;       /* - vy impulse on jump, character 0 (the penguin)*/
    int32_t jump_vel_c1;    /* - vy impulse on jump, character 1 (Fettalatte) */
    int32_t jump_hold_gravity;/* softer gravity while JUMP is held and rising */
    int      jump_hold_ticks;/* max ticks the softer gravity lasts            */
    int32_t run_speed;      /* vx SET to +/-this on the ground (PL-084: 5.0)  */
    int32_t ground_drag;    /* + per tick toward 0 when no L/R on ground      */
    int32_t air_accel;      /* + per tick toward air_max in the air (1.2)     */
    int32_t air_max;        /* air horizontal clamp (5.0)                     */
    int32_t crouch_max;     /* horizontal cap while crouching                 */
    int32_t floor_y;        /* flat-floor Y when no ground probe is set       */
    int32_t glide_max_vy;   /* descent cap while the penguin glides (slow)    */

    int32_t climb_speed;    /* px/tick on a ladder, any direction (PL-102: 3) */
    int32_t climb_jump_vx;  /* vx when JUMP leaves a ladder with a dir held (5)*/
    int      idle_delay;    /* stand ticks before the first idle roll (300)   */
    int      idle_repeat;   /* stand ticks between later idle rolls (240)     */
    /*
     * The swap is locked for the outgoing kid's voice line (PL-104): penguin
     * -> Fettalatte on pi0020.wav (~94 ticks), Fettalatte -> penguin on
     * ms0013.wav (~105 ticks). Fettalatte then plays a turn-back
     * (MILCHSCHNITTE.W01 150..159) for swap_end_c1 more ticks before the
     * character actually changes; the penguin has no turn-back.
     */
    int      swap_ticks;    /* lock length when character 0 (penguin) leaves  */
    int      swap_ticks_c1; /* lock length when character 1 leaves (voice + end)*/
    int      swap_end_c1;   /* trailing ticks of the c1 swap that are the turn-back */
    int32_t  push_obj_vx;   /* vx pushed onto a heavy object (PL-103: 7.0)    */

    int      throw_cooldown;    /* min extra ticks between throws            */
    /*
     * The throw is a locked animation: the player commits for throw_ticks and
     * the snowball leaves the hand throw_release ticks in, not on the press
     * (PL-100). The exe plays PINGUIN.W01 233..260 (ball spawns on frame 255)
     * and MILCHSCHNITTE.W01 273..296 (spawn 291) at 2 ticks/frame.
     */
    int      throw_ticks;       /* total throw lock, character 0 (penguin)   */
    int      throw_ticks_c1;    /* total throw lock, character 1 (Fettalatte)*/
    int      throw_release;     /* ticks from start to spawn, character 0    */
    int      throw_release_c1;  /* ticks from start to spawn, character 1    */
    int32_t  snow_vx;           /* forward throw: + speed, signed by facing  */
    int32_t  snow_vy;           /* forward throw: - initial vy               */
    int32_t  snow_vx_up;        /* up throw (UP held + Fire): + speed        */
    int32_t  snow_vy_up;        /* up throw: - initial vy                    */
    int32_t  snow_gravity;      /* + per tick (PL-086: 1.0)                  */
    int32_t  snow_term_vy;      /* terminal vy clamp (PL-086: 20.0)          */
    int      snow_life;         /* ticks before it despawns                  */
    int32_t  snow_off_x, snow_off_y;   /* spawn offset from the player       */

    /* collision box for fa_player_set_solid (RRR-44 / PL-109). First pass -
     * no exe symbol pins it; owner-tunable like the physics constants. */
    int      body_hw;          /* half-width in whole pixels                 */
    int      body_h;           /* full height in whole pixels (feet at y)    */
} fa_player_tuning;

/* The documented first-pass constants (see the header note). */
extern const fa_player_tuning FA_PLAYER_DEFAULT_TUNING;

#define FA_MAX_SNOWBALLS 10   /* PL-085: the original pool is 10 */

typedef struct fa_snowball {
    int      alive;
    int32_t  x, y, vx, vy;   /* 16.16 */
    int      age;            /* ticks */
    fa_facing dir;
} fa_snowball;

typedef struct fa_player {
    fa_player_tuning t;

    int32_t x, y;            /* 16.16 world position, feet at (x, y) */
    int32_t vx, vy;          /* 16.16 per tick */
    fa_player_state state;
    fa_facing facing;
    int      on_ground;
    int      character;      /* 0 or 1 - the active kid */
    int      gliding;        /* 1 while the penguin glide is active this tick */

    int      jump_hold;      /* ticks left in the hold-higher jump window */
    int      jump_held_prev;
    int      switch_held_prev;
    int      fire_held_prev;
    int      throw_timer;    /* extra cooldown ticks after a throw */
    int      throw_anim;     /* ticks LEFT in the current throw (0 = not throwing) */
    int      throw_total;    /* this throw's full length, for the spawn timing */
    int      throw_up;       /* the UP modifier latched when the throw started */

    /* idle (PL-101): a sub-mode of STAND, not a state */
    int      idle_timer;     /* stand ticks until the next idle roll        */
    int      idle_kind;      /* 0 none, 1 idle A, 2 idle B - for the pose   */
    int      idle_play;      /* ticks the current idle clip has left        */
    int      idle_sound;     /* 0/1 sub-roll for the idle voice line        */
    int      in_boss;        /* 1 in a boss arena (exe 0x4DABD4 >= 4)       */

    /* character swap (PL-104) */
    int      swap_timer;     /* > 0 = swap running, all input locked        */

    /* push (PL-135): state 33 is a committed 172..190 clip, ~38 ticks; the
     * shove impulse leaves on frame 176 (~tick 8). Not aborted by releasing
     * the direction - only by losing the ground / the pushable probe. */
    int      push_timer;

    /* climb (PL-102) */
    int      on_ladder;      /* 1 while FA_PST_CLIMB and a ladder is present */
    int      climb_moving;   /* 1 if the player moved on the ladder this tick*/

    fa_snowball snow[FA_MAX_SNOWBALLS];

    int32_t (*ground_fn)(int32_t x, void *ctx);   /* NULL = flat floor */
    void   *ground_ctx;
    /* map collision probe (RRR-44): the solidity class at world pixel
     * (px, py) - 0 none / 1 solid / 2 one-way. NULL = flat-floor fallback. */
    int    (*solid_fn)(int px, int py, void *ctx);
    void   *solid_ctx;
    /* ladder probe: 1 if a ladder tile covers world pixel (px,py). NULL = none. */
    int    (*ladder_fn)(int px, int py, void *ctx);
    void   *ladder_ctx;
    /* pushable probe: 1 if a heavy object sits at world pixel (px,py) (RRR-50).
     * NULL = nothing pushable, so Fettalatte never enters FA_PST_PUSH. */
    int    (*pushable_fn)(int px, int py, void *ctx);
    void   *pushable_ctx;

    uint32_t rng_state;      /* xorshift32 - idle rolls (stand-in for RRR-52) */
    uint64_t tick;
    uint32_t hash;           /* rolling FNV-1a-32 over every tick's state */
} fa_player;

/* Ground probe: return the solid surface Y (16.16) at world X `x`, or
 * INT32_MAX for "no ground here". `ctx` is the pointer from fa_player_set_ground. */
typedef int32_t (*fa_ground_fn)(int32_t x, void *ctx);

/* Ladder probe: return 1 if a ladder tile covers the world pixel (px, py). */
typedef int (*fa_ladder_fn)(int px, int py, void *ctx);

/* Initialise at (spawn_x, spawn_y) in whole pixels, facing right, standing.
 * Uses FA_PLAYER_DEFAULT_TUNING; change p->t afterwards to retune. */
void fa_player_init(fa_player *p, int spawn_x, int spawn_y);

/* Mark the player as inside (1) or outside (0) a boss arena. The exe's idle
 * roll (0x417C95 / 0x418EA3) reads world index 0x4DABD4: >= 4 (a boss stage)
 * forces the penguin to idle B and suppresses Fettalatte's idle entirely. */
void fa_player_set_boss_arena(fa_player *p, int in_boss);

void fa_player_set_ground(fa_player *p, fa_ground_fn fn, void *ctx);
void fa_player_set_ladder(fa_player *p, fa_ladder_fn fn, void *ctx);
void fa_player_set_pushable(fa_player *p, fa_ladder_fn fn, void *ctx);

/* Map collision (RRR-44). `fn` returns the solidity class at a world pixel
 * (0 none / 1 solid / 2 one-way); pass fa_map_solid_class bound to the
 * level. NULL restores the flat-floor + ground-probe fallback. */
void fa_player_set_solid(fa_player *p, int (*fn)(int px, int py, void *ctx),
                         void *ctx);

/* One 60 Hz simulation tick. `input` is the action bitmask (FA_PI_*). */
void fa_player_tick(fa_player *p, uint32_t input);

/* --- queries -------------------------------------------------------- */

int  fa_player_px(const fa_player *p);   /* whole-pixel X */
int  fa_player_py(const fa_player *p);   /* whole-pixel Y */
int  fa_player_live_snowballs(const fa_player *p);
const char *fa_player_state_name(fa_player_state s);

/* --- owner tuning aid (C07) --------------------------------------- */

typedef struct fa_player_jump_probe {
    int  apex_tick;      /* ticks from lift-off to the highest point */
    int  apex_height_px; /* peak height above the start, whole pixels */
    int  air_ticks;      /* total ticks off the ground */
    int  distance_px;    /* horizontal travel over the jump */
} fa_player_jump_probe;

/*
 * Run a jump in isolation on a flat floor: stand, then hold `hold_input`
 * (typically FA_PI_JUMP | FA_PI_RIGHT) for `hold_ticks`, then release JUMP,
 * until the player lands again or `max_ticks` elapse. Fills `out`. Uses the
 * tuning in `t` (NULL = the defaults). Does not touch `p`.
 */
void fa_player_jump_probe_run(const fa_player_tuning *t, uint32_t hold_input,
                              int hold_ticks, int max_ticks,
                              fa_player_jump_probe *out);

#ifdef __cplusplus
}
#endif

#endif /* FA_PLAYER_H */
