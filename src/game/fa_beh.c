/*
 * fa_beh.c - per-ObjNr enemy behaviour layer (RRR-51). See fa_beh.h.
 *
 * Rewritten from the raw disassembly (jr_disasm.txt) after the first
 * parametrised pass diverged from the oracle. Every constant and state
 * transition below is traced to a handler in the exe:
 *
 *   papagei  0x415FF0   adler   0x40A700   biene   0x40BBA0   (dive family)
 *   kong     0x413CB0   kl_yeti 0x41B8B0   schneemann 0x41AA50
 *   eier_rob 0x40FEB0   baer    0x40B1F0                      (throw family)
 *   kl_rob   0x413450   (charge)   flugrob 0x4106C0 (2-axis)
 *   schlange 0x41B3E0   kl_krake 0x412F70   (stationary contact)
 *   yeti/gorilla/roboter/krake bosses 0x40E350/0x40C4E0/0x40D6B0/0x40CD70
 *
 * The shared updater 0x4335A0 advances rec[0x0e] within [rec[0x0a], rec[0x0c]]
 * once every (base_delay + rec[0x10]) ticks (base_delay = level[0x1735] = 0).
 * Each handler init sets rec[0x10] itself: 1 for the flyers, 3 for the ground
 * enemies. fa_entity_tick reproduces that timer; this module sets
 * anim_extra_delay per ObjNr so the cadence matches.
 *
 * Positions/velocities here are 16.16 fixed point (the exe uses float; the
 * determinism contract forbids float in the sim). Enemy bodies do not probe
 * terrain - they turn around at the record's stored min/max X only.
 */
#include "fa/fa_beh.h"
#include "fa/fa_entity.h"
#include "fa/fa_aom.h"
#include "fa/fa_player.h"
#include "fa/fa_rng.h"

#include <stdlib.h>
#include <string.h>

/* --- 16.16 fixed point --------------------------------------------- */
#define FX      65536
#define FX_2_0  131072
#define FX_3_0  196608
#define FX_6_0  393216
#define FX_11_0 720896
#define FX_12_0 786432
#define FX_1_0  65536
#define FX_0_2  13107     /* 0.2  (block friction, 0x4522A0)  */
#define FX_0_3  19661     /* 0.3  */
#define FX_0_03 1966      /* 0.03 (coconut flat-throw gravity, 0x40C883)   */
#define FX_0_07 4588      /* 0.07 (octopus flat milk-particle grav, 0x40D0BF) */
#define FX_0_5  32768     /* 0.5  (flat-coconut reflect vy, 0x40C331)      */
#define FX_9_0  589824    /* 9.0  (reflected coconut speed, 0x40C318)      */
#define FX_0_4  26214     /* 0.4  (RRR-61 pipe-drop gravity, 0x452294)  */
#define FX_0_6  39322     /* 0.6  (gravity, PL-087)  */
#define FX_7_0  458752    /* 7.0  (shove impulse, PL-135)     */
#define FX_20_0 1310720   /* terminal fall (PL-087 / 0x452250) */
/* FX_2_0 (block gravity, 0x452288) is defined above. */

static int fx_round(int32_t v)
{
    return v >= 0 ? (int)((v + FX / 2) >> 16)
                  : -(int)(((-v) + FX / 2) >> 16);
}

/* --- per-record behaviour scratch (fa_entity_rec.bs[12]) ----------- */
enum {
    BS_LS = 0,   /* logic state: rec[0x62] in the exe.
                  *   1  ROAM / patrol
                  *   2  READY / IDLE (kong family only)
                  *  10  ATTACK (dive / throw / charge)
                  * 100  hit: play Freeze in place
                  * 101  launched death
                  * 200  spent - waiting for the anim wrap that removes it
                  *   5  snowman frozen (thawing when it flips to mode 2)     */
    BS_COOL,     /* rec[0x74] attack cooldown                                */
    BS_RT,       /* rec[0x78] roam / idle phase timer                        */
    BS_FX,       /* float X, 16.16                                           */
    BS_FY,       /* float Y, 16.16                                           */
    BS_VX,       /* float vx, 16.16                                          */
    BS_VY,       /* float vy, 16.16                                          */
    BS_N,        /* boss HP / throw-burst loop count                         */
    BS_KT,       /* launched-death / freeze timer                            */
    BS_AF,       /* per-loop "threw this loop already" latch / misc          */
    BS_INIT,     /* 0 until the init state has run                           */
    BS_RNG       /* reserved (was a per-record RNG; RRR-52 moved the draws to
                  * the shared fa_rng stream, mirroring the exe's one stream) */
};

/* --- enemy descriptor -------------------------------------------- */
enum { K_DIVE, K_THROW, K_CHARGE, K_STAND, K_FLYROBOT, K_BOSS };

typedef struct {
    short obj_nr;
    unsigned char kind;
    unsigned char cycle;     /* 1 = ROAM/READY alternation (kong family)  */
    unsigned char andelay;   /* rec[0x10] anim_extra_delay                */
    unsigned char patrol;    /* 1 = the body moves horizontally           */
    short vx;                /* patrol speed, whole px/tick               */
    short bx, by, bw, bh;    /* contact / vuln box rel (x,y); bw 0 = sprite */
    short dcx, rng, ylo, yhi;/* attack gate                               */
    short rel;               /* throw release: sheet frame                */
    short tdx, tdy, tspan;   /* throw origin (facing left) + right span   */
    short tvx, tvy;          /* projectile velocity, whole px/tick        */
    short hp;                /* boss                                      */
} edesc;

/* Constants below are from the handler disassembly (see the file header). */
static const edesc DESC[] = {
/*obj kind     cyc del pat  vx   bx  by  bw   bh  dcx rng ylo yhi rel tdx tdy tspan tvx tvy hp */
{ 3,K_DIVE,   0,1,1,  2,   0,  0, 60, 100,  40,180,300,500,  0,  0,  0,  0,   0,  0,  0},/*papagei*/
{ 4,K_STAND,  0,1,0,  0,   5,  5, 25, 130,   0,  0,  0,  0,  0,  0,  0,  0,   0,  0,  0},/*schlange*/
{ 5,K_THROW,  1,3,1,  3,  40, 20,120, 190, 107,600, 19,419, 26,-72, 76,330, 11, -3,  0},/*kong*/
{ 6,K_DIVE,   0,1,1,  2,   0,130,180,  77,  90,180,300,500,  0,  0,  0,  0,   0,  0,  0},/*adler*/
{ 7,K_THROW,  1,3,1,  3,  40, 20, 40, 159,  50,600,-11,389, 19,-52, 56,240, 11, -3,  0},/*kl_yeti*/
{ 8,K_THROW,  1,1,1,  3,   8,  8, 48, 110,  65,600, 18,418, 32,-22, 66,176, 11, -3,  0},/*schneemann*/
{ 9,K_BOSS,   0,3,0,  0,  20, 20,134, 262,   0,  0,  0,  0,  0,  0,  0,  0,   0,  0,  9},/*yeti boss (andelay 3: exe 0x40E3B9)*/
{10,K_BOSS,   0,3,0,  0,  70, 20,110, 262,   0,  0,  0,  0,  0,  0,  0,  0,   0,  0,  9},/*gorilla boss (andelay 3: exe 0x40C527)*/
{11,K_CHARGE, 0,3,1,  3,  30, 10, 60, 198,  45,600,  8,408,  0,  0,  0,  0,   0,  0,  0},/*kl_roboter*/
{12,K_THROW,  1,3,1,  3,  20, 20, 72, 155,  56,600, -5,395, 14,-17, 53,145, 11,  0,  0},/*eier_roboter*/
{13,K_FLYROBOT,0,1,0, 0,   0, 40,  0,   0,   0,  0,  0,  0,  0,  0,  0,  0,   0,  0,  0},/*flugroboter*/
{14,K_BOSS,   0,3,0,  0,  20, 20,130, 262,   0,  0,  0,  0,  0,  0,  0,  0,   0,  0,  9},/*roboter boss (andelay 3: exe 0x40D6F7)*/
{15,K_THROW,  1,3,1,  3,  10, 10, 68, 205, 107,600, 19,419, 47,-72, 76,245, 11, -3,  0},/*baer*/
{16,K_DIVE,   0,1,1,  2,  10, 30, 68,  70,  44,180,300,500,  0,  0,  0,  0,   0,  0,  0},/*biene*/
{17,K_STAND,  0,3,1,  3,  40,  0, 70,  90,   0,  0,  0,  0,  0,  0,  0,  0,   0,  0,  0},/*kl_krake*/
{18,K_BOSS,   0,3,0,  0,  30, 10,170, 175,   0,  0,  0,  0,  0,  0,  0,  0,   0,  1,  9},/*krake/octopus boss (andelay 3: exe 0x40CDA1; tvy=1 marks the direct-hit path)*/
};
#define DESC_COUNT ((int)(sizeof DESC / sizeof DESC[0]))

static const edesc *desc_for(int obj_nr)
{
    for (int i = 0; i < DESC_COUNT; i++)
        if (DESC[i].obj_nr == obj_nr) return &DESC[i];
    return NULL;
}

/* --- enemy-owned projectile pool -------------------------------- */
#define FA_BEH_MAX_PROJ 24
typedef struct {
    int     alive;
    int32_t x, y, vx, vy;   /* 16.16 */
    int     grav;           /* 16.16 per tick (kong shots have none)     */
    int     life;
    int     owner_obj;      /* the throwing enemy's ObjNr - the exe draws
                             * the projectile from that sheet (0x40AF80)  */
    int     reflected;      /* RRR-59: a player snowball turned this boss
                             * coconut back toward the boss (0x40C2C0)     */
} bproj;

struct fa_beh {
    fa_entity_store *store;
    fa_beh_hooks     h;

    int  px, py, phw, ph, pface, pif;   /* player snapshot (begin_frame)  */
    struct fa_snowball *snow;
    int  snow_max;

    bproj proj[FA_BEH_MAX_PROJ];
    int   boss_hp;
    int   boss_defeated;               /* RRR-59: the gorilla KO has fired    */
    int   recipe_done;                 /* RRR-59: the 7th piece (i7) was taken */
    int   world;                       /* 1..4, for the Paradiso level cue  */
    int   pchar;                       /* active character 0/1 (0x4E1020)   */
    int   ammo_dirty;                  /* 1 = "dirty" (black) snowballs, ds:0x4E1044.
                                        * RRR-60: only these hurt the yeti boss. */
    int   ib_phase;                    /* RRR-60 yeti<->ice-block handshake, ds:0x4E0B24:
                                        * 1 block ready, 2 kick fired, 3 slide done, 0 rearm */
    int   icicle_mask;                 /* RRR-60 yeti->icicle trigger bitfield, ds:0x4E0B20 */

    /* RRR-61 World-3 robot boss: 3 buttons (ObjNr 83) on the left of the
     * arena; push all 3 -> the pipe (ObjNr 85) drops onto the robot -> hit,
     * then the buttons reset. ds:0x4E0B2C is the exe's 3-byte flag array. */
    int   rb_btn[3];                   /* 1 = that button is pushed and held  */
    int   rb_pipe;                     /* 0 parked, 1 dropping                 */

    fa_rng rng;                        /* RRR-52: the shared enemy RNG stream */

    int   pending_dmg, pending_kb;
};

/* ---- helpers --------------------------------------------------- */

/*
 * RRR-52: every random enemy timer draws from the behaviour layer's single
 * fa_rng stream (the exe's rand(), fcn @0x4395B0), reduced with `% n` exactly
 * as the handlers do. `brand(b, n)` == the exe's `rand() % n`.
 */
static int brand(fa_beh *b, int n)
{
    return fa_rng_below(&b->rng, n);
}

/* RRR-59: the gorilla boss voice lines (0x40C4E0 / 0x40CC48 -> strings at
 * 0x455E28..0x455D74). GB0002 intro/roar, GB0003 alt roar, GB0008..GB0011
 * the four random on-hit taunts. Streamed on the Paradiso voice lane. */
#define GB_INTRO "SDat/voices/ita/GB0002.wav"
static const char *GB_ROAR[2]  = { "SDat/voices/ita/GB0002.wav",
                                   "SDat/voices/ita/GB0003.wav" };
static const char *GB_TAUNT[4] = { "SDat/voices/ita/GB0008.wav",
                                   "SDat/voices/ita/GB0009.wav",
                                   "SDat/voices/ita/GB0010.wav",
                                   "SDat/voices/ita/GB0011.wav" };

/* RRR-60: the World-2 yeti boss voice lines (0x40E350 -> strings at
 * 0x4561D0 / 0x456188 / 0x4561AC / 0x456164..0x4560D4). YB0001 intro,
 * YB0002/3 the two roars, YB0004..YB0008 the five on-hit taunts. */
#define YB_INTRO "SDat/voices/ita/YB0001.wav"
static const char *YB_ROAR[2]  = { "SDat/voices/ita/YB0002.wav",
                                   "SDat/voices/ita/YB0003.wav" };
static const char *YB_TAUNT[5] = { "SDat/voices/ita/YB0004.wav",
                                   "SDat/voices/ita/YB0005.wav",
                                   "SDat/voices/ita/YB0006.wav",
                                   "SDat/voices/ita/YB0007.wav",
                                   "SDat/voices/ita/YB0008.wav" };

/* RRR-61: the World-3 robot boss voice lines (0x40D6B0 -> strings at
 * 0x45608C / 0x456068 / 0x456044 / 0x456020 / 0x455FFC / 0x455FD8 /
 * 0x455FB4 / 0x455F90). rb0003 intro; rb0011/2/8/1/13 the taunt roll
 * (state 40); rb0010 (state 41); rb0004 the on-hit reaction. */
#define RB_INTRO "SDat/voices/ita/rb0003.wav"
#define RB_STATE41 "SDat/voices/ita/rb0010.wav"
#define RB_HIT   "SDat/voices/ita/rb0004.wav"
static const char *RB_TAUNT[5] = { "SDat/voices/ita/rb0011.wav",
                                   "SDat/voices/ita/rb0002.wav",
                                   "SDat/voices/ita/rb0008.wav",
                                   "SDat/voices/ita/rb0001.wav",
                                   "SDat/voices/ita/rb0013.wav" };

/* RRR-62: the World-4 octopus (KRAKE) boss voice lines (0x40CD70 -> strings at
 * 0x455F6C / 0x455F48 / 0x455F24 / 0x455EB8 / 0x455EDC / 0x455F00 / 0x455E70 /
 * 0x455E94 / 0x455E4C). ob0001 intro; ob0004 a snowball registered; ob0002/3
 * the on-hit taunts (state 101); ob0007 the calm shoot-end line; ob0008/9/13
 * the shoot-end lines when the kid shot back; ob0006 defeat. */
#define OB_INTRO  "SDat/voices/ita/ob0001.wav"
#define OB_HIT    "SDat/voices/ita/ob0004.wav"
#define OB_DEFEAT "SDat/voices/ita/ob0006.wav"
/* shoot-end when the kid did NOT shoot back: rand%2 over ob0007 / ob0013
 * (exe 0x40D122..0x40D166). */
static const char *OB_CALM[2] = { "SDat/voices/ita/ob0007.wav",
                                  "SDat/voices/ita/ob0013.wav" };
static const char *OB_HITTAUNT[2] = { "SDat/voices/ita/ob0002.wav",
                                      "SDat/voices/ita/ob0003.wav" };
static const char *OB_SHOTAT[3]   = { "SDat/voices/ita/ob0008.wav",
                                      "SDat/voices/ita/ob0009.wav",
                                      "SDat/voices/ita/ob0013.wav" };

/* RRR-60: when the yeti lands from a hop it sets ds:0x4E0B20 to one of these
 * 6-bit patterns (exe table 0x4560B0, rand()%9); every ceiling icicle (ObjNr
 * 79) whose rec[+0x2A] bit is set then falls. */
static const int ICICLE_PAT[9] =
    { 0x07,0x38,0x15,0x2f,0x1f,0x37,0x3b,0x3d,0x3e };

static int rec_index(const fa_beh *b, const fa_entity_rec *e)
{
    const fa_entity_rec *base = fa_entity_at(b->store, 0);
    return base ? (int)(e - base) : 0;
}

static int overlap(int ax0,int ay0,int ax1,int ay1,int bx0,int by0,int bx1,int by1)
{
    return !(ax1 < bx0 || ax0 > bx1 || ay1 < by0 || ay0 > by1);
}

/* RRR-61: the robot boss aim lane. The exe (0x40D7B7) buckets the player's
 * SCREEN x at 0x120 / 0x240, but the owner prefers our own read: the lane is
 * the wall button (ObjNr 83) whose frame-box centre Y is nearest the kid's
 * feet. Buttons are stacked (idx 0 low .. 2 high), so the bolt - through the
 * fixed exe vy table {+10, 0, -8} - tracks the button the kid stands at. */
static int rb_lane(const fa_beh *b)
{
    int best = 1, bestd = -1;
    int c = fa_entity_count(b->store);
    for (int i = 0; i < c; i++) {
        const fa_entity_rec *e = fa_entity_at(b->store, i);
        if (!e || !e->active || e->obj_nr != 83) continue;
        int x0, y0, x1, y1, cy = e->y;
        if (fa_entity_frame_box((fa_entity_store *)b->store, i,
                                &x0, &y0, &x1, &y1) == 0)
            cy = (y0 + y1) / 2;
        int d = cy - b->py;  if (d < 0) d = -d;
        int idx = (unsigned char)e->raw[0x2a] & 3;  if (idx > 2) idx = 0;
        if (bestd < 0 || d < bestd) { bestd = d; best = idx; }
    }
    return best;
}

/* RRR-59: the live boss record (ObjNr 9/10/14/18), or NULL. */
static fa_entity_rec *boss_rec(fa_beh *b)
{
    int c = fa_entity_count(b->store);
    for (int i = 0; i < c; i++) {
        fa_entity_rec *e = fa_entity_at_mut(b->store, i);
        if (!e || !e->active) continue;
        if (e->obj_nr == 9 || e->obj_nr == 10 || e->obj_nr == 14 ||
            e->obj_nr == 18)
            return e;
    }
    return NULL;
}

static void player_box(const fa_beh *b, int *x0,int *y0,int *x1,int *y1)
{
    *x0 = b->px - b->phw;  *x1 = b->px + b->phw;
    *y0 = b->py - b->ph;   *y1 = b->py;
}

/*
 * The enemy's contact / vulnerability box = the FULL rendered sprite AABB
 * (RRR-51 owner playtest: "the hitbox should be the entire sprite"). The
 * disassembly's tighter per-enemy sub-boxes (kong X+40,Y+20,120,190 etc.)
 * are not what the oracle shows on screen. fa_entity_frame_box is now the
 * exe-exact placed-object box (frame-0-normalised table A + ReferenceSprWidth
 * mirror; see ent_frame_box), so the sprite and this box coincide.
 */
static void enemy_box(fa_beh *b, int idx, const fa_entity_rec *e,
                      const edesc *d, int *x0,int *y0,int *x1,int *y1)
{
    if (fa_entity_frame_box(b->store, idx, x0, y0, x1, y1) != 0) {
        int fw = 40, fh = 80;                 /* headless fallback */
        *x0 = e->x - fw; *x1 = e->x + fw;
        *y0 = e->y - fh; *y1 = e->y;
    }

    /* Flying enemies (dive family + flying robot): on the attack / dive
     * frames the per-frame sprite AABB balloons well past the body (wings and
     * beak spread, the dive pose stretches), so a raw-AABB contact box drifts
     * off the visible enemy - the "disjointed hitbox" the owner reported.
     * Intersect the AABB with the exe's fixed class box, anchored at the
     * record origin (enemy-render-and-hit-disasm.md Q3). The result can never
     * be larger than the drawn sprite and never detaches from it, in every
     * state including the attack. */
    if (d && (d->kind == K_DIVE || d->kind == K_FLYROBOT)) {
        int cx0 = e->x + d->bx;
        int cy0 = e->y + d->by;
        int cx1 = d->bw > 0 ? cx0 + d->bw : *x1;
        int cy1 = d->bh > 0 ? cy0 + d->bh : *y1;
        if (cx0 < *x0) cx0 = *x0;
        if (cy0 < *y0) cy0 = *y0;
        if (cx1 > *x1) cx1 = *x1;
        if (cy1 > *y1) cy1 = *y1;
        if (cx1 > cx0 && cy1 > cy0) {
            *x0 = cx0; *y0 = cy0; *x1 = cx1; *y1 = cy1;
        }
    }
}

/* pick an AOM range: which = 0 move[LEFT] (Walk) / 16 Attack / 17 Freeze / 18 KO */
static const fa_aom_range *aom_range(fa_beh *b, const fa_entity_rec *e, int which)
{
    const fa_aom_def *def = fa_entity_def(b->store, e->obj_nr);
    if (!def) return NULL;
    switch (which) {
        case 16: return &def->attack;
        case 17: return &def->freeze;
        case 18: return &def->ko;
        default: return &def->move[FA_DIR_LEFT];
    }
}

/* 0x430B20: load [start,end] into the anim range, reset the frame. */
static void set_state(fa_beh *b, fa_entity_rec *e, int which, int mode)
{
    const fa_aom_range *r = aom_range(b, e, which);
    if (!r || !r->set) return;
    e->anim_first = r->start;
    e->anim_last  = r->end < r->start ? r->start : r->end;
    e->anim_mode  = mode;
    e->frame      = (mode == 2) ? e->anim_last : e->anim_first;
    e->anim_timer = e->anim_extra_delay;
}

/* fire one enemy projectile (0x40ADC0 constructor + 0x413B70 callback). */
static void spawn_proj(fa_beh *b, int owner_obj, int wx, int wy,
                       int vx, int vy, int grav, int life)
{
    for (int i = 0; i < FA_BEH_MAX_PROJ; i++) {
        if (b->proj[i].alive) continue;
        b->proj[i].alive = 1;
        b->proj[i].x = (int32_t)wx << 16;  b->proj[i].y = (int32_t)wy << 16;
        b->proj[i].vx = (int32_t)vx << 16; b->proj[i].vy = (int32_t)vy << 16;
        b->proj[i].grav = grav;
        b->proj[i].life = life;
        b->proj[i].owner_obj = owner_obj;
        b->proj[i].reflected = 0;          /* RRR-59: clear a reused slot     */
        if (b->h.sfx) b->h.sfx(FA_BEH_SFX_ENEMY_SHOT, owner_obj, b->h.user);
        return;
    }
}

/* consume the first player snowball whose point lands in the box; 1 = hit. */
static int snow_hit(fa_beh *b, int x0,int y0,int x1,int y1)
{
    if (!b->snow) return 0;
    for (int k = 0; k < b->snow_max; k++) {
        struct fa_snowball *s = &b->snow[k];
        if (!s->alive) continue;
        int sx = s->x >> 16, sy = s->y >> 16;
        if (sx < x0 || sx > x1 || sy < y0 || sy > y1) continue;
        s->alive = 0;
        return 1;
    }
    return 0;
}

/* 0x41A3E0: contact -> 20 damage, unless i-frames are up. */
static void touch_player(fa_beh *b, int ex0,int ey0,int ex1,int ey1)
{
    if (b->pif != 0) return;
    int qx0,qy0,qx1,qy1;
    player_box(b, &qx0,&qy0,&qx1,&qy1);
    if (overlap(ex0,ey0,ex1,ey1, qx0,qy0,qx1,qy1)) {
        b->pending_dmg = 20;
        b->pending_kb  = 1;
    }
}

/* the shared KO award (0x4164xx): Freeze in place, then the launch. */
static void enemy_ko(fa_beh *b, fa_entity_rec *e)
{
    e->anim_extra_delay = 1;                /* rec[0x10] = 1 on KO          */
    set_state(b, e, 17, 0);                 /* state 0x11 = Freeze          */
    e->bs[BS_LS] = 100;
    e->bs[BS_KT] = 120;                     /* rec[0x74] fallback timer     */
    e->automove = 0;
    e->collision_enabled = 0;
    if (b->h.score) b->h.score(100, b->h.user);
    if (b->h.sfx)   b->h.sfx(FA_BEH_SFX_ENEMY_KO, e->obj_nr, b->h.user);
}

/* patrol one axis: X += vx (16.16), reverse at the record's min/max X. */
static void patrol_x(fa_entity_rec *e, int lo, int hi)
{
    e->bs[BS_FX] += e->bs[BS_VX];
    int nx = fx_round(e->bs[BS_FX]);
    if (e->bs[BS_VX] > 0 && nx >= hi) { nx = hi; e->bs[BS_FX] = (int32_t)hi << 16; e->bs[BS_VX] = -e->bs[BS_VX]; }
    if (e->bs[BS_VX] < 0 && nx <= lo) { nx = lo; e->bs[BS_FX] = (int32_t)lo << 16; e->bs[BS_VX] = -e->bs[BS_VX]; }
    e->x = nx;
    e->flip_x = e->bs[BS_VX] > 0;           /* rec[0x2b] = (vx > 0)         */
}

static int patrol_lo(const fa_entity_rec *e)
{ return e->min_x != -1 ? e->min_x : (int)(e->bs[BS_FX] >> 16) - 200; }
static int patrol_hi(const fa_entity_rec *e)
{ return e->max_x != -1 ? e->max_x : (int)(e->bs[BS_FX] >> 16) + 200; }

/*
 * The attack gate, traced rec-relative to the exe (parrot 0x4161C0.. /
 * kong 0x413EC5..). NOT sprite-box-relative - the exe compares the player
 * against rec.X/rec.Y plus fixed offsets, so the anchor fix does not move it.
 *
 *   dive  (parrot): player well below (rec.Y + [ylo,yhi] = [+300,+500]) AND
 *                   player X inside the patrol span (rec[0x1F]..rec[0x23]).
 *   throw (kong):   |player_x - rec.X - dcx| < rng, on the facing side of
 *                   rec.X + dcx, AND player_y in rec.Y + [ylo,yhi].
 *
 * Cooldown rec[0x74] zero and the rec[0x2A] bit-0 flag clear, as in the exe.
 */
static int attack_gate(fa_beh *b, fa_entity_rec *e, const edesc *d)
{
    if (e->bs[BS_COOL] != 0) return 0;
    if ((e->raw[0x2a] & 1) != 0) return 0;

    if (d->kind == K_DIVE) {
        if (b->py <= e->y + d->ylo || b->py >= e->y + d->yhi) return 0;
        int lo = patrol_lo(e), hi = patrol_hi(e);
        if (b->px < lo || b->px > hi) return 0;
        int dx = b->px - e->x;
        return e->flip_x ? (dx > 0) : (dx < 0);
    }

    int cx  = e->x + d->dcx;                 /* range centre = rec.X + 0x6b   */
    int hdx = b->px - cx;
    int side = e->flip_x ? (hdx > 0) : (hdx < 0);
    if (!side) return 0;
    if ((hdx < 0 ? -hdx : hdx) >= d->rng) return 0;
    if (b->py <= e->y + d->ylo || b->py >= e->y + d->yhi) return 0;
    return 1;
}

/* ================================================================
 * pushable block  (fcn.00414E50, ObjNr 76/78/86/87 - PL-135)
 * ================================================================ */

static const int BLOCK_OBJ[] = { 76, 78, 86, 87 };

/* sparse leading-edge terrain test: samples along `span` at stride 32 plus
 * the far endpoint (0x414D70 vertical / 0x414DE0 horizontal). */
static int edge_solid(fa_beh *b, int ax, int ay, int dx, int dy, int span)
{
    if (!b->h.terrain) return 0;
    if (b->h.terrain(ax + dx * (span - 1), ay + dy * (span - 1), b->h.user) == 1)
        return 1;
    for (int k = 0; k * 32 < span; k++)
        if (b->h.terrain(ax + dx * (k * 32), ay + dy * (k * 32), b->h.user) == 1)
            return 1;
    return 0;
}

static int beh_block(fa_entity_rec *e, int wrapped, void *ctx)
{
    (void)wrapped;
    fa_beh *b = (fa_beh *)ctx;
    int idx = rec_index(b, e);
    int x0, y0, x1, y1;
    if (fa_entity_frame_box(b->store, idx, &x0, &y0, &x1, &y1) != 0) return 0;
    int fw = x1 - x0, fh = y1 - y0;
    if (fw < 2 || fh < 2) return 0;

    if (!e->bs[BS_INIT]) {
        e->bs[BS_INIT] = 1;
        e->bs[BS_FX] = (int32_t)e->x << 16;
        e->bs[BS_FY] = (int32_t)e->y << 16;
        e->bs[BS_VX] = e->bs[BS_VY] = 0;
        e->bs[BS_KT] = 0;                    /* rec[0x74] fall-scan counter  */
        e->dx = e->dy = 0;
        return 0;
    }
    int old_x = e->x, old_y = e->y;

    /* friction toward 0 by 0.2 BEFORE integration, clamp on the sign cross */
    if (e->bs[BS_VX] > 0) { e->bs[BS_VX] -= FX_0_2; if (e->bs[BS_VX] < 0) e->bs[BS_VX] = 0; }
    else if (e->bs[BS_VX] < 0) { e->bs[BS_VX] += FX_0_2; if (e->bs[BS_VX] > 0) e->bs[BS_VX] = 0; }

    /* gravity += 2.0, terminal 20.0 */
    e->bs[BS_VY] += FX_2_0;
    if (e->bs[BS_VY] > FX_20_0) e->bs[BS_VY] = FX_20_0;

    /* --- swept vertical, one integer Y per step (0x414F28..0x414FB5) --- */
    {
        int cy = fx_round(e->bs[BS_FY]);
        int ty = fx_round(e->bs[BS_FY] + e->bs[BS_VY]);
        int landed = 0;
        while (cy < ty) {
            /* sparse bottom-edge test at candidateY + H */
            if (edge_solid(b, x0, cy + fh, 1, 0, fw - 1)) {
                if (e->bs[BS_KT] < 32) { e->bs[BS_VY] = 0; landed = 1; break; }
                e->bs[BS_KT]++;             /* >= 32: ignore the surface     */
            } else {
                e->bs[BS_KT]++;            /* one iteration per crossed px   */
            }
            cy++;
        }
        (void)landed;
        e->bs[BS_FY] = (int32_t)cy << 16;
        e->y = cy;
        y0 = cy; y1 = cy + fh;
    }
    if (e->bs[BS_KT] > 1000) { e->active = 0; return 0; }   /* lost in a pit */

    /* --- swept horizontal, one integer X per step (0x415000..0x415118) --- */
    if (e->bs[BS_VX] != 0) {
        int step = e->bs[BS_VX] > 0 ? 1 : -1;
        int cx = fx_round(e->bs[BS_FX]);
        int tx = fx_round(e->bs[BS_FX] + e->bs[BS_VX]);
        while (cx != tx) {
            int nxt = cx + step;
            int lead = step > 0 ? (nxt + fw) : nxt;
            if (edge_solid(b, lead, y0, 0, 1, fh - 1)) { e->bs[BS_VX] = 0; break; }
            cx = nxt;
        }
        e->bs[BS_FX] = (int32_t)cx << 16;
        e->x = cx;
    }

    /* rec[0xB8]: keep updating off-screen while it still moves (0x4150AA) */
    e->force_offscreen = (e->bs[BS_VX] != 0 || e->bs[BS_VY] != 0);
    e->dx = e->x - old_x;                    /* for the fa_slice push carry  */
    e->dy = e->y - old_y;
    return 0;
}

/* ================================================================
 * Kinder Paradiso  (ObjNr 77, misc_paradiso.w01, DetailGroup 4 - 0x415640)
 * ================================================================
 * NOT an enemy, never blocks or hurts the kid. The placement's rec[+0x2A]
 * byte selects the behaviour (handler 0x415640, state-1 dispatch 0x415B70):
 *
 *   rec[+0x2A] == 10 : a NORMAL level. The mascot is ROPED. Frames from the
 *     contact sheet: hold frame 5 (bound) -> kid approaches -> frames 6..14
 *     ONCE (breaks free) -> loop frames 0..4 (mouth) for the whole world line
 *     PA00NN.wav -> hold frame 0, latched. World line (ds:0x4DABD4):
 *     1 PA0011, 2 PA0014, 3 PA0013, 4 PA0012.
 *
 *   rec[+0x2A] 0..9 : a TUTORIAL level (WeltNt). The mascot is ALREADY FREED
 *     (no rope, no break-free): idle on frame 0, and on approach it loops the
 *     mouth frames 0..4 for a PAT00NN.wav checkpoint line, then holds frame 0.
 *     Every WeltNt places ~9 of these (rec[+0x2A] 0..8, sometimes 9) as you
 *     walk the level. The line per rec[+0x2A] (0x415B70), some multi-part,
 *     6/8 per active character:
 *       0 pat0001,pat0002,pat0005   1 pat0006   2 pat0009,pat0010
 *       3 pat0011   4 pat0012   5 pat0013   6 pat0015 / pat0017
 *       7 pat0020 -> TUTORIAL COMPLETE   8 pat0018 / pat0019   9 pat0014
 *     The rec[+0x2A] == 7 placement sits at the level exit; when its line ends
 *     the exe sets tut.ini[world] = 1 and reloads the world as its normal
 *     level (0x4159BD -> scene 20). fa_beh reports that via hooks.tutorial_done.
 */
static const char *paradiso_cue(int world)
{
    switch (world) {
        case 1: return "SDat/voices/ita/PA0011.wav";
        case 2: return "SDat/voices/ita/PA0014.wav";
        case 3: return "SDat/voices/ita/PA0013.wav";
        case 4: return "SDat/voices/ita/PA0012.wav";
        default: return NULL;
    }
}

/* rec[+0x2A] (0..9) + part index -> the PAT checkpoint line, or NULL past the
 * end of the (usually one-element) sequence. `ch` = active character 0/1. */
static const char *pat_seq(int rec2a, int part, int ch)
{
    switch (rec2a) {
    case 0:
        if (part == 0) return "SDat/voices/ita/pat0001.wav";
        if (part == 1) return "SDat/voices/ita/pat0002.wav";
        if (part == 2) return "SDat/voices/ita/pat0005.wav";
        return NULL;
    case 1: return part == 0 ? "SDat/voices/ita/pat0006.wav" : NULL;
    case 2:
        if (part == 0) return "SDat/voices/ita/pat0009.wav";
        if (part == 1) return "SDat/voices/ita/pat0010.wav";
        return NULL;
    case 3: return part == 0 ? "SDat/voices/ita/pat0011.wav" : NULL;
    case 4: return part == 0 ? "SDat/voices/ita/pat0012.wav" : NULL;
    case 5: return part == 0 ? "SDat/voices/ita/pat0013.wav" : NULL;
    case 6: return part == 0 ? (ch ? "SDat/voices/ita/pat0017.wav"
                                   : "SDat/voices/ita/pat0015.wav") : NULL;
    case 7: return part == 0 ? "SDat/voices/ita/pat0020.wav" : NULL;
    case 8: return part == 0 ? (ch ? "SDat/voices/ita/pat0019.wav"
                                   : "SDat/voices/ita/pat0018.wav") : NULL;
    case 9: return part == 0 ? "SDat/voices/ita/pat0014.wav" : NULL;
    }
    return NULL;
}

enum { PD_WAIT = 0, PD_FREE = 1, PD_CUE = 2, PD_DONE = 3,
       PDT_TALK = 4, PDT_DONE = 5 };

/* set an explicit frame range directly (misc_paradiso needs 5, 6..14 and
 * 0..4 - none is a plain AOM range). */
static void pd_range(fa_entity_rec *e, int first, int last, int mode)
{
    e->anim_first = first;
    e->anim_last  = last < first ? first : last;
    e->anim_mode  = mode;
    e->frame      = first;
    e->anim_timer = e->anim_extra_delay;
}

static int beh_paradiso(fa_entity_rec *e, int wrapped, void *ctx)
{
    (void)wrapped;
    fa_beh *b = (fa_beh *)ctx;
    int idx = rec_index(b, e);
    int tut = ((unsigned char)e->raw[0x2a] != 10);   /* 0..9 = a tutorial one */

    if (!e->bs[BS_INIT]) {
        e->bs[BS_INIT] = 1;
        e->bs[BS_LS]   = PD_WAIT;
        e->bs[BS_AF]   = 0;
        e->automove = 0;
        e->anim_extra_delay = 3;               /* rec[+0x10] = 3 (0x415703)  */
        if (tut) pd_range(e, 0, 0, 0);          /* freed: hold frame 0 (0x11) */
        else     pd_range(e, 5, 5, 0);          /* roped: hold frame 5 (0x0)  */
        return 0;
    }

    switch (e->bs[BS_LS]) {

    case PD_WAIT: {
        int x0, y0, x1, y1;
        if (fa_entity_frame_box(b->store, idx, &x0, &y0, &x1, &y1) != 0) {
            x0 = x1 = e->x; y0 = y1 = e->y;
        }
        /* the exe's tight rec.Y + [-30, +100] band assumes a walk surface
         * near the sprite top; not true for every placement, so trigger on
         * the whole sprite AABB + a wide downward skirt. */
        if (b->px <= x0 - 150 || b->px >= x1 + 150) return 0;
        if (b->py <= y0 - 150 || b->py >= y1 + 250) return 0;

        if (tut) {
            const char *wav = pat_seq((unsigned char)e->raw[0x2a], 0, b->pchar);
            if (wav && b->h.voice) b->h.voice(wav, b->h.user);
            pd_range(e, 0, 4, 1);              /* mouth loop, Attack 0..4     */
            e->bs[BS_AF] = 0;
            e->bs[BS_KT] = 0;
            e->bs[BS_LS] = PDT_TALK;
        } else {
            pd_range(e, 6, 14, 0);            /* break free 6..14 once        */
            e->bs[BS_LS] = PD_FREE;
        }
        return 0;
    }

    case PD_FREE:                               /* normal level: break free   */
        if (e->frame >= e->anim_last) {         /* reached frame 14           */
            const char *wav = paradiso_cue(b->world);
            if (wav && b->h.voice) b->h.voice(wav, b->h.user);
            pd_range(e, 0, 4, 1);               /* mouth loop                 */
            e->bs[BS_KT] = 0;
            e->bs[BS_LS] = PD_CUE;
        }
        return 0;

    case PD_CUE:                                /* normal level: the world line */
        e->bs[BS_KT]++;
        if ((e->bs[BS_KT] > 20 && b->h.voice_busy &&
             !b->h.voice_busy(b->h.user)) || e->bs[BS_KT] > 1200) {
            pd_range(e, 0, 0, 0);               /* hold frame 0, latched      */
            e->bs[BS_LS] = PD_DONE;
        }
        return 0;

    case PDT_TALK: {                            /* tutorial: a PAT line / chain */
        e->bs[BS_KT]++;
        int done = (e->bs[BS_KT] > 20 && b->h.voice_busy &&
                    !b->h.voice_busy(b->h.user)) || e->bs[BS_KT] > 1800;
        if (!done) return 0;
        int next = e->bs[BS_AF] + 1;
        const char *wav = pat_seq((unsigned char)e->raw[0x2a], next, b->pchar);
        if (wav) {                             /* another part of the chain   */
            if (b->h.voice) b->h.voice(wav, b->h.user);
            e->bs[BS_AF] = next;
            e->bs[BS_KT] = 0;
            return 0;
        }
        /* the whole checkpoint line is done */
        if ((unsigned char)e->raw[0x2a] == 7 && b->h.tutorial_done)
            b->h.tutorial_done(b->h.user);      /* -> tut.ini + reload as WeltN */
        pd_range(e, 0, 0, 0);                   /* freed: hold frame 0        */
        e->bs[BS_LS] = PDT_DONE;
        return 0;
    }

    default:                                    /* PD_DONE / PDT_DONE latched  */
        return 0;
    }
}

/* ================================================================
 * Broeselplatform  (ObjNr 414, broeselplatform.w01, DetailGroup 0 - 0x416CB0)
 * ================================================================
 * A crumbling stand-on platform (11 of them in welt4 / the Phantasie world;
 * the only map that places ObjNr 414). Handler 0x416CB0 is a 4-state machine
 * on rec[0x62]:
 *
 *   0 IDLE  : solid, holds frame 0 - state-0 init (0x416CD0) zeroes the anim
 *             range so it does NOT cycle its sheet. The common tail walks the
 *             player list; when the kid stands on the deck (X in
 *             [rec.X, rec.X + w], feet at rec.Y + rec[0x2A]) and the arm latch
 *             rec[0x70] is 0, it sets rec[0x74] = 0x1E, rec[0x70] = 1,
 *             rec[0x98] = 3.0 (a cosmetic pre-break bob) and rec[0x62] = 1.
 *             Stepping off before it commits clears rec[0x70] (0x416E6C).
 *   1 FUSE  : rec[0x74]-- each tick (0x416CEE). At 0: rec[0x1A] = 0 (drop
 *             collision), rec[0x62] = 2, 0x430B20(rec, 0, 1) starts the break
 *             sheet 0..8 once, 0x422B60(6, knusper.wav) is the crumble sfx.
 *   2 BREAK : when the sheet reaches its last frame (0x416D34): rec[0xB9] = 1
 *             (hide), rec[0x74] = 0x78, rec[0x62] = 3.
 *   3 GONE  : rec[0x74]-- (0x416D66). At 0: rec[0x1A] = 1, rec[0xB9] = 0,
 *             rec[0x62] = 0. rec[0x70] is NOT reset here, so if the kid is
 *             still on the tile at regen it will not re-break until he leaves.
 *
 * The pre-break bob (rec[0x94]/rec[0x98]) is cosmetic and needs a per-record
 * render offset the port entity has no field for - not reproduced (same
 * omission as the Kinder Paradiso wobble). Collision is toggled through
 * e->is_lift (fa_entity_solid_at / fa_entity_ride gate on it), so only this
 * ObjNr is affected; e->collision_enabled is kept in step for consistency.
 */
enum { BR_IDLE = 0, BR_FUSE = 1, BR_BREAK = 2, BR_GONE = 3 };

#define BR_FUSE_TICKS   30    /* rec[0x74] = 0x1E */
#define BR_REGEN_TICKS  120   /* rec[0x74] = 0x78 */

static int beh_broesel(fa_entity_rec *e, int wrapped, void *ctx)
{
    fa_beh *b = (fa_beh *)ctx;
    int idx = rec_index(b, e);

    if (!e->bs[BS_INIT]) {
        e->bs[BS_INIT] = 1;
        e->bs[BS_LS]   = BR_IDLE;
        e->bs[BS_AF]   = 0;                 /* rec[0x70] arm latch          */
        e->bs[BS_KT]   = 0;
        e->automove = 0;
        e->anim_extra_delay = 2;            /* rec[0x10] = 2 (0x416CE1)     */
        pd_range(e, 0, 0, 0);               /* idle: hold frame 0, no anim  */
        return 0;
    }

    switch (e->bs[BS_LS]) {

    case BR_IDLE: {
        int x0, y0, x1, y1;
        if (fa_entity_frame_box(b->store, idx, &x0, &y0, &x1, &y1) != 0) {
            /* frame 0 origin is (0,0), align-to-null is a no-op -> the box is
             * exactly [rec.X, rec.X + 216] x [rec.Y, rec.Y + 78]. */
            x0 = e->x; x1 = e->x + 216; y0 = e->y; y1 = e->y + 78;
        }
        int deck = y0 + e->collision_bottom_adjust;
        int on = b->px + b->phw >= x0 && b->px - b->phw <= x1 &&
                 b->py >= deck - FA_ENTITY_RIDE_SLOP &&
                 b->py <= deck + FA_ENTITY_RIDE_SLOP;
        if (on) {
            if (!e->bs[BS_AF]) {
                e->bs[BS_AF] = 1;
                e->bs[BS_KT] = BR_FUSE_TICKS;
                e->bs[BS_LS] = BR_FUSE;
            }
        } else {
            e->bs[BS_AF] = 0;              /* stepped off -> re-armable     */
        }
        return 0;
    }

    case BR_FUSE:
        if (--e->bs[BS_KT] <= 0) {
            e->is_lift = 0;               /* rec[0x1A] = 0: no landing now  */
            e->collision_enabled = 0;
            pd_range(e, 0, 8, 0);         /* 0x430B20(rec,0,1): break 0..8  */
            e->bs[BS_LS] = BR_BREAK;
            if (b->h.sfx)
                b->h.sfx(FA_BEH_SFX_BROESEL_BREAK, e->obj_nr, b->h.user);
        }
        return 0;

    case BR_BREAK:
        if (wrapped) {                     /* the 0..8 sheet finished       */
            e->hidden = 1;                 /* rec[0xB9] = 1                  */
            pd_range(e, 0, 0, 0);
            e->bs[BS_KT] = BR_REGEN_TICKS;
            e->bs[BS_LS] = BR_GONE;
        }
        return 0;

    default:                              /* BR_GONE                        */
        if (--e->bs[BS_KT] <= 0) {
            e->is_lift = 1;               /* rec[0x1A] = 1: solid again     */
            e->collision_enabled = 1;
            e->hidden = 0;                /* rec[0xB9] = 0                  */
            e->bs[BS_LS] = BR_IDLE;
            /* BS_AF (rec[0x70]) left set on purpose: no re-break until the
             * kid steps off and back on (matches 0x416D66). */
        }
        return 0;
    }
}

/*
 * RRR-60: the yeti's in-place hop (exe state 5 / 100, 0x40E64F..0x40E717).
 * On animation frame 88 it launches vy = -12.0; gravity +0.6, terminal +20.0;
 * it lands back on the Y it left from (bs[BS_KT]). It never moves in X. While
 * airborne it holds the last hop frame (the exe syncs the clip to the physics
 * at frames 90/91) so it does not jump-cut back to the ground.
 * Returns 0 airborne, 2 on the exact landing tick, 1 when grounded / it never
 * left the ground. */
static int yeti_hop(fa_entity_rec *e)
{
    if (e->frame == 88 && !e->bs[BS_AF]) {
        e->bs[BS_AF] = 1;
        e->bs[BS_KT] = e->y;                       /* the ground line          */
        e->bs[BS_FY] = (int32_t)e->y << 16;
        e->bs[BS_VY] = -FX_12_0;                   /* 0x40E64F: vy = -12.0     */
    }
    if (!e->bs[BS_AF]) return 1;

    e->bs[BS_VY] += FX_0_6;                        /* ds:0x452254             */
    if (e->bs[BS_VY] > FX_20_0) e->bs[BS_VY] = FX_20_0;
    e->bs[BS_FY] += e->bs[BS_VY];
    int ny = fx_round(e->bs[BS_FY]);
    if (ny >= e->bs[BS_KT]) {                      /* landed                   */
        e->y = e->bs[BS_KT];
        e->bs[BS_VY] = 0;
        e->bs[BS_AF] = 0;
        return 2;                                  /* the landing tick         */
    }
    e->y = ny;
    if (e->frame >= 95) { e->frame = 95; e->anim_first = 95; }  /* hold in air */
    return 0;
}

/* RRR-60: the yeti landed - play the thud (exe 0x40E6BE: ds:0x4E0AA4 on
 * channel 9), trigger the ceiling icicles (0x40E6D4) and re-arm the ice
 * block (0x40E6F6). Fires on every hop landing - the idle hop and the
 * hit-recoil jump both run this block. */
static void yeti_landed(fa_beh *b)
{
    if (b->h.sfx) b->h.sfx(FA_BEH_SFX_BOSS_LAND, 9, b->h.user);
    if (b->icicle_mask == 0)
        b->icicle_mask = ICICLE_PAT[brand(b, 9)];
    if (b->ib_phase == 3)
        b->ib_phase = 0;
}

/* ---- the enemy behaviour callback --------------------------- */

static int beh_enemy(fa_entity_rec *e, int wrapped, void *ctx)
{
    fa_beh *b = (fa_beh *)ctx;
    const edesc *d = desc_for(e->obj_nr);
    if (!d) return 1;
    int idx = rec_index(b, e);

    /* --- init (rec[0x62] == 0) --- */
    if (!e->bs[BS_INIT]) {
        e->bs[BS_INIT] = 1;
        e->bs[BS_FX] = (int32_t)e->x << 16;
        e->bs[BS_FY] = (int32_t)e->y << 16;
        e->bs[BS_VX] = -(int32_t)d->vx << 16;   /* vx = -speed (faces left) */
        e->bs[BS_VY] = 0;
        /* the exe inits rec[+0x78] = rec[+0x74] = 0 (kong 0x413D24, parrot
         * 0x416056): state 1's timer hits 0 on the first tick, so the enemy
         * snaps straight into READY and can attack once in range. The "too
         * aggressive" the owner saw earlier was the flat (gravity-less)
         * projectile, not the pacing - which is fixed now. */
        e->bs[BS_RT]   = 0;
        e->bs[BS_COOL] = 0;
        e->bs[BS_LS] = 1;
        e->bs[BS_N]  = d->hp;
        e->anim_extra_delay = d->andelay;
        e->anim_timer = d->andelay;
        e->automove = 1;
        e->flip_x = 0;
        if (d->kind == K_BOSS) {
            b->boss_hp = d->hp;
            b->boss_defeated = 0;
            e->automove = 0;                 /* the boss holds its ground     */
            e->bs[BS_RT]  = 90 + brand(b, 120);  /* first idle before a throw */
            e->bs[BS_RNG] = 300 + brand(b, 240); /* periodic-roar timer       */
            if (e->obj_nr == 10) {           /* gorilla: intro speech first   */
                e->bs[BS_LS] = 320;
                e->flip_x = 0;               /* faces left, toward the kid    */
                pd_range(e, 47, 51, 1);      /* speech gesture (exe 0x40C921) */
                if (b->h.voice) b->h.voice(GB_INTRO, b->h.user);
            } else if (e->obj_nr == 9) {     /* yeti: intro babble first      */
                e->bs[BS_LS] = 320;
                e->flip_x = 0;
                e->anim_extra_delay = 3;     /* exe 0x40E3B9: rec[0x10] = 3   */
                e->anim_timer = 3;
                e->bs[BS_RT]  = brand(b, 30);       /* timer A: first act      */
                e->bs[BS_RNG] = 40 + brand(b, 180); /* timer B: first roar     */
                e->bs[BS_KT]  = 0;                  /* talk sub-phase (0x40E7E8)*/
                pd_range(e, 38, 46, 0);      /* "Laber gedreht" mouth loop    */
                if (b->h.voice) b->h.voice(YB_INTRO, b->h.user);
                b->icicle_mask = 0;
                b->ib_phase = 0;             /* the block sets it to 1 on land */
            } else if (e->obj_nr == 14) {    /* robot: intro talk first       */
                e->bs[BS_LS] = 320;
                e->flip_x = 0;
                e->anim_extra_delay = 3;     /* exe 0x40D6F7: rec[0x10] = 3   */
                e->anim_timer = 3;
                e->bs[BS_RNG] = 0;           /* intro sub-phase 0            */
                pd_range(e, 172, 176, 0);    /* RBTL02 lead-in 172..176       */
                if (b->h.voice) b->h.voice(RB_INTRO, b->h.user);
            } else if (e->obj_nr == 18) {    /* octopus: intro talk first     */
                e->bs[BS_LS] = 320;
                e->flip_x = 0;
                e->anim_extra_delay = 3;     /* exe 0x40CDA1: rec[0x10] = 3   */
                e->anim_timer = 3;
                pd_range(e, 42, 49, 0);      /* OBTL02 "Talk" 42..49          */
                if (b->h.voice) b->h.voice(OB_INTRO, b->h.user);
            }
        }
        /* the exe body never terrain-probes; the RRR-50 one-time spawn snap
         * stays as a placement fix for floating shipped data. Bosses are
         * placed exactly by the arena data - snapping the tall gorilla sprite
         * pushed it underground (RRR-59). */
        if (b->h.terrain && d->kind != K_DIVE && d->kind != K_FLYROBOT &&
            d->kind != K_BOSS) {
            int x0,y0,x1,y1, foot = e->y + 1;
            if (fa_entity_frame_box(b->store, idx, &x0,&y0,&x1,&y1) == 0) foot = y1;
            int drop = 0;
            while (drop < 224 && b->h.terrain(e->x, foot + drop, b->h.user) == 0) drop++;
            if (drop < 224) { e->y += drop; e->bs[BS_FY] = (int32_t)e->y << 16; }
        }
        if (!(d->kind == K_BOSS &&
              (e->obj_nr == 10 || e->obj_nr == 9 || e->obj_nr == 14 ||
               e->obj_nr == 18)))
            set_state(b, e, 0, 1);          /* Walk, forward (not the bosses -
                                            * they hold their intro pose)     */
        return 0;
    }

    if (e->bs[BS_COOL] > 0) e->bs[BS_COOL]--;
    int lo = patrol_lo(e), hi = patrol_hi(e);

    /* ============ hit / KO phases (0x4164C8 state 100, 0x41655B state 101) ==
     * State 100: the Freeze range plays ONCE in place (automove already
     * cleared), ~16 ticks at the anim delay of 1 the award set. When it
     * reaches its last frame, state 101 begins.
     * State 101: the body is launched - vy -12, +0.6/tick, terminal +20, vx
     * +/-1 - for the FULL 120-tick rec[0x74] timer, with the frame PINNED at
     * the last Freeze frame (the exe re-writes anim_timer=1 every tick at
     * 0x4165E1 so the frame never advances/wraps, so the wrap-driven lifetime
     * removal never fires). It arcs up then falls far off-screen, then is
     * gone. The first pass removed it on the first anim wrap (~2 ticks) -
     * that is the "wrong defeat timing" the owner reported. */
    if (e->bs[BS_LS] == 100) {
        if (e->frame >= e->anim_last) {
            e->bs[BS_LS] = 101;
            e->force_offscreen = 1;         /* rec[0xB8] = 1 (0x4164FE)      */
            e->bs[BS_VX] = (e->flip_x ? FX_1_0 : -FX_1_0);
            e->bs[BS_VY] = -FX_12_0;
            e->bs[BS_KT] = 120;             /* rec[0x74] = 0x78              */
            if (b->h.sfx) b->h.sfx(FA_BEH_SFX_ENEMY_LAUNCH, e->obj_nr, b->h.user);
        }
        return 0;
    }
    if (e->bs[BS_LS] == 101) {
        e->bs[BS_VY] += FX_0_6;
        if (e->bs[BS_VY] > FX_20_0) e->bs[BS_VY] = FX_20_0;
        e->bs[BS_FX] += e->bs[BS_VX];
        e->bs[BS_FY] += e->bs[BS_VY];
        e->x = fx_round(e->bs[BS_FX]);
        e->y = fx_round(e->bs[BS_FY]);
        e->frame = e->anim_last;            /* pin the Freeze end frame      */
        e->anim_timer = 1;
        if (--e->bs[BS_KT] <= 0) e->bs[BS_LS] = 200;   /* -> state 0xC8       */
        return 0;
    }
    if (e->bs[BS_LS] == 200) {
        /* the 120 timer reached 0 last tick; the exe stays visible one more
         * pass, then the anim wrap clears rec[+6] (0x4165C0..0x4165E1). */
        e->frame = e->anim_last;
        e->active = 0;
        return 0;
    }

    /* ===== snowman frozen / thawing (0x41B095 state 100 / 0x41B0FE 101) =====
     * The snowman does NOT die from a snowball - it falls over stiff, holds
     * on the last Freeze frame for ~120 ticks, then plays the Freeze range
     * BACKWARDS to stand up, then resumes. bs[BS_AF]: 0 falling, 1 held,
     * 2 getting up. (Owner: it was looping the fall animation.) */
    if (e->bs[BS_LS] == 5) {
        if (e->bs[BS_AF] == 0) {              /* FALLING: Freeze 0..9 once   */
            if (e->frame >= e->anim_last) {
                e->frame = e->anim_last;
                e->anim_first = e->anim_last; /* collapse -> hold last frame */
                e->bs[BS_AF] = 1;
            }
            return 0;
        }
        if (e->bs[BS_AF] == 1) {              /* HELD DOWN: count rec[0x74]  */
            if (--e->bs[BS_KT] <= 0) {
                set_state(b, e, 17, 2);       /* Freeze reversed -> stand up */
                e->bs[BS_AF] = 2;
            }
            return 0;
        }
        if (e->frame <= e->anim_first) {      /* GETTING UP done -> resume   */
            e->bs[BS_LS] = 1;
            e->bs[BS_AF] = 0;
            e->bs[BS_RT] = 120 + brand(b, 120);
            e->automove = 1;
            e->anim_extra_delay = d->andelay;
            e->bs[BS_VX] = (int32_t)d->vx << 16;   /* exe: vx = +3.0 (0x41B176) */
            e->flip_x = 1;
            set_state(b, e, 0, 1);
        }
        return 0;
    }

    /* ============ boss ============ */
    if (d->kind == K_BOSS) {
        int x0,y0,x1,y1;
        enemy_box(b, idx, e, d, &x0,&y0,&x1,&y1);

        /* --- octopus / KRAKE (Welt4E, ObjNr 18): stationary; talks, then
         * either drinks milk (OBFL, VULNERABLE) or shoots milk particles
         * (OBSH, invincible). 0x41A5E0 flag 1 in the drink state only
         * (0x40CE39) -> a direct snowball there is the ONLY damage; flag 2
         * (bounce) everywhere else. Exe 0x40CD70, hand disasm + Codex.
         * KRAKE.W01 / "Animation von Krake.txt": OBFL 0..10 "Milch Tanken"
         * (flipflop), OBGE 11..19 punch (unused), OBKO 20..26, OBPA 27..35
         * "Hit", OBSH 36..41 "Shoot", OBTL 42..49 "Talk" (loop 46..49),
         * frame 50 = the milk-particle sprite. States (ours -> exe):
         *   320 talk    <- 0/50/51 : OBTL while a voice plays -> decide
         *   360 refuel  <- 1       : OBFL ping-pong; slurp (w4sf01) at fr 7;
         *                            VULNERABLE; done -> shoot
         *   350 shoot   <- 2       : OBSH loop; one milk particle / loop at
         *                            fr 41 (w4sf02); ~50..79 t burst -> talk
         *   300 hit     <- 101     : OBPA once -> --HP; <0 -> defeat else talk
         *   310 dead    <- 110     : OBKO, held --- */
        if (e->obj_nr == 18) {
            int bx0 = e->x + 30,  bx1 = e->x + 200;
            int by0 = e->y + 10,  by1 = e->y + 185;

            /* only the drink state takes a hit (exe 0x40CE25 flag 1) */
            if (e->bs[BS_LS] == 360 && snow_hit(b, bx0,by0,bx1,by1)) {
                e->bs[BS_LS] = 300;
                e->bs[BS_AF] = 0;
                e->anim_extra_delay = 3;
                pd_range(e, 27, 35, 0);            /* OBPA "Hit"             */
                if (b->h.voice) b->h.voice(OB_HIT, b->h.user);
                touch_player(b, bx0,by0,bx1,by1);
                return 0;
            }

            switch (e->bs[BS_LS]) {
            case 310:                              /* defeated: hold OBKO    */
                e->frame = e->anim_last;
                return 0;

            case 320:                              /* OBTL "Talk" 42..49     */
                if (e->frame >= 46) e->anim_first = 46;   /* loop 46..49     */
                if ((!b->h.voice_busy || !b->h.voice_busy(b->h.user)) &&
                    (wrapped || e->frame >= 49)) {
                    if (brand(b, 100) < 50) {            /* -> drink / refuel */
                        e->bs[BS_LS] = 360;
                        e->bs[BS_AF] = 0;
                        e->anim_extra_delay = 3;
                        pd_range(e, 0, 10, 0);            /* OBFL             */
                    } else {                            /* -> shoot          */
                        e->bs[BS_LS]  = 350;
                        e->bs[BS_RT]  = 50 + brand(b, 30);   /* burst ticks  */
                        e->bs[BS_KT]  = brand(b, 2);         /* particle arc */
                        e->bs[BS_AF]  = 0;
                        e->bs[BS_RNG] = 0;                   /* shot-at flag */
                        pd_range(e, 36, 41, 0);             /* OBSH          */
                    }
                }
                touch_player(b, bx0,by0,bx1,by1);
                return 0;

            case 360:                              /* OBFL ping-pong: drink  */
                if (e->frame >= 10)      e->anim_mode = 2;  /* -> reverse    */
                else if (e->frame <= 0)  e->anim_mode = 0;  /* -> forward    */
                if (e->anim_mode == 0 && e->frame >= 7 && !e->bs[BS_AF]) {
                    e->bs[BS_AF] = 1;                       /* slurp, once   */
                    if (b->h.sfx)
                        b->h.sfx(FA_BEH_SFX_BOSS_CHARGE, 18, b->h.user);
                }
                if (e->bs[BS_AF] && e->anim_mode == 0 && e->frame <= 0) {
                    e->bs[BS_LS]  = 350;                    /* -> shoot      */
                    e->bs[BS_RT]  = 50 + brand(b, 30);
                    e->bs[BS_KT]  = brand(b, 2);
                    e->bs[BS_AF]  = 0;
                    e->bs[BS_RNG] = 0;
                    pd_range(e, 36, 41, 0);
                }
                touch_player(b, bx0,by0,bx1,by1);
                return 0;

            case 350: {                            /* OBSH: raise, then fire  */
                if (snow_hit(b, bx0,by0,bx1,by1))
                    e->bs[BS_RNG] = 1;              /* bounced, but noted     */
                /* exe 0x40D046: on OBSH frame 41 it pins the frame
                 * (rec[0x12]=1) and toggles rec[0x78] every tick - so one
                 * milk particle every 2 ticks for the whole rec[0x74] burst,
                 * not one per anim loop. Codex-verified 0x40D04D..0x40D0F6. */
                if (e->frame >= 41) {
                    e->anim_first = 41;            /* pin (anim_last == 41)   */
                    if (!e->bs[BS_AF]) {
                        int lob = e->bs[BS_KT] == 0;
                        /* spawn (bossX-40, bossY+60), vx -10 always;
                         * lob = vy -6 grav 0.2, flat = vy 0 grav 0.07. */
                        spawn_proj(b, 18, e->x - 40, e->y + 60, -10,
                                   lob ? -6 : 0, lob ? FX_0_2 : FX_0_07, 240);
                        e->bs[BS_AF] = 1;
                    } else {
                        e->bs[BS_AF] = 0;
                    }
                }
                if (--e->bs[BS_RT] <= 0) {          /* burst done -> taunt    */
                    if (b->h.voice)
                        b->h.voice(e->bs[BS_RNG] ? OB_SHOTAT[brand(b, 3)]
                                                 : OB_CALM[brand(b, 2)],
                                   b->h.user);
                    e->bs[BS_LS] = 320;
                    pd_range(e, 42, 49, 0);
                }
                touch_player(b, bx0,by0,bx1,by1);
                return 0;
            }

            default:                               /* 300: took a hit (OBPA) */
                if (wrapped) {
                    if (--b->boss_hp < 0) {
                        b->boss_hp = -1;
                        b->boss_defeated = 1;
                        e->bs[BS_LS] = 310;
                        e->collision_enabled = 0;
                        pd_range(e, 20, 26, 0);    /* OBKO                   */
                        if (b->h.score) b->h.score(10000, b->h.user);
                        if (b->h.voice) b->h.voice(OB_DEFEAT, b->h.user);
                    } else {
                        if (b->h.voice)
                            b->h.voice(OB_HITTAUNT[brand(b, 2)], b->h.user);
                        e->bs[BS_LS] = 320;
                        pd_range(e, 42, 49, 0);
                    }
                }
                touch_player(b, bx0,by0,bx1,by1);
                return 0;
            }
        }

        /* --- gorilla (Welt1E, ObjNr 10): stationary; lobs coconuts; hurt
         * only by a coconut a snowball has turned back (see fa_beh_post).
         * State machine traced to 0x40C4E0 (Codex-verified). The gorilla.jrs
         * "Right" range 27..51 is one continuous sheet the exe plays in slices:
         *   47..51 = the speech gesture (mouth), looped while a GB voice plays
         *   27..38 = the chest-beat (exe caps rec[0xc] at 0x26 in 0x40C99D)
         *   38..47 = the throw wind-up (exe sets rec[0xc] = 0x2f in state 2)
         *   33..38 = the tight stationary idle sway (exe holds rec[0xa] at 33)
         * States (ours -> exe):
         *   320 speech  <- 50/51  : loop 47..51, wait for the voice, -> 330
         *   330 chest    <- 0x40C99D tail : 27..38 x2, -> 340
         *   340 idle     <- 1     : hold 33..38; RT -> 350; roar timer -> 320
         *   350 wind-up  <- 2     : 38..47 once, -> 10
         *    10 attack   <- 10    : Attack 72..86, coconut at frame 82
         *                           (0x40C7F5), rec[6] = 3..6 bursts -> 340
         *   300 hit      <- 100/101 : Freeze 53..63, then a GB0008..11 taunt
         *                           speech -> 320 (exe -> state 50)
         *   310 defeated <- 110   : KO 64..71, held. --- */
        if (e->obj_nr == 10) {
            e->flip_x = 0;                    /* always faces the kid (left)  */
            switch (e->bs[BS_LS]) {
            case 310:                        /* defeated: hold the KO frame  */
                e->frame = e->anim_last;
                return 0;

            case 320:                        /* speech: loop 47..51 + voice  */
                if (!b->h.voice_busy || !b->h.voice_busy(b->h.user)) {
                    e->bs[BS_LS] = 330;
                    e->bs[BS_KT] = 0;
                    pd_range(e, 27, 38, 0);            /* -> chest-beat      */
                }
                touch_player(b, x0,y0,x1,y1);
                return 0;

            case 330:                        /* chest-beat: 27..38, twice    */
                if (wrapped && ++e->bs[BS_KT] >= 2) {
                    e->bs[BS_LS]  = 340;
                    e->bs[BS_RT]  = 90 + brand(b, 120);
                    e->bs[BS_RNG] = 300 + brand(b, 300);
                    pd_range(e, 33, 38, 0);            /* -> idle sway       */
                }
                touch_player(b, x0,y0,x1,y1);
                return 0;

            case 350:                        /* wind-up: 38..47 once         */
                if (wrapped) {
                    e->bs[BS_LS] = 10;
                    e->bs[BS_N]  = 3 + brand(b, 4);    /* rec[6] = 3..6      */
                    e->bs[BS_AF] = 0;
                    pd_range(e, 72, 86, 0);            /* Attack range       */
                }
                touch_player(b, x0,y0,x1,y1);
                return 0;

            case 300:                        /* took a hit: Freeze 53..63    */
                if (wrapped) {                /* Freeze finished -> taunt     */
                    if (b->h.voice) b->h.voice(GB_TAUNT[brand(b, 4)], b->h.user);
                    e->bs[BS_LS] = 320;
                    pd_range(e, 47, 51, 1);            /* speech (holds on it) */
                }
                touch_player(b, x0,y0,x1,y1);
                return 0;

            case 10:                         /* attack: throw at anim fr 82  */
                /* the exe fires exactly on Attack frame 0x52 (0x40C7F5); the
                 * coconut leaves the fully-extended arm - spawn rec-relative
                 * rec.X-126 / rec.Y+60, vx ALWAYS -6.0 (no facing branch),
                 * lob variant on rand()%1000 <= 500 (0x40C86D). */
                if (e->frame == 82 && !e->bs[BS_AF]) {
                    int lob = brand(b, 1000) <= 500;
                    spawn_proj(b, 10, e->x - 126, e->y + 60, -6,
                               lob ? -6 : 0, lob ? FX_0_3 : FX_0_03, 240);
                    e->bs[BS_AF] = 1;
                } else if (e->frame != 82) {
                    e->bs[BS_AF] = 0;            /* re-arm for the next loop  */
                }
                if (wrapped && --e->bs[BS_N] <= 0) {
                    e->bs[BS_LS] = 340;
                    e->bs[BS_RT] = 90 + brand(b, 120);
                    pd_range(e, 33, 38, 0);           /* back to idle sway   */
                }
                touch_player(b, x0,y0,x1,y1);
                return 0;

            default:                         /* 340 idle: stationary sway    */
                if (--e->bs[BS_RT] <= 0) {
                    e->bs[BS_LS] = 350;               /* -> wind-up         */
                    pd_range(e, 38, 47, 0);
                } else if (--e->bs[BS_RNG] <= 0 &&
                           (!b->h.voice_busy || !b->h.voice_busy(b->h.user))) {
                    /* periodic roar = a fresh speech + chest-beat pass       */
                    if (b->h.voice) b->h.voice(GB_ROAR[brand(b, 2)], b->h.user);
                    e->bs[BS_LS] = 320;
                    pd_range(e, 47, 51, 1);
                }
                touch_player(b, x0,y0,x1,y1);
                return 0;
            }
        }

        /* --- yeti (Welt2E, ObjNr 9): stationary (never writes X); it holds a
         * frozen pose and periodically kicks / hops / talks. Only a DIRTY
         * ("black") snowball - the one from collect_dirtyballs - hurts it, and
         * only while it is idle or kicking. Exe 0x40E350, hand-disassembled.
         *   0x41A3E0(X+20,Y+20,134,262,20) = 20 contact damage every tick
         *   0x41A5E0(body, flag 1) in states 1 & 3: any ball in the box is
         *     eaten; ret==2 (a non-0x105 = dirty ball) -> hit (-> exe state 100)
         * yeti.jrs frames: KO 0..22, KICK 23..37, "Laber gedreht" (turned talk)
         *   38..52 (mouth loop 42..46), "Laber" 53..67, Jump 68..95. Exe state 1
         *   freezes on frame 38.
         * States (ours -> exe):
         *   320 talk  <- 50/51/52: 38..46, loop 42..46 while the voice runs,
         *                          then 46..52 -> 340; act timer = rand()%30
         *   340 idle  <- 1       : FROZEN on frame 38; RT -> kick/hop;
         *                          RNG -> roar (YB0002/3) -> 320
         *   350 kick  <- 3       : KICK 23..37 -> 340 (still vulnerable);
         *                          act timer on return = rand()%60 + 60
         *   360 hop   <- 5       : Jump 83..95, real leap (vy -12) on frame 88;
         *                          land -> thud + icicles; return = rand()%60+60
         *   300 hit   <- 100/101 : startled jump 68..95, grunt at fr 69, --HP;
         *                          <0 -> defeat, else taunt YB0004..8 -> 320
         *   310 dead  <- 110/0x40EC0B : KO 0..22, then slump off under gravity,
         *                          record removed after 120 ticks --- */
        if (e->obj_nr == 9) {
            int bx0 = e->x + 20, bx1 = e->x + 154;
            int by0 = e->y + 20, by1 = e->y + 282;
            int ls  = e->bs[BS_LS];

            /* idle / kick: eat any ball in the body box; a dirty one also hits */
            if ((ls == 340 || ls == 350) &&
                snow_hit(b, bx0,by0,bx1,by1) && b->ammo_dirty) {
                e->bs[BS_LS] = 300;
                e->bs[BS_AF] = 0;
                e->bs[BS_N]  = 0;                 /* frame-69 grunt latch     */
                pd_range(e, 68, 95, 0);            /* startled full jump      */
                /* exe 0x40E5DB: a hit cuts the roar / taunt voice line.
                 * The hurt grunt plays later, at anim frame 69 (0x40EA0D). */
                if (b->h.sfx) b->h.sfx(FA_BEH_SFX_BOSS_VOICE_CUT, 9, b->h.user);
                touch_player(b, bx0,by0,bx1,by1);
                return 0;
            }

            switch (e->bs[BS_LS]) {
            case 310:                               /* defeated (exe st 110)  */
                /* KO 0..22 plays once; meanwhile the body keeps falling under
                 * gravity (exe 0x40EC65 - it slumps off the ice shelf), and
                 * the record is removed after 120 ticks (0x40EC11 rec[0x74]). */
                if (e->frame >= e->anim_last) {
                    e->frame = e->anim_last;
                    e->anim_first = e->anim_last;
                }
                e->bs[BS_VY] += FX_0_6;
                if (e->bs[BS_VY] > FX_20_0) e->bs[BS_VY] = FX_20_0;
                e->bs[BS_FY] += e->bs[BS_VY];
                e->y = fx_round(e->bs[BS_FY]);
                if (--e->bs[BS_KT] <= 0) e->active = 0;
                return 0;

            case 320: {                             /* talk (exe st 50/51/52) */
                int done = !b->h.voice_busy || !b->h.voice_busy(b->h.user);
                if (e->bs[BS_KT] == 0) {            /* mouth loop while talking */
                    if (done) {
                        e->bs[BS_KT] = 1;
                        pd_range(e, 46, 52, 0);     /* play out the turn       */
                    } else if (wrapped) {
                        e->anim_first = 42;         /* tighten to 42..46 (0x40E94F) */
                    }
                } else if (wrapped || e->frame >= 52) {
                    e->bs[BS_KT]  = 0;
                    e->bs[BS_LS]  = 340;
                    e->bs[BS_RT]  = brand(b, 30);   /* exe 0x40E7E8: rand()%30 */
                    pd_range(e, 38, 38, 0);         /* freeze on frame 38      */
                }
                touch_player(b, bx0,by0,bx1,by1);
                return 0;
            }

            case 350:                               /* KICK 23..37            */
                /* frame 32: fire the floor ice block (exe 0x40E611) */
                if (e->frame >= 32 && !e->bs[BS_AF]) {
                    if (b->ib_phase == 1) b->ib_phase = 2;
                    e->bs[BS_AF] = 1;
                }
                if (wrapped) {
                    e->bs[BS_LS] = 340;
                    e->bs[BS_RT] = 60 + brand(b, 60);
                    pd_range(e, 38, 38, 0);
                }
                touch_player(b, bx0,by0,bx1,by1);
                return 0;

            case 360: {                             /* hop 83..95 + leap      */
                int r = yeti_hop(e);
                if (r == 2) yeti_landed(b);          /* icicles + rearm block  */
                if (r && (wrapped || e->frame >= 95 || e->anim_first >= 95)) {
                    e->bs[BS_KT] = 0;
                    e->bs[BS_LS] = 340;
                    e->bs[BS_RT] = 60 + brand(b, 60);
                    pd_range(e, 38, 38, 0);
                }
                touch_player(b, e->x + 20, e->y + 20, e->x + 154, e->y + 282);
                return 0;
            }

            case 300: {                             /* took a hit (exe st 100) */
                if (e->frame >= 69 && !e->bs[BS_N]) {
                    e->bs[BS_N] = 1;               /* exe 0x40EA0D: grunt fr 69 */
                    if (b->h.sfx) b->h.sfx(FA_BEH_SFX_BOSS_HURT, 9, b->h.user);
                }
                int r = yeti_hop(e);
                if (r == 2) yeti_landed(b);
                if (r && (wrapped || e->frame >= 95 || e->anim_first >= 95)) {
                    e->bs[BS_KT] = 0;
                    if (--b->boss_hp < 0) {
                        b->boss_hp = -1;
                        b->boss_defeated = 1;
                        e->bs[BS_LS] = 310;
                        e->collision_enabled = 0;
                        e->bs[BS_FY] = (int32_t)e->y << 16;  /* slump-off start */
                        e->bs[BS_VY] = 0;
                        e->bs[BS_KT] = 120;        /* exe rec[0x74] = 0x78    */
                        pd_range(e, 0, 22, 0);     /* KO range               */
                        if (b->h.score) b->h.score(10000, b->h.user);
                        if (b->h.sfx)
                            b->h.sfx(FA_BEH_SFX_BOSS_KO, 9, b->h.user);
                        return 0;
                    }
                    if (b->h.voice)
                        b->h.voice(YB_TAUNT[brand(b, 5)], b->h.user);
                    e->bs[BS_LS] = 320;
                    pd_range(e, 38, 46, 0);
                }
                touch_player(b, e->x + 20, e->y + 20, e->x + 154, e->y + 282);
                return 0;
            }

            default:                                /* 340 idle: frozen on 38 */
                if (--e->bs[BS_RNG] <= 0 &&
                    (!b->h.voice_busy || !b->h.voice_busy(b->h.user))) {
                    if (b->h.voice) b->h.voice(YB_ROAR[brand(b, 2)], b->h.user);
                    e->bs[BS_RNG] = 600 + brand(b, 200);
                    e->bs[BS_KT]  = 0;             /* talk sub-phase           */
                    e->bs[BS_LS]  = 320;
                    pd_range(e, 38, 46, 0);
                    touch_player(b, bx0,by0,bx1,by1);
                    return 0;
                }
                if (--e->bs[BS_RT] <= 0) {
                    e->bs[BS_RT]  = 60 + brand(b, 60);
                    e->bs[BS_AF] = 0;
                    if (b->ib_phase == 1) {        /* block ready -> KICK it   */
                        e->bs[BS_LS] = 350;        /* (exe 0x40E536)          */
                        pd_range(e, 23, 37, 0);
                    } else {                       /* else -> hop (icicles)   */
                        e->bs[BS_LS] = 360;
                        pd_range(e, 83, 95, 0);
                    }
                }
                touch_player(b, bx0,by0,bx1,by1);
                return 0;
            }
        }

        /* --- robot (Welt3E, ObjNr 14): stationary; fires bolts (ROBOTER.W01
         * frame 188) in one of 3 lanes (rb_lane: the wall button nearest the
         * kid; the exe buckets screen-x thirds, owner prefers this).
         * NEVER hurt by a snowball - every exe state calls 0x41A5E0(box,
         * flag 2) = bounce. Damage: the kid pushes the 3 buttons (ObjNr 83)
         * on the left of the arena; all 3 down -> the pipe (ObjNr 85) drops
         * onto the robot -> 1 hit, then the buttons reset (see beh_button /
         * beh_pipe; the hit is raised there via boss state 300).
         * Exe 0x40D6B0, hand disasm + PE tables (jump 0x40E280, state byte
         * table 0x40E2B8, lane sub-table 0x40E328). ROBOTER.W01: RBAD01 0..18,
         * RBAD02 19..37, RBGE02 38..48, RBKO02 49..71, RBSA02 88..104 /
         * RBSB02 105..121 / RBSC02 122..138 (lane 0/1/2 shoot),
         * RBTL02 172..187 (talk). States (ours -> exe):
         *   320 talk <- 50/51 ; 325 st41 <- 41 ; 330 breathe <- 60/61 ;
         *   350 shoot <- 10/11 ; 340 taunt roll <- 12/40 ;
         *   300 hit <- 100/101 ; 310 defeated <- 110.
         * Every path to the shoot burst passes through 325 (rb0010) + one
         * Breathe B - the exe's state 41. Loop: talk -> 325 -> breathe ->
         * shoot -> taunt roll -> { talk | breathe -> 325 -> breathe } -> ...
         * bs[BS_KT]: in 325/330 it flags "the breathe after this leads to
         * shoot"; in 350 it is the lane. bs[BS_RNG] = the sub-phase in
         * 320 (0..2 talk) and 350 (0 raise / 1 fire / 2 lower). --- */
        if (e->obj_nr == 14) {
            int bx0 = e->x + 20,  bx1 = e->x + 130;
            int by0 = e->y + 20,  by1 = e->y + 282;

            switch (e->bs[BS_LS]) {
            case 310:                             /* defeated: RBKO 49..71 once */
                if (e->frame >= 56 && !e->bs[BS_AF]) {
                    e->bs[BS_AF] = 1;             /* w3sf04b ch10 (exe 0x40E237) */
                    if (b->h.sfx)
                        b->h.sfx(FA_BEH_SFX_BOSS_KO_ANIM, 14, b->h.user);
                }
                if (wrapped) {                    /* played through -> hold last */
                    e->anim_first = e->anim_last;
                    e->frame = e->anim_last;
                }
                return 0;

            case 320:                             /* talk RBTL02 (exe 50/51)   */
                touch_player(b, bx0,by0,bx1,by1);
                if (e->bs[BS_RNG] == 0) {          /* 172..176 lead-in          */
                    if (wrapped || e->frame >= 176) {
                        e->bs[BS_RNG] = 1;
                        pd_range(e, 178, 181, 0);
                    }
                } else if (e->bs[BS_RNG] == 1) {   /* 178..181 loop while talking */
                    if (!b->h.voice_busy || !b->h.voice_busy(b->h.user)) {
                        e->bs[BS_RNG] = 2;
                        pd_range(e, 182, 187, 0);
                    }
                } else if (wrapped || e->frame >= 187) {
                    e->bs[BS_LS] = 325;          /* -> exe state 41            */
                }
                return 0;

            case 325:                             /* exe 41: rb0010 -> breathe  */
                if (b->h.voice) b->h.voice(RB_STATE41, b->h.user);
                e->bs[BS_KT] = 1;                 /* this breathe -> shoot      */
                e->bs[BS_LS] = 330;
                e->anim_extra_delay = 3;
                pd_range(e, 19, 37, 0);           /* RBAD02 Breathe B          */
                if (b->h.sfx) b->h.sfx(FA_BEH_SFX_BOSS_CHARGE, 14, b->h.user);
                touch_player(b, bx0,by0,bx1,by1);
                return 0;

            case 330:                             /* Breathe B (exe 60/61)     */
                touch_player(b, bx0,by0,bx1,by1);
                if ((!b->h.voice_busy || !b->h.voice_busy(b->h.user)) &&
                    (wrapped || e->frame >= 37)) {
                    if (e->bs[BS_KT]) {           /* -> shoot burst            */
                        e->bs[BS_LS]  = 350;
                        e->bs[BS_RNG] = 0;
                        e->bs[BS_N]   = 7 + brand(b, 10);  /* 7..16 (kept)     */
                        e->bs[BS_AF]  = 0;
                        e->bs[BS_KT]  = rb_lane(b);
                        int f0 = 88 + e->bs[BS_KT] * 17;
                        pd_range(e, f0, f0 + 4, 0);        /* raise            */
                    } else {                     /* exe: 60/61 -> 41 -> 60/61  */
                        e->bs[BS_LS] = 325;
                    }
                }
                return 0;

            case 350: {                            /* shoot (exe 10/11)        */
                /* exe 0x40D824: always fires LEFT (vx -20), no gravity. The
                 * lane follows the player every tick; each lane has a fixed
                 * spawn offset + vy (exe sub-table 0x40E328). Bolt = frame 188.*/
                static const int RB_DX[3] = { -12, -24, -12 };
                static const int RB_DY[3] = { 157,  98,  58 };
                static const int RB_VY[3] = {  10,   0,  -8 };
                int lane = e->bs[BS_KT];
                int f0   = 88 + lane * 17;

                if (e->bs[BS_RNG] == 0) {          /* raise f0..f0+4           */
                    if (wrapped || e->frame >= f0 + 4) {
                        e->bs[BS_RNG] = 1;
                        e->bs[BS_AF]  = 12;
                        pd_range(e, f0 + 5, f0 + 12, 0);
                    }
                } else if (e->bs[BS_RNG] == 1) {   /* loop f0+5..f0+12 + fire   */
                    int nl = rb_lane(b);
                    if (nl != lane) {              /* kid moved -> re-aim pose  */
                        e->bs[BS_KT] = lane = nl;
                        f0 = 88 + lane * 17;
                        pd_range(e, f0 + 5, f0 + 12, 0);
                    }
                    if (--e->bs[BS_AF] <= 0) {
                        e->bs[BS_AF] = 20;                 /* cadence (kept)    */
                        spawn_proj(b, 14, e->x + RB_DX[lane], e->y + RB_DY[lane],
                                   -20, RB_VY[lane], 0, 240);
                        if (--e->bs[BS_N] <= 0) {
                            e->bs[BS_RNG] = 2;
                            pd_range(e, f0 + 13, f0 + 16, 0);  /* lower        */
                        }
                    }
                } else if (wrapped || e->frame >= f0 + 16) {   /* lower done   */
                    e->bs[BS_LS] = 340;
                }
                touch_player(b, bx0,by0,bx1,by1);
                return 0;
            }

            case 340: {                            /* exe 12 wait + 40 roll    */
                int r = brand(b, 5);
                if (b->h.voice) b->h.voice(RB_TAUNT[r], b->h.user);
                e->anim_extra_delay = 3;
                if (r < 2) {                       /* rb0011/rb0002 -> talk    */
                    e->bs[BS_LS]  = 320;
                    e->bs[BS_RNG] = 0;
                    pd_range(e, 172, 176, 0);
                } else {                           /* rb0008/rb0001/rb0013     */
                    e->bs[BS_LS] = 330;           /* -> breathe, then 325     */
                    e->bs[BS_KT] = 0;
                    pd_range(e, 19, 37, 0);
                    if (b->h.sfx)
                        b->h.sfx(FA_BEH_SFX_BOSS_CHARGE, 14, b->h.user);
                }
                touch_player(b, bx0,by0,bx1,by1);
                return 0;
            }

            case 300:                              /* took a hit: RBGE02 (exe 100/101) */
            default:
                b->rb_btn[0] = b->rb_btn[1] = b->rb_btn[2] = 0; /* buttons reset */
                b->rb_pipe = 0;
                if (wrapped) {
                    if (--b->boss_hp < 0) {
                        b->boss_hp = -1;
                        b->boss_defeated = 1;
                        e->bs[BS_LS] = 310;
                        e->bs[BS_AF] = 0;         /* arm the KO-anim sfx latch */
                        e->collision_enabled = 0;
                        e->anim_extra_delay = 3;
                        pd_range(e, 49, 71, 0);    /* RBKO02                  */
                        if (b->h.score) b->h.score(10000, b->h.user);
                        if (b->h.sfx)             /* w3sf04a ch9 (exe 0x40E1D5) */
                            b->h.sfx(FA_BEH_SFX_BOSS_KO, 14, b->h.user);
                    } else {                      /* exe 101: rb0004 -> talk 50 */
                        if (b->h.voice) b->h.voice(RB_HIT, b->h.user);
                        e->bs[BS_LS]  = 320;
                        e->bs[BS_RNG] = 0;
                        e->anim_extra_delay = 3;
                        pd_range(e, 172, 176, 0); /* RBTL02                  */
                    }
                }
                touch_player(b, bx0,by0,bx1,by1);
                return 0;
            }
        }

        touch_player(b, x0,y0,x1,y1);
        return 0;
    }

    /* ============ projectile hit -> one-hit KO (or snowman freeze) ====== */
    {
        int x0,y0,x1,y1;
        enemy_box(b, idx, e, d, &x0,&y0,&x1,&y1);
        if (e->bs[BS_LS] != 100 && e->bs[BS_LS] != 101 &&
            snow_hit(b, x0,y0,x1,y1)) {
            if (d->obj_nr == 8) {          /* snowman: freeze + thaw       */
                e->anim_extra_delay = 1;   /* rec[0x10] = 1 (fast fall)    */
                set_state(b, e, 17, 0);    /* Freeze 0..9 forward, frame 0 */
                e->bs[BS_LS] = 5;
                e->bs[BS_KT] = 120;        /* rec[0x74] hold timer         */
                e->bs[BS_AF] = 0;          /* 0 = falling                  */
                e->automove = 0;
                if (b->h.sfx) b->h.sfx(FA_BEH_SFX_FREEZE, e->obj_nr, b->h.user);
            } else {
                enemy_ko(b, e);
            }
            return 0;
        }
    }

    /* ============ movement + attack ============ */
    switch (d->kind) {

    case K_STAND:                          /* snake (still) / octopus (patrol) */
        if (d->patrol) {
            patrol_x(e, lo, hi);
            /* small octopus ping-pongs its walk anim at both ends (0x413008) */
            if (e->obj_nr == 17 && e->anim_last > e->anim_first) {
                if (e->frame >= e->anim_last)  e->anim_mode = 2;
                if (e->frame <= e->anim_first) e->anim_mode = 0;
            }
        }
        break;

    case K_FLYROBOT: {                     /* 2-axis accel flight          */
        int ylo = e->min_y != -1 ? e->min_y : (int)(e->bs[BS_FY] >> 16) - 96;
        int yhi = e->max_y != -1 ? e->max_y : (int)(e->bs[BS_FY] >> 16) + 96;
        if (e->bs[BS_VX] == 0 && e->bs[BS_VY] == 0) { e->bs[BS_VX] = FX_0_3; e->bs[BS_VY] = FX_0_3; }
        int ax = (e->x <= lo) ? FX_0_3 : (e->x >= hi) ? -FX_0_3 : (e->bs[BS_VX] > 0 ? FX_0_3 : -FX_0_3);
        int ay = (e->y <= ylo) ? FX_0_3 : (e->y >= yhi) ? -FX_0_3 : (e->bs[BS_VY] > 0 ? FX_0_3 : -FX_0_3);
        e->bs[BS_VX] += ax; e->bs[BS_VY] += ay;
        int c5 = 5 * FX;
        if (e->bs[BS_VX] >  c5) e->bs[BS_VX] =  c5;
        if (e->bs[BS_VX] < -c5) e->bs[BS_VX] = -c5;
        if (e->bs[BS_VY] >  c5) e->bs[BS_VY] =  c5;
        if (e->bs[BS_VY] < -c5) e->bs[BS_VY] = -c5;
        e->bs[BS_FX] += e->bs[BS_VX]; e->bs[BS_FY] += e->bs[BS_VY];
        e->x = fx_round(e->bs[BS_FX]); e->y = fx_round(e->bs[BS_FY]);
        e->flip_x = e->bs[BS_VX] > 0;
        break;
    }

    case K_DIVE:
        if (e->bs[BS_LS] == 10) {          /* diving                       */
            e->bs[BS_VY] -= FX_0_3;
            if (e->bs[BS_VY] < -FX_12_0) e->bs[BS_VY] = -FX_12_0;
            e->bs[BS_FX] += e->bs[BS_VX];
            e->bs[BS_FY] += e->bs[BS_VY];
            e->x = fx_round(e->bs[BS_FX]);
            int top = e->min_y != -1 ? e->min_y : (int)(e->bs[BS_FY] >> 16);
            if (fx_round(e->bs[BS_FY]) <= top) {
                e->bs[BS_FY] = (int32_t)top << 16;
                e->y = top;
                e->bs[BS_VY] = 0;
                e->bs[BS_COOL] = 120;
                e->bs[BS_LS] = 1;
                set_state(b, e, 0, 1);
            } else {
                e->y = fx_round(e->bs[BS_FY]);
            }
            /* the dive sound is a CONTACT cue, not a launch telegraph: the exe
             * plays it (0x422B60 id 3) from 0x40C2C0 only when the diver's
             * box overlaps the player and bounces him (0x41A5E0), so it never
             * sounds for an off-screen bird. Fire once per dive. */
            if (!e->bs[BS_AF]) {
                int hx0, hy0, hx1, hy1, qx0, qy0, qx1, qy1;
                enemy_box(b, idx, e, d, &hx0, &hy0, &hx1, &hy1);
                player_box(b, &qx0, &qy0, &qx1, &qy1);
                if (overlap(hx0, hy0, hx1, hy1, qx0, qy0, qx1, qy1)) {
                    e->bs[BS_AF] = 1;
                    if (b->h.sfx)
                        b->h.sfx(FA_BEH_SFX_ENEMY_ATTACK, e->obj_nr, b->h.user);
                }
            }
            /* reverse vx at bounds while diving too */
            if (e->bs[BS_VX] > 0 && e->x >= hi) e->bs[BS_VX] = -e->bs[BS_VX];
            if (e->bs[BS_VX] < 0 && e->x <= lo) e->bs[BS_VX] = -e->bs[BS_VX];
            e->flip_x = e->bs[BS_VX] > 0;
        } else {
            patrol_x(e, lo, hi);
            if (attack_gate(b, e, d)) {
                set_state(b, e, 16, 0);   /* Attack range                 */
                e->bs[BS_VY] = FX_12_0;
                e->bs[BS_LS] = 10;
                e->bs[BS_AF] = 0;         /* arm the once-per-dive sound latch */
            }
        }
        break;

    case K_CHARGE:
        if (e->bs[BS_LS] == 10) {          /* charging                     */
            int spd = FX_6_0;
            if (e->x - lo < 100 || hi - e->x < 100) spd = FX_3_0;
            int dir = e->bs[BS_VX] < 0 ? -1 : 1;
            e->bs[BS_FX] += dir * spd;
            e->x = fx_round(e->bs[BS_FX]);
            if (e->x <= lo || e->x >= hi) {
                if (e->x < lo) { e->x = lo; e->bs[BS_FX] = (int32_t)lo << 16; }
                if (e->x > hi) { e->x = hi; e->bs[BS_FX] = (int32_t)hi << 16; }
                e->bs[BS_VX] = -e->bs[BS_VX];
                e->bs[BS_LS] = 1;
                set_state(b, e, 0, 1);
            }
            if (wrapped) { e->bs[BS_LS] = 1; set_state(b, e, 0, 1); }
        } else {
            patrol_x(e, lo, hi);
            if (attack_gate(b, e, d)) {
                set_state(b, e, 16, 0);
                e->bs[BS_LS] = 10;
                /* charge start: no telegraph sound in the exe */
            }
        }
        break;

    case K_THROW: {
        int can_wrap = e->anim_last > e->anim_first;   /* false without a def */
        if (e->bs[BS_LS] == 10) {          /* attack burst (0x413f12..)    */
            /* release one projectile per loop on the release frame        */
            int at_rel = can_wrap ? (e->frame == d->rel) : (e->bs[BS_KT] == 4);
            if (at_rel && !e->bs[BS_AF]) {
                e->bs[BS_AF] = 1;
                int right = e->flip_x;
                /* exe spawns rec-relative: kong 0x414171 rec.X-0x48 (+330 if
                 * facing right) / rec.Y+0x4c.  DESC tdx/tdy/tspan. */
                int ox = e->x + (right ? d->tspan + d->tdx : d->tdx);
                int oy = e->y + d->tdy;
                int vx = right ? d->tvx : -d->tvx;
                /* 0x413B70: every enemy shot takes gravity +0.3 / terminal 20;
                 * kong/yeti/snow/bear leave with vy -3, the egg with vy 0. */
                spawn_proj(b, e->obj_nr, ox, oy, vx, d->tvy, FX_0_3, 120);
            }
            if (!at_rel) e->bs[BS_AF] = 0;
            e->bs[BS_KT]++;
            int loop_end = can_wrap ? wrapped : (e->bs[BS_KT] >= 20);
            if (loop_end) {
                e->bs[BS_KT] = 0;
                if (--e->bs[BS_N] <= 0) {
                    /* owner: the throw re-attacks too soon.  End the burst
                     * into a full ROAM (not straight back to READY) and hold
                     * a long cooldown so the next throw is a fresh cycle. */
                    e->bs[BS_COOL] = 300;
                    e->bs[BS_LS] = 1;
                    e->bs[BS_RT] = 120 + brand(b, 120);
                    set_state(b, e, 0, 1);
                }
            }
        } else if (e->bs[BS_LS] == 1) {    /* ROAM (state 1, 0x413d50)     */
            patrol_x(e, lo, hi);
            if (--e->bs[BS_RT] <= 0) {
                e->bs[BS_LS] = 2;
                e->bs[BS_RT] = 60 + brand(b, 60);               /* 60..119 */
            }
        } else {                          /* READY (state 2, 0x413E84)     */
            /* the exe keeps patrolling in READY (rec[+0x1C] stays 1 except at
             * walk-frame 15) - it does NOT stand still.  Standing still was
             * the "gets stuck when it changes direction" the owner saw. */
            patrol_x(e, lo, hi);
            if (--e->bs[BS_RT] <= 0) {
                e->bs[BS_LS] = 1;
                e->bs[BS_RT] = 120 + brand(b, 120);     /* 120..239 */
                set_state(b, e, 0, 1);
            } else if ((wrapped || !can_wrap) && attack_gate(b, e, d)) {
                set_state(b, e, 16, 0);   /* 0x430b20(rec, 0x10, 1)        */
                e->bs[BS_LS] = 10;
                /* exe rec[+6] = 1 + rng%10; owner wants a shorter, more
                 * deliberate burst - 1..3 shots. */
                e->bs[BS_N]  = 1 + brand(b, 3);
                e->bs[BS_AF] = 0;
                e->bs[BS_KT] = 0;
                /* no sound on the wind-up: the exe plays the whip per shot
                 * (0x41416A), not on the attack-state entry. */
            }
        }
        break;
    }

    default: break;
    }

    /* ============ contact damage (every non-KO tick) ============ */
    {
        int x0,y0,x1,y1;
        enemy_box(b, idx, e, d, &x0,&y0,&x1,&y1);
        touch_player(b, x0,y0,x1,y1);
    }
    return 0;                              /* the callback owns movement    */
}

/* ================================================================
 * i7 - the 7th recipe piece in the boss arena (ObjNr 59, 0x40F770)
 * ================================================================
 * The exe hides it above the screen (state 0 sets rec.Y = -200), holds until
 * the boss-defeated flag ds:0x4DAB18 is set (state 1), then drops it straight
 * down under gravity 0.6 (state 2). It lands when the SPRITE BOTTOM EDGE meets
 * solid ground - exe 0x40F8E7: 0x434180(plane 2, rec.X + frameW/2,
 * rec.Y + frameH). Checking the record origin instead sinks the whole sprite
 * underground (Welt2E floor is ~90 px below the drop column). Catching it
 * (0x41A3E0 overlap) awards +10000 and ends the level (owner: -> CLASSIFICA).
 */
enum { I7_WAIT, I7_FALL, I7_LAND };

static int beh_i7(fa_entity_rec *e, int wrapped, void *ctx)
{
    (void)wrapped;
    fa_beh *b = (fa_beh *)ctx;

    if (!e->bs[BS_INIT]) {
        e->bs[BS_INIT] = 1;
        e->bs[BS_LS]   = I7_WAIT;
        e->bs[BS_FY]   = (int32_t)e->y << 16;
        e->bs[BS_VY]   = 0;
        e->hidden      = 1;                 /* off-screen until the boss falls */
        e->force_offscreen = 1;             /* keep this callback ticking      */
        e->detail_group = -1;               /* this callback owns collection,
                                             * not the generic pickup path    */
        return 0;
    }

    if (e->bs[BS_LS] == I7_WAIT) {
        if (b->boss_defeated) {
            e->hidden = 0;
            e->bs[BS_LS] = I7_FALL;
        }
        return 0;
    }

    if (e->bs[BS_LS] == I7_FALL) {
        e->bs[BS_VY] += FX_0_6;
        if (e->bs[BS_VY] > FX_20_0) e->bs[BS_VY] = FX_20_0;
        e->bs[BS_FY] += e->bs[BS_VY];
        e->y = fx_round(e->bs[BS_FY]);
        /* exe 0x40F8E7: land when the sprite BOTTOM meets solid, not the
         * record origin (which buries the whole sprite). */
        int px = e->x + 32, py = e->y + 89, bx0, by0, bx1, by1;
        if (fa_entity_frame_box(b->store, rec_index(b, e),
                                &bx0, &by0, &bx1, &by1) == 0) {
            px = (bx0 + bx1) / 2;
            py = by1;
        }
        if (b->h.terrain && b->h.terrain(px, py, b->h.user) == 1) {
            e->bs[BS_VY] = 0;
            e->bs[BS_LS] = I7_LAND;
            e->force_offscreen = 0;
        }
    }

    /* catch test - the whole player box vs a box around the piece */
    {
        int qx0,qy0,qx1,qy1;
        player_box(b, &qx0,&qy0,&qx1,&qy1);
        int hw = 40, hh = 40;
        if (overlap(e->x - hw, e->y - hh, e->x + hw, e->y + hh,
                    qx0,qy0,qx1,qy1)) {
            if (b->h.score) b->h.score(10000, b->h.user);
            b->recipe_done = 1;              /* host plays the jingle + ends   */
            e->active = 0;
        }
    }
    return 0;
}

/* ============================================================ *
 * RRR-60: the World-2 yeti arena hazards.                       *
 * ============================================================ */

/*
 * The floor ice block (ObjNr 265 "Eis_klotz", exe 0x4149F0). It sits on the
 * ground by the yeti; when the yeti kicks (b->ib_phase 1 -> 2) it slides left
 * across the floor toward the kid at 10 px/tick, 20 contact damage, until it
 * is off the left edge (b->ib_phase -> 3); the yeti's next HOP-landing rearms
 * it (b->ib_phase -> 0) - it drops back in from its placed position and, once
 * it hits the floor again, is ready for the next kick (exe state 4 -> state 1).
 *
 * ib_phase (ds:0x4E0B24) becomes 1 ONLY when the block has landed on the floor
 * (exe state 1 -> 0x414B7F). While it is still dropping in, ib_phase stays 0,
 * so the idle yeti hops instead of kicking at a block that is not down yet.
 */
enum { ICB_FALL = 0, ICB_WAIT, ICB_SLIDE, ICB_SPENT };

static void iceblock_drop(fa_entity_rec *e)          /* (re)start the fall */
{
    e->x = e->bs[BS_FX];
    e->y = e->bs[BS_N];
    e->bs[BS_FY] = (int32_t)e->bs[BS_N] << 16;
    e->bs[BS_VY] = 0;
    e->hidden = 0;
    e->bs[BS_LS] = ICB_FALL;
}

static int beh_iceblock(fa_entity_rec *e, int wrapped, void *ctx)
{
    (void)wrapped;
    fa_beh *b = (fa_beh *)ctx;

    if (!e->bs[BS_INIT]) {
        e->bs[BS_INIT] = 1;
        e->bs[BS_FX] = e->x;               /* placed position (exe [+0x74/78]) */
        e->bs[BS_N]  = e->y;
        e->automove = 0;
        e->collision_enabled = 0;
        iceblock_drop(e);                  /* ib_phase set on landing, not here */
        return 0;
    }

    switch (e->bs[BS_LS]) {
    case ICB_FALL:
        e->bs[BS_VY] += FX_0_6;
        if (e->bs[BS_VY] > FX_20_0) e->bs[BS_VY] = FX_20_0;
        e->bs[BS_FY] += e->bs[BS_VY];
        e->y = fx_round(e->bs[BS_FY]);
        if (b->h.terrain && b->h.terrain(e->x, e->y + 20, b->h.user) == 1) {
            e->bs[BS_VY] = 0;
            e->bs[BS_LS] = ICB_WAIT;                    /* -> ready for a kick */
            b->ib_phase = 1;               /* exe 0x414B7F: block is down       */
        }
        break;

    case ICB_WAIT:
        if (b->ib_phase == 2) e->bs[BS_LS] = ICB_SLIDE;
        break;

    case ICB_SLIDE:
        e->x -= 10;                                     /* exe [+2] += 0xFFF6 */
        touch_player(b, e->x - 30, e->y - 40, e->x + 30, e->y + 20);
        if (e->x < -100) {                              /* exe cmp -100      */
            b->ib_phase = 3;
            e->hidden = 1;
            e->bs[BS_LS] = ICB_SPENT;
        }
        break;

    default: /* ICB_SPENT */
        if (b->ib_phase == 0) {                         /* the yeti hopped   */
            iceblock_drop(e);          /* re-fall; ICB_FALL sets ib_phase = 1 */
        }
        break;
    }
    return 0;
}

/*
 * A ceiling icicle (ObjNr 79 "misc_icespikee", exe 0x414740). rec[+0x2A] is its
 * bit in b->icicle_mask; when the yeti's hop-landing sets that bit the icicle
 * shivers ~1-2 s, then falls under gravity (20 contact damage), shatters on the
 * floor, waits ~1 s and returns to the ceiling.
 */
enum { ICI_HANG = 0, ICI_SHIVER, ICI_FALL, ICI_SHATTER };

static int beh_icicle(fa_entity_rec *e, int wrapped, void *ctx)
{
    (void)wrapped;
    fa_beh *b = (fa_beh *)ctx;

    if (!e->bs[BS_INIT]) {
        e->bs[BS_INIT] = 1;
        e->bs[BS_FX] = e->x;                            /* home              */
        e->bs[BS_N]  = e->y;
        e->bs[BS_AF] = (unsigned char)e->raw[0x2a] & 7; /* the trigger bit   */
        e->bs[BS_LS] = ICI_HANG;
        e->automove = 0;
        e->collision_enabled = 0;
        return 0;
    }

    int bit = 1 << e->bs[BS_AF];

    switch (e->bs[BS_LS]) {
    case ICI_HANG:
        if (b->icicle_mask & bit) {
            b->icicle_mask &= ~bit;
            e->bs[BS_LS] = ICI_SHIVER;
            e->bs[BS_KT] = 60 + brand(b, 60);
        }
        break;

    case ICI_SHIVER:
        e->x = e->bs[BS_FX] + brand(b, 3);   /* exe 0x414875: home + rand()%3 */
        e->y = e->bs[BS_N]  + brand(b, 3);
        if (--e->bs[BS_KT] <= 0) {
            e->x = e->bs[BS_FX];
            e->y = e->bs[BS_N];
            e->bs[BS_VY] = 0;
            e->bs[BS_FY] = (int32_t)e->y << 16;
            e->bs[BS_LS] = ICI_FALL;
        }
        break;

    case ICI_FALL:
        e->bs[BS_VY] += FX_0_6;
        if (e->bs[BS_VY] > FX_20_0) e->bs[BS_VY] = FX_20_0;
        e->bs[BS_FY] += e->bs[BS_VY];
        e->y = fx_round(e->bs[BS_FY]);
        touch_player(b, e->x - 10, e->y - 30, e->x + 10, e->y + 10);
        if (b->h.terrain && b->h.terrain(e->x, e->y + 10, b->h.user) == 1) {
            e->bs[BS_KT] = 60;
            e->bs[BS_LS] = ICI_SHATTER;
        }
        break;

    default: /* ICI_SHATTER */
        if (--e->bs[BS_KT] <= 0) {
            e->x = e->bs[BS_FX];
            e->y = e->bs[BS_N];
            e->bs[BS_LS] = ICI_HANG;
        }
        break;
    }
    return 0;
}

/*
 * The ice platform the yeti stands on (ObjNr 80 "misc_abbruch", exe 0x414C90).
 * While the yeti lives it is FROZEN on frame 0 (the exe pins rec[+0x12]=1 every
 * tick, state 1). The moment the yeti is defeated (ds:0x4E0B1C, our
 * b->boss_defeated) it plays its 0..28 break animation once and freezes on
 * frame 28 (exe state 2).
 */
enum { ABB_HOLD = 0, ABB_BREAK, ABB_DONE };

static int beh_abbruch(fa_entity_rec *e, int wrapped, void *ctx)
{
    fa_beh *b = (fa_beh *)ctx;

    if (!e->bs[BS_INIT]) {
        e->bs[BS_INIT] = 1;
        e->bs[BS_LS] = ABB_HOLD;
        e->automove = 0;
        pd_range(e, 0, 0, 0);                           /* freeze on frame 0 */
        return 0;
    }

    switch (e->bs[BS_LS]) {
    case ABB_HOLD:
        e->frame = 0;                                   /* keep it pinned    */
        if (b->boss_defeated) {
            pd_range(e, 0, 28, 0);                      /* play the break    */
            e->bs[BS_LS] = ABB_BREAK;
        }
        break;
    case ABB_BREAK:
        if (wrapped || e->frame >= 28) {
            e->frame = 28;
            e->anim_first = e->anim_last = 28;          /* stop animating    */
            e->bs[BS_LS] = ABB_DONE;
        }
        break;
    default:
        e->frame = 28;
        break;
    }
    return 0;
}

/* ================================================================
 * RRR-61: the World-3 (FABBRICA) robot-boss arena mechanism.
 * ================================================================
 * 3 buttons (ObjNr 83, rec[0x2A] = index 0/1/2) sit on the left of the
 * arena, frozen on frame 0 until the kid walks into one. A press plays the
 * button anim + w3sf03 and latches b->rb_btn[index]. ObjNr 84 (also 3, same
 * index) is a reactor that just animates while its button is down. When all
 * 3 are latched the pipe (ObjNr 85) drops from its placed position straight
 * onto the robot; on contact the robot takes one hit (boss state 300) and
 * every button resets to frozen / pushable. Exe: buttons 0x4151F0 /
 * 0x4152F0, pipe 0x4153D0, shared 3-byte flag array ds:0x4E0B2C, all three
 * sounds = w3sf03 (ds:0x4E0B06/10/18).
 */
static int rb_index(const fa_entity_rec *e)
{
    int i = (unsigned char)e->raw[0x2a] & 3;
    return i > 2 ? 0 : i;
}

static int beh_button(fa_entity_rec *e, int wrapped, void *ctx)
{
    fa_beh *b = (fa_beh *)ctx;
    int idx = rb_index(e);

    if (!e->bs[BS_INIT]) {
        e->bs[BS_INIT] = 1;
        e->bs[BS_LS]   = 0;
        e->bs[BS_VX]   = 0;
        e->automove    = 0;
        pd_range(e, 0, 0, 0);                 /* frozen on frame 0            */
        return 0;
    }

    if (e->obj_nr == 84) {                    /* reactor: mirrors the flag    */
        if (e->bs[BS_LS] == 0) {
            if (b->rb_btn[idx]) { e->bs[BS_LS] = 1; set_state(b, e, 0, 0); }
            else e->frame = 0;
        } else if (e->bs[BS_LS] == 1) {
            if (wrapped) { e->bs[BS_LS] = 2;
                           e->anim_first = e->anim_last; e->frame = e->anim_last; }
        } else if (!b->rb_btn[idx]) {
            e->bs[BS_LS] = 0;
            pd_range(e, 0, 0, 0);
        }
        return 0;
    }

    switch (e->bs[BS_LS]) {                   /* ObjNr 83: the pushable button */
    case 0:                                   /* frozen, waiting for a shove   */
        e->frame = 0;
        if (e->bs[BS_VX] != 0) {              /* fa_beh_push gave it a velocity */
            e->bs[BS_VX] = 0;
            e->bs[BS_LS] = 1;
            set_state(b, e, 0, 0);            /* play the press once           */
            if (b->h.sfx) b->h.sfx(FA_BEH_SFX_BOSS_HIT, 83, b->h.user);
        }
        break;
    case 1:                                   /* pressing                      */
        if (wrapped || e->frame >= e->anim_last) {
            e->bs[BS_LS] = 2;
            e->anim_first = e->anim_last;     /* hold pressed                  */
            e->frame = e->anim_last;
            b->rb_btn[idx] = 1;
        }
        break;
    default:                                  /* held down                     */
        if (!b->rb_btn[idx]) {                /* the pipe reset it             */
            e->bs[BS_LS] = 0;
            pd_range(e, 0, 0, 0);
        }
        break;
    }
    return 0;
}

enum { RBP_PARK = 0, RBP_DROP, RBP_RESET };

static int beh_pipe(fa_entity_rec *e, int wrapped, void *ctx)
{
    (void)wrapped;
    fa_beh *b = (fa_beh *)ctx;

    if (!e->bs[BS_INIT]) {
        e->bs[BS_INIT] = 1;
        e->bs[BS_LS] = RBP_PARK;
        e->bs[BS_FX] = e->x;                  /* home X (plain int)           */
        e->bs[BS_KT] = e->y;                  /* home Y (plain int)           */
        e->bs[BS_FY] = (int32_t)e->y << 16;   /* falling Y, 16.16             */
        e->bs[BS_VY] = 0;
        e->automove = 0;
        e->hidden = 1;
        e->force_offscreen = 1;               /* keep ticking while hidden    */
        return 0;
    }

    switch (e->bs[BS_LS]) {
    case RBP_PARK:
        e->hidden = 1;
        if (b->rb_btn[0] && b->rb_btn[1] && b->rb_btn[2]) {
            e->bs[BS_LS] = RBP_DROP;
            e->x = e->bs[BS_FX];
            e->y = e->bs[BS_KT];
            e->bs[BS_FY] = (int32_t)e->bs[BS_KT] << 16;
            e->bs[BS_VY] = 0;
            e->hidden = 0;
            b->rb_pipe = 1;
            if (b->h.sfx) b->h.sfx(FA_BEH_SFX_BOSS_HIT, 85, b->h.user);
        }
        break;

    case RBP_DROP: {
        e->bs[BS_VY] += FX_0_4;               /* exe grav 0.4 (ds:0x452294)   */
        if (e->bs[BS_VY] > FX_20_0) e->bs[BS_VY] = FX_20_0;
        e->bs[BS_FY] += e->bs[BS_VY];
        e->y = fx_round(e->bs[BS_FY]);

        fa_entity_rec *bo = boss_rec(b);
        int done = 0;
        if (bo && bo->obj_nr == 14 &&
            bo->bs[BS_LS] != 300 && bo->bs[BS_LS] != 310) {
            int bx0 = bo->x + 20, bx1 = bo->x + 130;
            int by0 = bo->y + 20, by1 = bo->y + 282;
            if (e->x + 40 >= bx0 && e->x - 40 <= bx1 &&
                e->y + 40 >= by0 && e->y <= by1) {
                bo->bs[BS_LS] = 300;             /* raise the boss hit        */
                bo->bs[BS_AF] = 0;
                pd_range(bo, 38, 48, 0);         /* RBGE02                    */
                if (b->h.sfx) b->h.sfx(FA_BEH_SFX_BOSS_HIT, 14, b->h.user);
                done = 1;
            }
        }
        if (done || e->y > e->bs[BS_KT] + 1000)  /* hit, or fell past (0x4522A4) */
            e->bs[BS_LS] = RBP_RESET;
        break;
    }

    default:                                  /* RBP_RESET                     */
        e->hidden = 1;
        e->x = e->bs[BS_FX];
        e->y = e->bs[BS_KT];
        e->bs[BS_FY] = (int32_t)e->bs[BS_KT] << 16;
        e->bs[BS_VY] = 0;
        b->rb_btn[0] = b->rb_btn[1] = b->rb_btn[2] = 0;  /* buttons reset      */
        b->rb_pipe = 0;
        e->bs[BS_LS] = RBP_PARK;
        break;
    }
    return 0;
}

/* ---- public API ---------------------------------------------- */

fa_beh *fa_beh_create(fa_entity_store *store, const fa_beh_hooks *hooks)
{
    if (!store) return NULL;
    fa_beh *b = (fa_beh *)calloc(1, sizeof *b);
    if (!b) return NULL;
    b->store = store;
    if (hooks) b->h = *hooks;
    b->boss_hp = -1;
    fa_rng_seed(&b->rng, FA_BEH_RNG_DEFAULT_SEED);   /* RRR-52; fa_beh_seed overrides */
    for (int i = 0; i < DESC_COUNT; i++)
        fa_entity_set_behaviour(store, DESC[i].obj_nr, beh_enemy, b);
    for (unsigned i = 0; i < sizeof BLOCK_OBJ / sizeof BLOCK_OBJ[0]; i++)
        fa_entity_set_behaviour(store, BLOCK_OBJ[i], beh_block, b);
    fa_entity_set_behaviour(store, 77, beh_paradiso, b);   /* Kinder Paradiso */
    fa_entity_set_behaviour(store, 414, beh_broesel, b);   /* crumbling platform */
    fa_entity_set_behaviour(store, 59, beh_i7, b);         /* boss-arena i7    */
    fa_entity_set_behaviour(store, 265, beh_iceblock, b);  /* RRR-60 yeti kick */
    fa_entity_set_behaviour(store, 79, beh_icicle, b);     /* RRR-60 yeti hop  */
    fa_entity_set_behaviour(store, 80, beh_abbruch, b);    /* RRR-60 yeti floor*/
    fa_entity_set_behaviour(store, 83, beh_button, b);     /* RRR-61 W3 button */
    fa_entity_set_behaviour(store, 84, beh_button, b);     /* RRR-61 W3 reactor*/
    fa_entity_set_behaviour(store, 85, beh_pipe, b);       /* RRR-61 W3 pipe   */
    return b;
}

void fa_beh_set_world(fa_beh *b, int world)
{
    if (b) b->world = world;
}

void fa_beh_set_character(fa_beh *b, int character)
{
    if (b) b->pchar = character ? 1 : 0;
}

void fa_beh_set_ammo_dirty(fa_beh *b, int dirty)
{
    if (b) b->ammo_dirty = dirty ? 1 : 0;
}

void fa_beh_seed(fa_beh *b, uint32_t seed)
{
    if (b) fa_rng_seed(&b->rng, seed);
}

void fa_beh_free(fa_beh *b)
{
    if (!b) return;
    for (int i = 0; i < DESC_COUNT; i++)
        fa_entity_set_behaviour(b->store, DESC[i].obj_nr, NULL, NULL);
    for (unsigned i = 0; i < sizeof BLOCK_OBJ / sizeof BLOCK_OBJ[0]; i++)
        fa_entity_set_behaviour(b->store, BLOCK_OBJ[i], NULL, NULL);
    fa_entity_set_behaviour(b->store, 77, NULL, NULL);
    fa_entity_set_behaviour(b->store, 414, NULL, NULL);
    fa_entity_set_behaviour(b->store, 59, NULL, NULL);
    fa_entity_set_behaviour(b->store, 265, NULL, NULL);
    fa_entity_set_behaviour(b->store, 79, NULL, NULL);
    fa_entity_set_behaviour(b->store, 80, NULL, NULL);
    fa_entity_set_behaviour(b->store, 83, NULL, NULL);
    fa_entity_set_behaviour(b->store, 84, NULL, NULL);
    fa_entity_set_behaviour(b->store, 85, NULL, NULL);
    free(b);
}

int fa_beh_push(fa_beh *b, int probe_x, int probe_y, int facing)
{
    if (!b) return 0;
    int c = fa_entity_count(b->store);
    for (int i = 0; i < c; i++) {
        fa_entity_rec *e = fa_entity_at_mut(b->store, i);
        if (!e || !e->active || !e->is_block) continue;
        int x0, y0, x1, y1;
        if (fa_entity_frame_box(b->store, i, &x0, &y0, &x1, &y1) != 0) continue;
        y0 += (int)(unsigned char)e->raw[0x2a];   /* contact-box top adjust  */
        if (probe_x < x0 || probe_x >= x1 || probe_y < y0 || probe_y >= y1)
            continue;                             /* half-open, PL-135       */
        e->bs[BS_VX] = facing > 0 ? FX_7_0 : -FX_7_0;
        return 1;
    }
    return 0;
}

void fa_beh_begin_frame(fa_beh *b, int feet_x, int feet_y,
                        int half_w, int height, int facing, int iframes,
                        struct fa_snowball *snow, int snow_max)
{
    if (!b) return;
    b->px = feet_x; b->py = feet_y;
    b->phw = half_w > 0 ? half_w : 40;
    b->ph  = height > 0 ? height : 190;
    b->pface = facing < 0 ? -1 : 1;
    b->pif = iframes;
    b->snow = snow; b->snow_max = snow_max;
    b->pending_dmg = 0;
    b->pending_kb  = 0;
}

int fa_beh_post(fa_beh *b, int *knockback)
{
    if (!b) { if (knockback) *knockback = 0; return 0; }

    /* RRR-59: a player snowball that reaches an incoming boss coconut turns
     * it back toward the boss (0x40C2C0: vx -> +/-9.0, vy -> -9.0). The
     * snowball is consumed. */
    for (int i = 0; i < FA_BEH_MAX_PROJ; i++) {
        bproj *p = &b->proj[i];
        if (!p->alive || p->owner_obj != 10 || p->reflected || !b->snow) continue;
        if (p->vx >= 0) continue;                  /* 0x40C2E9: only vx < 0    */
        int cx = p->x >> 16, cy = p->y >> 16;
        for (int k = 0; k < b->snow_max; k++) {
            struct fa_snowball *s = &b->snow[k];
            if (!s->alive) continue;
            int sx = s->x >> 16, sy = s->y >> 16;
            if (sx < cx - 24 || sx > cx + 24 || sy < cy - 24 || sy > cy + 24)
                continue;
            s->alive = 0;
            p->reflected = 1;
            /* 0x40C318 / 0x40C328 / 0x40C331: vx -> +9.0; vy -> -9.0 when the
             * coconut's own gravity > 0.1 (the lob variant, grav 0.3) else
             * -0.5 (the flat variant, grav 0.03 - it skims back into the
             * torso). Gravity is NOT changed by the reflect. */
            p->vx = FX_9_0;
            p->vy = (p->grav > FX_0_03) ? -FX_9_0 : -FX_0_5;
            p->life = 120;
            if (b->h.sfx) b->h.sfx(FA_BEH_SFX_BOSS_HIT, 10, b->h.user);
            break;
        }
    }

    int px0,py0,px1,py1;
    player_box(b, &px0,&py0,&px1,&py1);
    for (int i = 0; i < FA_BEH_MAX_PROJ; i++) {
        bproj *p = &b->proj[i];
        if (!p->alive) continue;
        p->vy += p->grav;
        if (p->vy > FX_20_0) p->vy = FX_20_0;
        p->x += p->vx;
        p->y += p->vy;
        int wx = p->x >> 16, wy = p->y >> 16;
        if (--p->life <= 0) { p->alive = 0; continue; }
        if (b->h.terrain && b->h.terrain(wx, wy, b->h.user) == 1) { p->alive = 0; continue; }

        /* RRR-59: a turned-back coconut on the boss body is one accepted hit
         * (0x40C338). Ten hits -> +10000 -> KO (0x40CB5B). */
        if (p->reflected && p->owner_obj == 10) {
            fa_entity_rec *bo = boss_rec(b);
            if (bo && bo->obj_nr == 10 && bo->bs[BS_LS] != 300 &&
                bo->bs[BS_LS] != 310) {
                /* the exe torso box: boss.X + [0x46,0xB4], boss.Y + [0x14,0x11A]
                 * (0x40C338..0x40C39C) - fixed, not the ballooning frame AABB */
                int bx0 = bo->x + 70,  bx1 = bo->x + 180;
                int by0 = bo->y + 20,  by1 = bo->y + 282;
                if (wx >= bx0 && wx <= bx1 && wy >= by0 && wy <= by1) {
                    p->alive = 0;
                    if (--b->boss_hp < 0) {
                        b->boss_hp = -1;
                        b->boss_defeated = 1;
                        bo->bs[BS_LS] = 310;
                        bo->collision_enabled = 0;
                        pd_range(bo, 64, 71, 0);        /* KO range 64..71   */
                        if (b->h.score) b->h.score(10000, b->h.user);
                        if (b->h.sfx)
                            b->h.sfx(FA_BEH_SFX_BOSS_KO, 10, b->h.user);
                    } else {
                        /* hit: play the Freeze range, then state 300 does the
                         * taunt speech + chest-beat + resume (exe 0x40C9F0 ->
                         * 0x40CA7A -> taunt -> state 50). */
                        bo->bs[BS_LS] = 300;
                        pd_range(bo, 53, 63, 0);        /* Freeze 53..63     */
                        if (b->h.sfx)
                            b->h.sfx(FA_BEH_SFX_BOSS_HIT, 10, b->h.user);
                    }
                    continue;
                }
            }
        }

        /* an ordinary (not turned-back) projectile hurts the player on contact */
        if (!p->reflected && b->pif == 0 &&
            wx >= px0 && wx <= px1 && wy >= py0 && wy <= py1) {
            p->alive = 0;
            b->pending_dmg = 20;
            b->pending_kb  = 1;
        }
    }
    if (knockback) *knockback = b->pending_kb;
    return b->pending_dmg;
}

int fa_beh_enemies_alive(const fa_beh *b)
{
    if (!b) return 0;
    int n = 0, c = fa_entity_count(b->store);
    for (int i = 0; i < c; i++) {
        const fa_entity_rec *e = fa_entity_at(b->store, i);
        if (e && e->active && desc_for(e->obj_nr) && e->bs[BS_LS] < 100) n++;
    }
    return n;
}

int fa_beh_projectiles_live(const fa_beh *b)
{
    if (!b) return 0;
    int n = 0;
    for (int i = 0; i < FA_BEH_MAX_PROJ; i++) if (b->proj[i].alive) n++;
    return n;
}

int fa_beh_emitter_live(const fa_beh *b, int i)
{
    if (!b) return 0;
    const fa_entity_rec *e = fa_entity_at(b->store, i);
    if (!e || !e->active) return 0;
    if (e->obj_nr != 13 && e->obj_nr != 16) return 0;   /* flying robot / bee */
    return e->bs[BS_LS] < 100;   /* exe rec[0x62] < 0x64: not hit/launch/spent */
}

int fa_beh_boss_hp(const fa_beh *b) { return b ? b->boss_hp : -1; }
int fa_beh_boss_defeated(const fa_beh *b) { return b ? b->boss_defeated : 0; }
int fa_beh_recipe_done(const fa_beh *b) { return b ? b->recipe_done : 0; }

int fa_beh_push_carry(fa_beh *b, int probe_x, int probe_y)
{
    if (!b) return 0;
    int c = fa_entity_count(b->store);
    for (int i = 0; i < c; i++) {
        fa_entity_rec *e = fa_entity_at_mut(b->store, i);
        if (!e || !e->active || !e->is_block) continue;
        int x0, y0, x1, y1;
        if (fa_entity_frame_box(b->store, i, &x0, &y0, &x1, &y1) != 0) continue;
        y0 += (int)(unsigned char)e->raw[0x2a];
        if (probe_x >= x0 && probe_x < x1 && probe_y >= y0 && probe_y < y1)
            return e->dx;
    }
    return 0;
}

int fa_beh_projectile(const fa_beh *b, int i, int *wx, int *wy, int *owner_obj)
{
    if (!b || i < 0 || i >= FA_BEH_MAX_PROJ || !b->proj[i].alive) return 0;
    if (wx) *wx = b->proj[i].x >> 16;
    if (wy) *wy = b->proj[i].y >> 16;
    if (owner_obj) *owner_obj = b->proj[i].owner_obj;
    return 1;
}
