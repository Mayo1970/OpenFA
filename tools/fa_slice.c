/*
 * fa_slice.c - the desktop vertical-slice entry point (RRR-41/42/43/47).
 *
 *   fa_slice                 open a window; show the title / world-select menu
 *                            if GData is found, else a test pattern. Click a
 *                            world circle to play it (GIUNGLA = world 1), or
 *                            use keyboard arrows / a controller D-pad and
 *                            confirm with Enter / controller A.
 *   fa_slice --gdata DIR     use this GData directory
 *   fa_slice --world N       skip the menu, boot straight into world N (1..4)
 *   fa_slice --tut           with --world: load the tutorial layout (WeltNt)
 *   fa_slice --end           with --world: boot into the boss arena (WeltNE);
 *                            in play, all 6 recipe pieces trigger this too
 *   fa_slice --grid          with --world: draw the debug cell overlay
 *   fa_slice --tone          keep the 440 Hz tone on (default: 1 s at start)
 *   fa_slice --silent        no startup tone
 *   fa_slice --frames N      run N frames headless and print stats
 *   fa_slice --seed N        pin the enemy RNG (default: wall clock, RRR-57)
 *
 * Menu: this is the screen the real game opens on. Compare it to the oracle.
 *       Mouse, keyboard arrows, and a controller D-pad select a world;
 *       Enter / controller A confirms it.
 * Level (--world N): arrows walk, A jump, S throw, D switch kid. Esc quits.
 *   P  toggle free-move (dev): fly through walls to reach the pickups; hold
 *      A while flying for a fast dash. Pickups and the boss gate still work.
 *   I  skip straight to this world's boss arena (dev).
 *
 * GData lookup order: --gdata DIR, then <exe dir>/GData, then ./GData.
 */
#include "fa/fa_app.h"
#include "fa/fa_platform.h"
#include "fa/fa_input.h"
#include "fa/fa_surface.h"
#include "fa/fa_w01.h"
#include "fa/fa_map.h"
#include "fa/fa_render.h"
#include "fa/fa_entity.h"
#include "fa/fa_beh.h"
#include "fa/fa_player.h"
#include "fa/fa_charspr.h"
#include "fa/fa_collide.h"
#include "fa/fa_death.h"
#include "fa/fa_menu.h"
#include "fa/fa_hiscore.h"
#include "fa/fa_credits.h"
#include "fa/fa_hud.h"
#include "fa/fa_audio.h"
#include "fa/fa_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

typedef struct {
    fa_menu    *menu;
    fa_credits *credits;         /* non-NULL = the credits screen is up (RRR-54) */
    fa_hiscore *scores;          /* non-NULL = the high-score screen is up */
    int       show_scores;       /* request from the menu, handled next tick */
    int       hover;             /* menu button under the pointer, or -1 */
    int       menu_focus;        /* keyboard / D-pad focused world, or -1 */
    int       pending_world;     /* 1..4: load this world next tick      */
    int       world;             /* 1..4: the world currently loaded     */
    int       in_end;            /* in the boss arena (WeltNE), not the world */
    int       end_pending;       /* recipe complete: load WeltNE next tick    */
    int       boss_win_timer;    /* RRR-59: >0 counting down to CLASSIFICA    */
    int       tut_reload_pending;/* RRR-54: tutorial cleared - reload as WeltN */
    int       want_quit;
    char      gdata[600];

    int          have_map;
    fa_w01       bg;
    fa_map       map;
    fa_tileset  *tiles;
    fa_entity_store *ents;
    fa_beh      *beh;            /* RRR-51: per-ObjNr enemy behaviour       */
    fa_hud      *hud;           /* RRR-51 AC5: score / health / ammo panel */
    int          health;        /* RRR-51: 0x45F014, 100 max, 20 per hit   */
    int          ammo;         /* RRR-51 AC4: 0x45ED34, snowballs 0..10   */
    int          items[6];     /* RRR-51 AC4: 0x45EFD4, recipe-piece flags */
    int          dirty_shot;   /* RRR-51 AC4: collect_dirtyballs was taken */
    fa_camera    cam;
    fa_player pl;
    int       use_player;
    int       freemove;         /* P toggle: fly through the level, no clip  */

    /* RRR-57: the retail exe seeds the one rand() stream from the wall clock
     * at start-up (0x42AE27). RRR-52 shipped a fixed default for replay. Here
     * the slice seeds the enemy RNG from time() unless --seed N pins it. */
    uint32_t  rng_seed;
    int       rng_seed_set;

    /* RRR-53: health 0 -> the run ends. fa_death times the KO hold (240) +
     * fade (16); the level keeps running underneath. On DONE the level is
     * torn down and the CLASSIFICA / high-score screen comes up, then the
     * world-select menu. No in-place restart, no lives. */
    fa_death  death;
    /* solid probe ctx: terrain + the lift/block layer (RRR-50 follow-up) */
    struct { const fa_map *map; const fa_tileset *ts; fa_entity_store *ents; } coll;

    fa_cs_sheet kid_sheet[2];    /* 0 = penguin, 1 = Milchschnitte */
    fa_cs_anim  kid_anim[2];
    int         have_kids;
    int         kid_was_crouch;  /* to trigger the stand-up animation */
    int         kid_rise;        /* ticks left in the crouch-rise pose */
    int         grid;
    int       tone;         /* --tone: a continuous 440 Hz tone            */
    int       want_chime;   /* default: a 1 s startup tone, then silence   */
    int       chime_pos;    /* frames of chime emitted so far              */
    double    tone_phase;

    fa_audio *audio;        /* RRR-46 mixer; NULL without GData / --mute   */
    int       mute;         /* --mute: create no mixer                     */
    int       vol;          /* --vol 0..255, <0 = default (full)          */
    int       swap_was;     /* previous tick's FA_PST_SWAP flag            */
    int       jump_was;     /* previous tick's FA_PST_JUMP flag            */
    int       thr_was;      /* previous tick's throw_anim > 0              */
    int       glide_was;    /* previous tick's penguin glide flag         */

    uint32_t  cr_act_was;   /* RRR-54: prev tick's action mask (credits skip) */
    uint32_t  menu_nav_seen;/* suppress a held edge across multi-tick frames */
    int       score;        /* RRR-50: pickups collected                   */
    int       hurt_cd;      /* RRR-50: ticks of hit invulnerability left   */
    int       push_was;     /* RRR-50: previous tick was shoving a block   */

    /* physics-tuning overrides (px/tick), <=0 = keep the default */
    double    ov_gravity, ov_jumpvel, ov_jumpvel2, ov_runspeed, ov_airaccel;
    /* RRR-44/45 tuning overrides (whole px), <=0 = keep the default */
    int       ov_bboxw, ov_bboxh, ov_camdzx, ov_camdzy, ov_cambias;
} slice;

static void slice_focus_first_menu(slice *s)
{
    s->menu_focus = s->menu ? 0 : -1;
    s->hover = s->menu ? 0 : -1;
    s->menu_nav_seen = 0;
    if (s->menu) {
        for (int i = 0; i < fa_menu_button_count(s->menu); i++)
            fa_menu_set_state(s->menu, i, FA_MENU_REST);
        fa_menu_set_state(s->menu, 0, FA_MENU_HOVER);
    }
}

static void slice_set_menu(slice *s, fa_menu *menu)
{
    s->menu = menu;
    slice_focus_first_menu(s);
}

static void slice_merge_pad_movement(const fa_frame_input *fi, uint32_t *m)
{
    uint32_t b = fi->pad_down;
    if ((b & (1u << FA_PAD_DPAD_LEFT)) || fi->pad_lx < -0.35f)
        *m |= 1u << FA_ACT_LEFT;
    if ((b & (1u << FA_PAD_DPAD_RIGHT)) || fi->pad_lx > 0.35f)
        *m |= 1u << FA_ACT_RIGHT;
    if ((b & (1u << FA_PAD_DPAD_UP)) || fi->pad_ly < -0.35f)
        *m |= 1u << FA_ACT_UP;
    if ((b & (1u << FA_PAD_DPAD_DOWN)) || fi->pad_ly > 0.35f)
        *m |= 1u << FA_ACT_DOWN;

    /* Xbox-reference face layout: A jumps, B throws, Y swaps the active kid.
     * SDL_GameController supplies these as logical buttons on other layouts. */
    if (b & (1u << FA_PAD_A)) *m |= 1u << FA_ACT_JUMP;
    if (b & (1u << FA_PAD_B)) *m |= 1u << FA_ACT_FIRE;
    if (b & (1u << FA_PAD_Y)) *m |= 1u << FA_ACT_SPARE;
}

/* decode one .W01 frame and blit it colour-keyed, centred on (cx, cy) in the
 * destination surface. Returns 1 on success, 0 if there is no sheet / the
 * frame is out of range / decode failed. */
static int blit_w01_centered(const fa_surface *dst, const fa_w01 *w,
                             int frame, int cx, int cy)
{
    if (!w || frame < 0 || frame >= fa_w01_count(w)) return 0;
    int fw = 0, fh = 0;
    if (fa_w01_frame_size(w, frame, &fw, &fh) != 0 || fw <= 0 || fh <= 0)
        return 0;
    uint16_t *px = (uint16_t *)malloc((size_t)fw * fh * sizeof *px);
    if (!px) return 0;
    if (fa_w01_decode(w, frame, px) != 0) { free(px); return 0; }
    fa_surface src;
    fa_surface_wrap(&src, px, fw, fh, 0);
    fa_blit_keyed(dst, cx - fw / 2, cy - fh / 2, &src, NULL, NULL, FA_COLORKEY);
    free(px);
    return 1;
}

static void apply_tuning(slice *s)
{
    if (s->ov_gravity   > 0) s->pl.t.gravity     = (int32_t)(s->ov_gravity   * FA_FIX_ONE);
    if (s->ov_jumpvel   > 0) s->pl.t.jump_vel    = -(int32_t)(s->ov_jumpvel  * FA_FIX_ONE);
    if (s->ov_jumpvel2  > 0) s->pl.t.jump_vel_c1 = -(int32_t)(s->ov_jumpvel2 * FA_FIX_ONE);
    if (s->ov_runspeed  > 0) s->pl.t.run_speed   = (int32_t)(s->ov_runspeed  * FA_FIX_ONE);
    if (s->ov_airaccel  > 0) s->pl.t.air_accel   = (int32_t)(s->ov_airaccel  * FA_FIX_ONE);
    if (s->ov_bboxw     > 0) s->pl.t.body_hw     = s->ov_bboxw;
    if (s->ov_bboxh     > 0) s->pl.t.body_h      = s->ov_bboxh;
}

static int load_world(slice *s, const char *maps, int world,
                      const char *w01suf, const char *w02suf);

/* ladder probe for the climb state: a plane-2 grid entry with code bit 0. */
static int slice_ladder(int px, int py, void *ctx)
{
    return fa_map_ladder_at((const fa_map *)ctx, px, py);
}

/* RRR-44: terrain collision class at a world pixel (0 none / 1 solid / 2
 * one-way). Per-pixel via the decoded atlas so slopes follow their diagonal
 * (fa_render_solid_px); falls back to the coarse tile query without GData. */
typedef struct { const fa_map *map; const fa_tileset *ts;
                 fa_entity_store *ents; } coll_ctx;

static int slice_terrain(int px, int py, void *ctx)
{
    const coll_ctx *c = ctx;
    return fa_render_solid_px(c->map, c->ts, px, py);
}

/* RRR-50: terrain OR a lift top / a pushable block, so the player stands on
 * rafts (fall pose fixed) and cannot walk through blocks. */
static int slice_solid(int px, int py, void *ctx)
{
    const coll_ctx *c = ctx;
    int r = fa_render_solid_px(c->map, c->ts, px, py);
    if (!r && c->ents) {
        int e = fa_entity_solid_at(c->ents, px, py);   /* 1 block / 2 lift */
        if (e) r = e;
    }
    return r;
}

/* RRR-50 (PL-103): a pushable block at (px,py) -> Fettalatte enters PUSH. */
static int slice_pushable(int px, int py, void *ctx)
{
    return fa_entity_pushable_at((const fa_entity_store *)ctx, px, py);
}

/*
 * RRR-53: a HAZARD tile at world pixel (px,py). The exe (fcn.0041A290, run
 * from the player state-machine tail at 0x419DA1 every frame) queries plane 2
 * at the player origin: a non-empty cell whose attr byte has bit 0x80 set is
 * a hazard - spikes / lava / a bottom-of-pit pool. On it: play the character
 * hit sound (pi0005 / ms0007), and if not in i-frames deal 20 damage + 120
 * i-frames; either way bounce vy = -20.0 (0x431A20 with 0xFFFEC000). Shipped
 * maps: Welt1 a SOLID band at y=2336 (the floor of the water gaps between
 * platforms), Welt2 SOLID spike tiles, Welt3/4 NON-solid lava pools. `attr &
 * 0x80` was PL-092's "alt collision" - it is the hazard flag.
 *
 * The exe samples exactly `trunc(feet)`. `fa_collide` rests a standing kid
 * ~1 px ABOVE the solid pixel it lands on, so on the Welt1 SOLID hazard band
 * the feet can read as the empty row just above - check the feet cell AND the
 * cell a few px below (the tile the kid is standing ON), so wading in the
 * water hurts (owner playtest) while the grass platform above (no 0x80 tile)
 * never does.
 */
static int slice_hazard(const fa_map *m, int px, int py)
{
    int tw = m->info.tile_w > 0 ? m->info.tile_w : 32;
    int th = m->info.tile_h > 0 ? m->info.tile_h : 32;
    if (px < 0 || py < 0) return 0;
    for (int dy = 0; dy <= 4; dy += 4) {
        fa_map_entry e = fa_map_cell_entry(m, px / tw, (py + dy) / th, 2);
        if (!fa_map_entry_empty(e) && (e.attr & FA_MAP_ATTR_HAZARD))
            return 1;
    }
    return 0;
}

/* --- RRR-51 behaviour-layer hooks --- */
static int beh_terrain(int px, int py, void *ctx)
{
    slice *s = (slice *)ctx;
    return fa_render_solid_px(s->coll.map, s->coll.ts, px, py);
}
static void beh_score(int add, void *ctx)
{
    slice *s = (slice *)ctx;
    s->score += add;
}

/*
 * RRR-51 AC4 - one collected DetailGroup-1 pickup. The effect is per ObjNr,
 * traced from the handlers the exe installs at 0x411c18..0x411d46:
 *   48/49/50 collect_paradiso/pinguin/milchschnitte  0x40edb0  score += 100
 *   51       collect_energy                          0x40eeb0  health += 40 (cap 100)
 *   52       collect_snowballs                       0x40efe0  ammo = 10, normal
 *   53..58   collect_i1..i6                          0x40f110+ score += 1000, set flag
 *   59       collect_i7                              0x40f770  score += 10000
 *   60       collect_dirtyballs                      0x40fa00  ammo = 10, "dirty"
 * (the "+N" POINTS.W01 popup the exe also spawns is cosmetic - not ported.)
 * The energy + both ammo pickups (51/52/60) respawn 1200 ticks (20 s) after
 * they are taken (exe rec[+0x74] = 0x4B0); the score / recipe ones are gone
 * for good. The callback returns that respawn delay (0 = permanent).
 */
static int beh_pickup(int obj_nr, int detail_group, void *ctx)
{
    slice *s = (slice *)ctx;
    (void)detail_group;
    switch (obj_nr) {
        case 48: case 49: case 50: s->score += 100;   break;
        case 51:
            s->health += 40;
            if (s->health > 100) s->health = 100;
            return 1200;
        case 52: s->ammo = 10; s->dirty_shot = 0;     return 1200;
        case 53: case 54: case 55: case 56: case 57: case 58:
            s->score += 1000;
            s->items[obj_nr - 53] = 1;
            break;
        case 59: s->score += 10000;                   break;
        case 60: s->ammo = 10; s->dirty_shot = 1;     return 1200;
        default: break;
    }
    return 0;
}
static void beh_sfx(int ev, int obj, void *ctx)
{
    slice *s = (slice *)ctx;
    if (!s->audio) return;
    switch (ev) {
        case FA_BEH_SFX_BOSS_KO_ANIM:                   /* RRR-61 robot: RBKO fr56 */
            if (obj == 14) fa_audio_event(s->audio, FA_SND_W3_BOSS_KO);
            break;
        case FA_BEH_SFX_BOSS_LAND:                      /* RRR-60 yeti: hop land  */
            if (obj == 9) fa_audio_event(s->audio, FA_SND_W2_BOSS_LAND);
            break;
        case FA_BEH_SFX_BOSS_HURT:                      /* RRR-60 yeti: hit fr69  */
            if (obj == 9) fa_audio_event(s->audio, FA_SND_W2_BOSS_HURT);
            break;
        case FA_BEH_SFX_BOSS_VOICE_CUT:                 /* RRR-60 yeti: hit cuts   */
            fa_audio_stop(s->audio, FA_CH_VOICE);       /* the roar/taunt line     */
            fa_audio_stop(s->audio, FA_CH_BOSS);
            break;
        case FA_BEH_SFX_BOSS_KO:
            if (obj == 14) { fa_audio_event(s->audio, FA_SND_W3_BOSS_DEFEAT); break; }
            /* fall through */
        case FA_BEH_SFX_ENEMY_KO:
        case FA_BEH_SFX_FREEZE:    /* snowman: 0x422B60(3, alsf04) at 0x41AC5C */
                                  fa_audio_event(s->audio, FA_SND_ENEMY_DEFEAT); break;
        case FA_BEH_SFX_ENEMY_LAUNCH: break;   /* alsf04 already played at KO  */
        case FA_BEH_SFX_ENEMY_ATTACK:          /* the dive - one cue per bird  */
            fa_audio_event(s->audio,
                obj == 6  ? FA_SND_ENEMY_DIVE_EAGLE :
                obj == 16 ? FA_SND_ENEMY_DIVE_BEE   : FA_SND_ENEMY_DIVE);
            break;
        case FA_BEH_SFX_ENEMY_SHOT:            /* the throw - egg robot differs */
            fa_audio_event(s->audio,
                obj == 14 ? FA_SND_W3_BOSS_SHOT :       /* RRR-61 robot bolt   */
                obj == 18 ? FA_SND_W4_BOSS_SHOT :       /* RRR-62 milk particle */
                obj == 12 ? FA_SND_ENEMY_THROW_EGG : FA_SND_ENEMY_THROW);
            break;
        case FA_BEH_SFX_BOSS_CHARGE:                    /* boss wind-up cue     */
            fa_audio_event(s->audio,
                obj == 18 ? FA_SND_W4_BOSS_DRINK        /* RRR-62 octopus slurp */
                          : FA_SND_W3_BOSS_CHARGE);     /* RRR-61 robot wind-up */
            break;
        case FA_BEH_SFX_BOSS_HIT:
            /* RRR-61 World 3: button push / pipe drop / the robot taking a
             * hit each have their own w3sf cue (owner playtest). */
            fa_audio_event(s->audio,
                (obj == 83 || obj == 84) ? FA_SND_W3_BUTTON :
                obj == 85                ? FA_SND_W3_PIPE :
                obj == 14                ? FA_SND_W3_BOSS_HIT
                                         : FA_SND_ENEMY_KNOCK);
            break;
        case FA_BEH_SFX_BROESEL_BREAK: {   /* 0x422B60(6, knusper.wav) at 0x416D2D */
            int clip = fa_audio_load(s->audio, "SDat/knusper.wav");
            if (clip >= 0) fa_audio_play_clip(s->audio, clip, 6, 0, 256);
            break;
        }
        default: break;
    }
}

/*
 * A streamed voice line. The exe splits these across two lanes: the Kinder
 * Paradiso mascot streams on channel 0x11 = lane 17 (0x415550), the world
 * bosses on channel 0x12 = lane 18 (0x40E350 etc). Route by the file stem -
 * gb/yb/rb/ob NNNN are boss lines, pa/pat NNNN are the mascot.
 */
static void beh_voice(const char *rel_wav, void *ctx)
{
    slice *s = (slice *)ctx;
    if (!rel_wav) return;
    const char *base = strrchr(rel_wav, '/');
    base = base ? base + 1 : rel_wav;
    int c0 = base[0] | 0x20, c1 = base[1] | 0x20;   /* fold case */
    int boss = c1 == 'b' &&
               (c0 == 'g' || c0 == 'y' || c0 == 'r' || c0 == 'o');
    int lane = boss ? FA_CH_BOSS : FA_CH_VOICE;
    printf("voice -> %s\n", rel_wav);   /* console cue (Paradiso / boss) */
    if (s->audio) fa_audio_play_stream(s->audio, rel_wav, lane, 0);
}

static int beh_voice_busy(void *ctx)
{
    slice *s = (slice *)ctx;
    if (!s->audio) return 0;
    return fa_audio_channel_busy(s->audio, FA_CH_VOICE) ||
           fa_audio_channel_busy(s->audio, FA_CH_BOSS);
}

/*
 * RRR-54 / PL-142: the tutorial-end Kinder Paradiso (rec[+0x2A] == 7) finished
 * its pat0020 line. The exe writes tut.ini[world] = 1 (0x4159BD) and requests
 * scene 20 (0x4159C5), which reloads the world as its normal level
 * (0x4126F3: 0x4DABD4 = 0x4DAB5C, 0x45F008 = 0). Do exactly that: persist the
 * flag, then queue a reload of the same world (now WeltN, since slice_tut_seen
 * reads the file we just wrote).
 */
static void beh_tutorial_done(void *ctx)
{
    slice *s = (slice *)ctx;
    if (s->world >= 1 && s->world <= 4) {
        fa_vfs v;
        if (fa_vfs_init_default(&v, s->gdata, "FreshAdventures") == 0)
            fa_vfs_set_tut_world_seen(&v, s->world, 1);
    }
    printf("tutorial cleared -> Welt%d (normal level)\n", s->world);
    s->tut_reload_pending = 1;
}

/*
 * Arm the world's positional loops (exe level-audio setup 0x412643). World 3
 * starts w3sf01 on slot 8 (electric floor) and w3sf11 on slot 14 (flying
 * robot); world 4 starts w4sf03 on slot 14 (bee). Each starts muted -
 * slice_posloops() rides the volume by distance every tick. Worlds 1/2 have
 * no positional loop; the slots stay clear.
 */
static void set_world_ambient(slice *s, int world)
{
    if (!s->audio) return;
    fa_audio_stop(s->audio, 8);
    fa_audio_stop(s->audio, 14);
    if (world == 3) {
        fa_audio_event(s->audio, FA_SND_AMBIENT_W3);   /* w3sf01 slot 8  */
        fa_audio_event(s->audio, FA_SND_UFO);          /* w3sf11 slot 14 */
        fa_audio_set_channel_gain(s->audio, 8, 0);
        fa_audio_set_channel_gain(s->audio, 14, 0);
    } else if (world == 4) {
        fa_audio_event(s->audio, FA_SND_BEE_LOOP);     /* w4sf03 slot 14 */
        fa_audio_set_channel_gain(s->audio, 14, 0);
    }
}

/* stop every looping / one-shot SFX lane plus the voice channels. Called on
 * any level or menu transition so ambient / proximity loops (electric floor,
 * UFO, bee, glide, boss charge) never bleed across screens (owner ask). */
static void slice_audio_hush(slice *s)
{
    if (!s->audio) return;
    fa_audio_stop_sfx(s->audio);
    fa_audio_stop(s->audio, FA_CH_VOICE);
    fa_audio_stop(s->audio, FA_CH_BOSS);
}

/* ================================================================== *
 *  Positional loops  (exe emitter 0x412EE0 + volume pass 0x41139D)
 *
 *  The exe keeps one looping WAV per slot running for the whole level and
 *  sets its volume each frame from the minimum squared distance, in screen
 *  pixels, between screen centre (FA_FB_W/2, FA_FB_H/2) and the four corners
 *  of any live emitter's sprite box. A corner past a +/-1200 x / +/-900 y
 *  window of centre is ignored (0x412EE0). Volume curve (0x41139D): 0 dB
 *  within 350 px, a ramp linear in distance^2 to about -50 dB by 1063 px,
 *  then silence. The loop is never stopped mid-level - a far emitter is just
 *  gain 0.
 * ================================================================== */
#define POSLOOP_FULL_PX   350   /* <= : full volume                          */
#define POSLOOP_FADE_PX  1063   /* >= : muted (sqrt of the exe's 0x113E10)   */
#define POSLOOP_CULL_X   1200   /* corner ignored past this |dx| ...         */
#define POSLOOP_CULL_Y    900   /* ... or this |dy| (exe 0x412EE0)           */

/* min squared distance -> lane gain (/256). The exe ramps a dB value
 * linearly in distance^2 from 0 to ~-50 across the band, then hard-mutes;
 * DirectSound turns that dB into amplitude, and so do we. */
static int posloop_gain(long d2)
{
    long full = (long)POSLOOP_FULL_PX * POSLOOP_FULL_PX;
    long fade = (long)POSLOOP_FADE_PX * POSLOOP_FADE_PX;
    if (d2 <= full) return 256;
    if (d2 >= fade) return 0;
    double t  = (double)(d2 - full) / (double)(fade - full);   /* 0..1     */
    double db = -50.0 * t;                                     /* exe ramp */
    int g = (int)(256.0 * pow(10.0, db / 20.0) + 0.5);
    return g < 0 ? 0 : (g > 256 ? 256 : g);
}

/* Ride `lane`'s gain from the nearest live emitter of ObjNr `o1` (or `o2`,
 * -1 to skip). `guard` = 1 gates each emitter on fa_beh_emitter_live (the exe
 * rec[0x62] < 100 flyer guard); 0 = any active record emits (the electric
 * floor runs for the whole level once spawned). */
static void posloop_update(slice *s, int o1, int o2, int lane, int guard)
{
    if (!s->audio || !s->ents) return;
    long cx = (long)s->cam.x + FA_FB_W / 2;
    long cy = (long)s->cam.y + FA_FB_H / 2;
    long best = -1;
    int n = fa_entity_count(s->ents);
    for (int i = 0; i < n; i++) {
        const fa_entity_rec *e = fa_entity_at(s->ents, i);
        if (!e || !e->active) continue;
        if (e->obj_nr != o1 && e->obj_nr != o2) continue;
        if (guard && s->beh && !fa_beh_emitter_live(s->beh, i)) continue;
        int x0, y0, x1, y1;
        if (fa_entity_frame_box(s->ents, i, &x0, &y0, &x1, &y1) != 0) {
            x0 = x1 = e->x; y0 = y1 = e->y;
        }
        const int cxs[4] = { x0, x1, x0, x1 };
        const int cys[4] = { y0, y0, y1, y1 };
        for (int k = 0; k < 4; k++) {
            long dx = cxs[k] - cx, dy = cys[k] - cy;
            if (dx < -POSLOOP_CULL_X || dx > POSLOOP_CULL_X) continue;
            if (dy < -POSLOOP_CULL_Y || dy > POSLOOP_CULL_Y) continue;
            long d2 = dx * dx + dy * dy;
            if (best < 0 || d2 < best) best = d2;
        }
    }
    fa_audio_set_channel_gain(s->audio, lane, best < 0 ? 0 : posloop_gain(best));
}

/* per-tick: drive whichever positional loops this world armed. */
static void slice_posloops(slice *s)
{
    if (s->world == 3) {
        posloop_update(s, 355, 356,  8, 0);   /* electric floor -> w3sf01 */
        posloop_update(s,  13,  -1, 14, 1);   /* flying robot   -> w3sf11 */
    } else if (s->world == 4) {
        posloop_update(s,  16,  -1, 14, 1);   /* bee            -> w4sf03 */
    }
}

/*
 * Spawn point (RRR-50). The level's ObjNr 1000 (misc_start.jrs) entity record
 * carries the real spawn X/Y - the exe reads it at 0x4118F5 (PL-127). Fall
 * back to the RRR-44 terrain scan when the entity layer is absent (no GData).
 */
static void find_spawn(slice *s, int *out_x, int *out_y)
{
    if (s->ents && fa_entity_player_start(s->ents, out_x, out_y)) return;

    const fa_map *m = &s->map;
    for (int x = 80; x <= 400; x += 16) {
        int y = 0;
        for (; y < m->world_h; y += 8)
            if (fa_map_solid_class(m, x, y) == FA_SOLID_FULL) break;
        if (y < m->world_h) { *out_x = x; *out_y = y; return; }
    }
    *out_x = 96;
    *out_y = m->world_h - 32;
}

/* RRR-44/45: place the kid on solid ground, bind collision, frame the camera. */
static void wire_level(slice *s)
{
    int sx, sy;
    find_spawn(s, &sx, &sy);
    printf("spawn: kid at %d,%d (world %dx%d)\n", sx, sy,
           s->map.world_w, s->map.world_h);
    fa_player_init(&s->pl, sx, sy);
    s->pl.t.floor_y = FA_FIX(s->map.world_h - 32);

    s->coll.map = &s->map;
    s->coll.ts = s->tiles;
    s->coll.ents = s->ents;
    fa_player_set_ladder(&s->pl, slice_ladder, &s->map);
    fa_player_set_solid(&s->pl, slice_solid, &s->coll);
    fa_player_set_pushable(&s->pl, slice_pushable, s->ents);
    if (s->ents)
        fa_entity_set_terrain(s->ents, slice_terrain, &s->coll);

    /* RRR-51: bind the per-ObjNr enemy behaviour layer over the entity store */
    fa_beh_free(s->beh);
    s->beh = NULL;
    s->freemove = 0;             /* DEV: always start a level clipped */
    s->health = 100;             /* 0x45F014 init (0x4086fb) */
    s->ammo = 10;               /* 0x45ED34 init (0x4086f1) */
    s->dirty_shot = 0;
    memset(s->items, 0, sizeof s->items);
    fa_death_init(&s->death);    /* RRR-53: clear any in-flight death sequence */
    if (s->ents) {
        static const fa_beh_hooks HK = { beh_terrain, beh_score, beh_sfx,
                                         beh_voice, beh_voice_busy,
                                         beh_tutorial_done, NULL };
        fa_beh_hooks hk = HK;
        hk.user = s;
        s->beh = fa_beh_create(s->ents, &hk);
        fa_beh_seed(s->beh, s->rng_seed);   /* RRR-57: clock seed / --seed N */
        fa_beh_set_world(s->beh, s->world);
        fa_beh_set_character(s->beh, s->pl.character & 1);
    }
    set_world_ambient(s, s->world);
    apply_tuning(s);
    if (s->ov_camdzx  > 0) s->cam.dz_x   = s->ov_camdzx;
    if (s->ov_camdzy  > 0) s->cam.dz_y   = s->ov_camdzy;
    if (s->ov_cambias != 0) s->cam.bias_y = s->ov_cambias;

    /* snap the camera onto the player for frame 0 - the exe does an intro
     * pan (0x41129a), which is RRR-45 tuning */
    fa_camera_center_on(&s->cam, fa_player_px(&s->pl),
                        fa_player_py(&s->pl) + s->cam.bias_y);
}

/*
 * The two playable kids animate from PINGUIN.W01 / MILCHSCHNITTE.W01. The
 * per-pose frame ranges are the JR_FERRERO.exe state-machine constants
 * (RRR-43/player-anim-disasm.md, PL-096/097): each player state writes
 * {current, loop_start, inclusive_end, repeat} into the character's
 * animation record. `loop` = 1 repeats, 0 holds the last frame.
 *
 * base_facing = -1: the raw art is LEFT-facing (PL-099); the engine mirrors
 * it for a right-facing player. Advance rate = one frame per 2 ticks
 * (period 2 -> 30 fps, PL-098). The WESTKA sidecars are NOT used for
 * selection - several of their ranges disagree with the engine.
 */
typedef struct { fa_cs_pose pose; int cur, loop_first, end, loop; } kid_clip;

static const kid_clip PENGUIN_CLIPS[] = {
    { FA_CS_STAND,       47,  47,  47, 1 },  /* state 0/1  */
    { FA_CS_WALK,        75,  78,  87, 1 },  /* state 2/3  */
    { FA_CS_JUMP_RISE,    0,   0,   8, 0 },  /* state 4/5  */
    { FA_CS_JUMP_FALL,    8,   8,  14, 0 },  /* state 6/7  (rise 0..8 -> 9..14) */
    { FA_CS_CROUCH,     136, 136, 143, 0 },  /* state 8/9  down: hold at 143 */
    { FA_CS_CROUCH_RISE,144, 144, 149, 0 },  /* state 9 release: 144..149 once */
    { FA_CS_GLIDE,      287, 287, 287, 0 },  /* state 10/11 - a single frame */
    { FA_CS_CLIMB,      124, 124, 135, 1 },  /* state 12/13 (PL-102) */
    { FA_CS_THROW_FWD,  233, 233, 260, 0 },  /* state 14/15 snowball (spawn f255) */
    { FA_CS_THROW_UP,   233, 233, 260, 0 },  /* same range; only trajectory differs */
    { FA_CS_KO,         150, 158, 162, 1 },  /* state 34/35 */
    { FA_CS_IDLE_A,      65,  65,  69, 0 },  /* state 1 idle A (PL-101) */
    { FA_CS_IDLE_B,      91,  91, 115, 0 },  /* state 1 idle B */
    { FA_CS_SWAP,        65,  65,  69, 1 },  /* switch: loops 65-69 for the voice line */
    { FA_CS_SWAP_END,    65,  65,  69, 1 },  /* penguin has no turn-back */
};
static const kid_clip MILCH_CLIPS[] = {
    { FA_CS_STAND,        0,   0,   0, 1 },  /* state 16/17 */
    { FA_CS_WALK,         0,   3,  12, 1 },  /* state 18/19 */
    { FA_CS_JUMP_RISE,   56,  56,  58, 0 },  /* state 20/21 */
    { FA_CS_JUMP_FALL,   58,  58,  65, 0 },  /* state 22/23 */
    { FA_CS_CROUCH,     259, 259, 266, 0 },  /* state 24/25 down */
    { FA_CS_CROUCH_RISE,267, 267, 272, 0 },  /* state 25 release */
    /* no glide state for Milch - GLIDE stays unbound (falls back to FALL)   */
    { FA_CS_CLIMB,       24,  24,  35, 1 },  /* state 28/29 (PL-102) */
    { FA_CS_PUSH,       172, 172, 190, 1 },  /* state 32/33 (PL-103) */
    { FA_CS_THROW_FWD,  273, 273, 296, 0 },  /* state 30/31 snowball (spawn f291) */
    { FA_CS_THROW_UP,   273, 273, 296, 0 },
    { FA_CS_KO,          36,  44,  48, 1 },  /* state 36/37 */
    { FA_CS_IDLE_B,     137, 146, 159, 0 },  /* state 17 idle (loop point 146) */
    { FA_CS_SWAP,       137, 146, 150, 1 },  /* switch: turn to camera, loop 146-150 */
    { FA_CS_SWAP_END,   150, 150, 159, 0 },  /* the turn-back, once, at the end */
};

static void bind_kid(fa_cs_anim *a, const kid_clip *t, int n)
{
    for (int i = 0; i < n; i++)
        fa_cs_anim_bind_frames(a, t[i].pose, t[i].cur, t[i].loop_first,
                               t[i].end, t[i].loop);
}

static void load_kids(slice *s)
{
    const char *w01[2] = { "Animation/PINGUIN.W01", "Animation/MILCHSCHNITTE.W01" };

    s->have_kids = 0;
    for (int k = 0; k < 2; k++) {
        char wp[700];
        snprintf(wp, sizeof wp, "%s/%s", s->gdata, w01[k]);
        if (fa_cs_sheet_open(&s->kid_sheet[k], wp, NULL, -1) != 0) {
            printf("kid %d: no sprite sheet at %s (using a box)\n", k, wp);
            return;
        }
        fa_cs_anim_init(&s->kid_anim[k], &s->kid_sheet[k]);
        fa_cs_anim_set_period(&s->kid_anim[k], 2);   /* PL-098: 30 fps */
    }
    bind_kid(&s->kid_anim[0], PENGUIN_CLIPS,
             (int)(sizeof PENGUIN_CLIPS / sizeof *PENGUIN_CLIPS));
    bind_kid(&s->kid_anim[1], MILCH_CLIPS,
             (int)(sizeof MILCH_CLIPS / sizeof *MILCH_CLIPS));
    s->have_kids = 1;
    printf("kids: penguin %d frames, Milchschnitte %d frames\n",
           fa_cs_sheet_frame_count(&s->kid_sheet[0]),
           fa_cs_sheet_frame_count(&s->kid_sheet[1]));
}

static void free_kids(slice *s)
{
    if (!s->have_kids) return;
    for (int k = 0; k < 2; k++) {
        fa_cs_anim_free(&s->kid_anim[k]);
        fa_cs_sheet_close(&s->kid_sheet[k]);
    }
    s->have_kids = 0;
}

/* Map the player state machine to a sprite pose. */
static fa_cs_pose kid_pose(const fa_player *p)
{
    if (p->throw_anim > 0) return FA_CS_THROW_FWD;   /* up-throw: exe follow-up */
    switch (p->state) {
    case FA_PST_STAND:
        if (p->idle_kind == 1) return FA_CS_IDLE_A;
        if (p->idle_kind == 2) return FA_CS_IDLE_B;
        return FA_CS_STAND;
    case FA_PST_WALK:   return FA_CS_WALK;
    case FA_PST_CROUCH: return FA_CS_CROUCH;
    case FA_PST_JUMP:   return FA_CS_JUMP_RISE;
    case FA_PST_FALL:   return p->gliding ? FA_CS_GLIDE : FA_CS_JUMP_FALL;
    case FA_PST_CLIMB:  return FA_CS_CLIMB;
    case FA_PST_PUSH:   return FA_CS_PUSH;
    case FA_PST_SWAP:   return FA_CS_SWAP;
    default:            return FA_CS_STAND;
    }
}

/*
 * RRR-54 / PL-142: has this world's tutorial been cleared? The exe reads byte
 * world-1 of GData\Save\tut.ini (4 raw bytes) at level load; a 0 byte means
 * "play WeltNt". Missing file -> every tutorial shows. fa_vfs classifies
 * tut.ini to the install dir beside GData (RRR-39).
 */
static int slice_tut_seen(const char *gdata, int world)
{
    fa_vfs v;
    if (fa_vfs_init_default(&v, gdata, "FreshAdventures") != 0) return 0;
    return fa_vfs_tut_world_seen(&v, world);
}

/* The .w01/.w02 suffix for a normal play launch of `world`: "t" (tutorial)
 * until tut.ini says that world is cleared, else "" (0x411682). */
static const char *slice_world_suffix(const char *gdata, int world, int force_tut)
{
    return (force_tut || !slice_tut_seen(gdata, world)) ? "t" : "";
}

static void enter_world(slice *s, int world)
{
    slice_audio_hush(s);
    if (s->menu) { fa_menu_free(s->menu); s->menu = NULL; }
    const char *suf = slice_world_suffix(s->gdata, world, 0);
    if (load_world(s, s->gdata, world, suf, suf) == 0) {
        s->have_map = 1;
        s->use_player = 1;
        s->world = world;
        s->in_end = 0;
        s->end_pending = 0;
        s->boss_win_timer = 0;
        s->score = 0;               /* 0x41154C: a fresh run starts at 0 */
        if (s->audio && world >= 1 && world <= 4)
            fa_audio_event(s->audio, (fa_snd_event)(FA_SND_MUSIC_W1 + world - 1));
        fa_camera_init(&s->cam, FA_FB_W, FA_FB_H, s->map.world_w, s->map.world_h);
        wire_level(s);
        free_kids(s);
        load_kids(s);
        fa_hud_free(s->hud);
        s->hud = fa_hud_load(s->gdata);
        printf("hud: %s\n", s->hud ? "loaded" : "absent");
        printf("entered Welt%d%s%s: grid %dx%d, world %dx%d px\n", world,
               *suf ? suf : "", *suf ? " (tutorial - tut.ini byte 0)" : "",
               s->map.info.grid_w, s->map.info.grid_h,
               s->map.world_w, s->map.world_h);
    } else {
        printf("could not load Welt%d from %s\n", world, s->gdata);
        s->want_quit = 1;
    }
}

/*
 * The boss arena. The exe gates it on all six recipe pieces: hud_draw's
 * caller (0x411365) scans the 6 words at 0x45EFD4 and, when every one is set,
 * writes game state 20 (0x45F008) with target level = world + 4 (0x4DAB5C) -
 * the WeltNE map. WeltNE has no own .W01, so it renders on WeltN.W01.
 */
static void enter_end(slice *s)
{
    slice_audio_hush(s);
    int world = s->world;
    if (load_world(s, s->gdata, world, "", "E") == 0) {
        s->have_map = 1;
        s->use_player = 1;
        s->in_end = 1;
        s->end_pending = 0;
        s->boss_win_timer = 0;
        if (s->audio) fa_audio_event(s->audio, FA_SND_MUSIC_BOSS);
        fa_camera_init(&s->cam, FA_FB_W, FA_FB_H, s->map.world_w, s->map.world_h);
        wire_level(s);
        free_kids(s);
        load_kids(s);
        fa_hud_free(s->hud);
        s->hud = fa_hud_load(s->gdata);
        printf("recipe complete -> Welt%dE (boss): world %dx%d px\n",
               world, s->map.world_w, s->map.world_h);
    } else {
        /* no boss map shipped for this world: stay put, do not re-trigger */
        s->end_pending = 0;
        s->in_end = 1;
        printf("recipe complete but Welt%dE is absent - staying in Welt%d\n",
               world, world);
    }
}

/*
 * RRR-53: the run is over (health hit 0, fa_death's 240-tick KO hold + 16-tick
 * fade have elapsed). The exe (0x41272D -> scene 15, 0x402DE4) plays Start.wav
 * and shows the CLASSIFICA / high-score screen for the world that was played,
 * with the run's score, then the world-select menu. There is no in-place
 * restart and no lives. Tear the level down and bring up that screen; a click
 * on it returns to the menu (the existing s->scores dismiss path, which now
 * also rebuilds the menu and zeroes the score).
 */
static void begin_after_death(slice *s, int won)
{
    slice_audio_hush(s);
    if (s->audio) fa_audio_event(s->audio, FA_SND_MENU_MUSIC);   /* Start.wav */

    fa_beh_free(s->beh);  s->beh = NULL;
    free_kids(s);
    fa_hud_free(s->hud);  s->hud = NULL;
    s->have_map = 0;
    s->use_player = 0;
    s->in_end = 0;
    s->end_pending = 0;
    s->boss_win_timer = 0;

    s->scores = fa_hiscore_load(s->gdata);
    if (s->scores) {
        /* the run was in a world: show that world's table + boss portrait
         * (Gegner.w01 frame 0x4DABD4), and offer the run's score to the
         * table - it becomes an editable name row if it places. */
        fa_hiscore_begin(s->scores, s->world - 1, s->score, 1);
    } else {
        slice_set_menu(s, fa_menu_load(s->gdata));      /* no assets: menu */
    }

    fa_death_init(&s->death);
    printf("%s (score %d) -> CLASSIFICA (Welt%d), then the menu\n",
           won ? "World cleared - boss down" : "run over", s->score, s->world);
}

static int s_quit(void *user) { return ((slice *)user)->want_quit; }

/* Find the closest world icon in a requested screen direction. The menu
 * already owns the pixel rectangles, so controller navigation uses those
 * decoded regions instead of a second hand-written layout. */
static int menu_next_world(const fa_menu *m, int current, int dx, int dy)
{
    int ox = 400, oy = 300;
    if (current >= 0 && current < 4) {
        int x, y, w, h;
        if (fa_menu_hit_rect(m, current, &x, &y, &w, &h) == 0) {
            ox = x + w / 2;
            oy = y + h / 2;
        }
    }

    int best = -1, best_score = 0x7fffffff;
    for (int i = 0; i < 4; i++) {
        if (i == current) continue;
        int x, y, w, h;
        if (fa_menu_hit_rect(m, i, &x, &y, &w, &h) != 0) continue;
        int vx = x + w / 2 - ox;
        int vy = y + h / 2 - oy;
        int primary = dx ? vx * dx : vy * dy;
        if (primary <= 0) continue;
        int secondary = dx ? abs(vy) : abs(vx);
        int distance = vx * vx + vy * vy;
        int score = primary * 10000 + secondary * 100 + distance;
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

static int menu_move_focus(slice *s, const fa_frame_input *fi)
{
    uint32_t keys = fi->actions_pressed;
    uint32_t pad  = fi->pad_pressed;
    uint32_t nav_edges = keys & ((1u << FA_ACT_LEFT) | (1u << FA_ACT_RIGHT) |
                                 (1u << FA_ACT_UP) | (1u << FA_ACT_DOWN));
    nav_edges |= pad & ((1u << FA_PAD_DPAD_LEFT) | (1u << FA_PAD_DPAD_RIGHT) |
                        (1u << FA_PAD_DPAD_UP) | (1u << FA_PAD_DPAD_DOWN));
    uint32_t fresh_edges = nav_edges & ~s->menu_nav_seen;
    s->menu_nav_seen = nav_edges;
    if (!nav_edges) s->menu_nav_seen = 0;
    if (!fresh_edges) return 0;

    int dx = 0, dy = 0;

    if (fresh_edges & ((1u << FA_ACT_LEFT) | (1u << FA_PAD_DPAD_LEFT)))
        dx = -1;
    else if (fresh_edges & ((1u << FA_ACT_RIGHT) | (1u << FA_PAD_DPAD_RIGHT)))
        dx = 1;
    else if (fresh_edges & ((1u << FA_ACT_UP) | (1u << FA_PAD_DPAD_UP)))
        dy = -1;
    else if (fresh_edges & ((1u << FA_ACT_DOWN) | (1u << FA_PAD_DPAD_DOWN)))
        dy = 1;

    int had_focus = s->menu_focus >= 0 && s->menu_focus < 4;
    int current = had_focus ? s->menu_focus : 0;
    int next = menu_next_world(s->menu, current, dx, dy);
    if (next < 0 && !had_focus) next = 0;
    if (next < 0) return 0;
    s->menu_focus = next;
    return 1;
}

static void s_sim(uint64_t tick, const void *input, void *user)
{
    slice *s = (slice *)user;
    const fa_frame_input *fi = (const fa_frame_input *)input;
    uint32_t m = fi->actions;
    slice_merge_pad_movement(fi, &m);
    (void)tick;

    if (s->pending_world) {
        int wd = s->pending_world;
        s->pending_world = 0;
        enter_world(s, wd);
        return;
    }
    if (s->end_pending) { enter_end(s); return; }

    /* RRR-54: the tutorial's last Kinder Paradiso finished its line - reload
     * the same world as the normal level (tut.ini is already written). The
     * exe (0x4126F3) keeps the score across this reload. */
    if (s->tut_reload_pending) {
        s->tut_reload_pending = 0;
        int wd = s->world, sc = s->score;
        enter_world(s, wd);
        s->score = sc;
        return;
    }

    /* RRR-54: the credits screen. Credit1..4.bmp in order, then ENDTITLES,
     * then the menu. A page auto-advances after FA_CREDITS_PAGE_DWELL ticks;
     * a click or a JUMP/FIRE press skips to the next page at once (the exe
     * folds the same input into 0x4DAB44). */
    if (s->credits) {
        uint32_t act = fi->actions;
        uint32_t press = act & ~s->cr_act_was;
        s->cr_act_was = act;
        int skip = (fi->btn_pressed & 1u) != 0 ||
                   (press & ((1u << FA_ACT_JUMP) | (1u << FA_ACT_FIRE))) != 0;
        fa_credits_tick(s->credits, skip);
        if (fa_credits_done(s->credits)) {
            fa_credits_free(s->credits);
            s->credits = NULL;
            slice_set_menu(s, fa_menu_load(s->gdata));
            slice_audio_hush(s);
            if (s->audio) fa_audio_event(s->audio, FA_SND_MENU_MUSIC);
            printf("credits done -> the menu\n");
        }
        return;
    }

    /* CLASSIFICA -> the high-score screen. Reached from the menu (CLASSIFICA
     * button) or after a death (begin_after_death). A click returns to the
     * menu; if the menu was torn down (a death), rebuild it and zero the
     * score for the next run (0x41154C). */
    if (s->show_scores && !s->scores) {
        s->scores = fa_hiscore_load(s->gdata);
        s->show_scores = 0;
        if (s->scores)                       /* from the menu: plain view */
            fa_hiscore_begin(s->scores, 0, -1, 0);
    }
    if (s->scores) {
        fa_hiscore_in hi;
        memset(&hi, 0, sizeof hi);
        hi.ptr_x     = fi->ptr_x;
        hi.ptr_y     = fi->ptr_y;
        hi.click     = (fi->btn_pressed & 1u) != 0;
        hi.text      = (const char *)fi->text;
        hi.text_n    = fi->text_n;
        hi.backspace = (fi->edit_pressed & FA_EDIT_BACKSPACE) != 0;
        hi.enter     = (fi->edit_pressed & FA_EDIT_ENTER) != 0;
        hi.escape    = (fi->edit_pressed & FA_EDIT_ESCAPE) != 0;

        fa_hiscore_ev he;
        fa_hiscore_tick(s->scores, &hi, &he);

        if (he.hover_sound && s->audio)
            fa_audio_event(s->audio, FA_SND_MENU_HOVER);   /* alsf08 (PL-118) */

        if (he.wrote)   /* fa_hiscore persisted user:Highscore{n}.dat (RRR-39) */
            printf("high score entered -> Highscore%d.dat\n", he.wrote_world + 1);

        if (he.leave) {
            fa_hiscore_free(s->scores);
            s->scores = NULL;
            if (!s->menu) {                  /* came from a death */
                slice_set_menu(s, fa_menu_load(s->gdata));
                s->score = 0;
            } else {
                slice_focus_first_menu(s);
            }
        }
        return;
    }

    if (s->menu) {
        int navigated = menu_move_focus(s, fi);
        int pick = fa_menu_pick(s->menu, fi->ptr_x, fi->ptr_y);
        int mouse_click = (fi->btn_pressed & 1u) != 0;
        if (!navigated && fi->ptr_moved) s->menu_focus = pick;
        if (mouse_click && pick >= 0) s->menu_focus = pick;
        int focus = s->menu_focus >= 0 ? s->menu_focus : pick;
        int held = (fi->btn_down & 1u) != 0 ||
                   (fi->pad_down & (1u << FA_PAD_A)) != 0;
        for (int i = 0; i < fa_menu_button_count(s->menu); i++)
            fa_menu_set_state(s->menu, i,
                i == focus ? (held ? FA_MENU_PRESS : FA_MENU_HOVER)
                          : FA_MENU_REST);
        if (s->audio && focus >= 0 && focus != s->hover)
            fa_audio_event(s->audio, FA_SND_MENU_HOVER);   /* alsf08 (PL-118) */
        s->hover = focus;
        int confirm = (mouse_click && pick >= 0) ||
                      (fi->pad_pressed & (1u << FA_PAD_A)) != 0 ||
                      (fi->edit_pressed & FA_EDIT_ENTER) != 0;
        if (focus >= 0 && confirm) {
            if (focus <= 3)       s->pending_world = focus + 1; /* GIUNGLA=1 */
            else if (focus == 4)  s->show_scores = 1;            /* CLASSIFICA */
            else if (focus == 5)  s->want_quit = 1;              /* ESCI */
        }
        return;
    }

    if (s->use_player) {
        int out_char = s->pl.character;

        /* DEV: P toggles free / no-clip movement - fly straight to the
         * recipe pieces instead of walking the level. Pickup collection,
         * the camera and the 6-piece boss gate all still run; gravity,
         * terrain collision, lifts and enemy damage are bypassed. */
        if (fi->dbg_pressed & FA_DBG_FREEMOVE) {
            s->freemove = !s->freemove;
            printf("free-move %s\n", s->freemove ? "ON" : "OFF");
        }

        /* DEV: I skips straight to this world's boss arena (Welt<N>E) so a
         * boss can be tested without replaying the whole level. */
        if ((fi->dbg_pressed & FA_DBG_BOSS) && !s->in_end && !s->end_pending &&
            s->world >= 1 && s->world <= 4) {
            for (int i = 0; i < 6; i++) s->items[i] = 1;   /* recipe "complete" */
            s->end_pending = 1;
            printf("skip -> Welt%dE boss arena\n", s->world);
        }

        /* RRR-53: the kid is dead. The run is over - but the LEVEL KEEPS
         * RUNNING for the 240-tick KO hold (exe case 1 / 0x41110B loops the
         * whole entity table with no death guard), then a 16-tick fade, then
         * the CLASSIFICA screen + the menu (begin_after_death). The player is
         * locked out of input and just runs its physics (gravity + terrain)
         * so the corpse arcs down and lands; the render forces the KO pose. */
        if (fa_death_phase_of(&s->death) != FA_DEATH_ALIVE) {
            fa_death_tick(&s->death);
            if (fa_death_phase_of(&s->death) == FA_DEATH_DONE) {
                begin_after_death(s, 0);
                return;
            }
            if (s->beh) {
                fa_beh_begin_frame(s->beh, fa_player_px(&s->pl),
                                   fa_player_py(&s->pl), 40, 190,
                                   (s->pl.facing == FA_FACE_RIGHT) ? 1 : -1,
                                   999 /*i-frames: no more damage*/,
                                   s->pl.snow, FA_MAX_SNOWBALLS);
            }
            if (s->ents)
                fa_entity_tick(s->ents, s->cam.x, s->cam.y, FA_FB_W, FA_FB_H);
            if (s->beh) { int kb = 0; (void)fa_beh_post(s->beh, &kb); }
            slice_posloops(s);
            fa_player_tick(&s->pl, 0);          /* no input; body settles */
            fa_camera_follow(&s->cam, fa_player_px(&s->pl), fa_player_py(&s->pl));
            if (s->have_kids) {
                int k = s->pl.character & 1;
                fa_cs_anim_set_period(&s->kid_anim[k], 2);
                fa_cs_anim_set(&s->kid_anim[k], FA_CS_KO, s->pl.facing);
                fa_cs_anim_tick(&s->kid_anim[k]);
            }
            return;
        }

        /* RRR-59: the boss is down, its 7th recipe piece dropped, and the
         * player has caught it (fa_beh beh_i7). The level is complete -
         * to the CLASSIFICA / high-score screen (owner decision). */
        if (s->in_end && s->beh && fa_beh_recipe_done(s->beh)) {
            if (s->audio) fa_audio_event(s->audio, FA_SND_PICKUP);
            printf("World %d complete (score %d) -> CLASSIFICA\n",
                   s->world, s->score);
            begin_after_death(s, 1);
            return;
        }

        /* RRR-50: tick the object runtime FIRST so the player's collision
         * probe sees lifts / blocks / fallers at their new positions this
         * tick (fixes the raft fall-pose and lets blocks stay solid). */
        if (s->beh) {
            fa_beh_set_character(s->beh, s->pl.character & 1);
            fa_beh_set_ammo_dirty(s->beh, s->dirty_shot);
            fa_beh_begin_frame(s->beh, fa_player_px(&s->pl), fa_player_py(&s->pl),
                               40, (s->pl.state == FA_PST_CROUCH) ? 100 : 190,
                               (s->pl.facing == FA_FACE_RIGHT) ? 1 : -1,
                               s->hurt_cd, s->pl.snow, FA_MAX_SNOWBALLS);
        }
        if (s->ents)
            fa_entity_tick(s->ents, s->cam.x, s->cam.y, FA_FB_W, FA_FB_H);

        /* PRE-tick: if the kid rides a platform, plant his feet on the
         * (now-moved) deck so fa_player_tick's own collision grounds him
         * THIS tick - otherwise crouch / throw (gated on the post-collide
         * on_ground) fail while the platform moves (owner playtest). */
        int on_lift = 0, lift_top = 0, lift_dx = 0;
        if (!s->freemove && s->ents && s->pl.vy >= 0 && s->pl.state != FA_PST_JUMP) {
            int cdy = 0;
            if (fa_entity_ride(s->ents, fa_player_px(&s->pl),
                               fa_player_py(&s->pl), s->pl.t.body_hw,
                               &lift_top, &lift_dx, &cdy)) {
                on_lift = 1;
                s->pl.y = FA_FIX(lift_top);
                s->pl.vy = 0;
                s->pl.on_ground = 1;
            }
        }

        if (s->freemove) {
            /* direct fly: arrows move, hold A for a fast dash. No physics. */
            int spd = (m & (1u << FA_ACT_JUMP)) ? 12 : 6;
            int dx = 0, dy = 0;
            if (m & (1u << FA_ACT_LEFT))  dx -= spd;
            if (m & (1u << FA_ACT_RIGHT)) dx += spd;
            if (m & (1u << FA_ACT_UP))    dy -= spd;
            if (m & (1u << FA_ACT_DOWN))  dy += spd;
            s->pl.x += FA_FIX(dx);
            s->pl.y += FA_FIX(dy);
            if (dx) s->pl.facing = (dx < 0) ? FA_FACE_LEFT : FA_FACE_RIGHT;
            s->pl.vx = s->pl.vy = 0;
            s->pl.on_ground = 1;
            s->pl.gliding = s->pl.throw_anim = 0;
            s->pl.state = (dx || dy) ? FA_PST_WALK : FA_PST_STAND;
            if (s->pl.x < 0) s->pl.x = 0;
            if (s->pl.y < FA_FIX(16)) s->pl.y = FA_FIX(16);
            if (s->pl.x > FA_FIX(s->map.world_w)) s->pl.x = FA_FIX(s->map.world_w);
            if (s->pl.y > FA_FIX(s->map.world_h)) s->pl.y = FA_FIX(s->map.world_h);
        } else {
            if (s->ammo <= 0) m &= ~(1u << FA_ACT_FIRE);   /* AC4: no snowballs */
            fa_player_tick(&s->pl, m);   /* FA_PI_* == (1u<<FA_ACT_*) */
        }
        /* the swap voice line starts when the swap starts; the swap lock
         * length is that line's duration (PL-104). */
        int swap_now  = (s->pl.state == FA_PST_SWAP);
        int jump_now  = (s->pl.state == FA_PST_JUMP);
        int thr_now   = (s->pl.throw_anim > 0);
        int glide_now = (s->pl.gliding != 0);       /* penguin only */
        int c1 = (s->pl.character & 1);
        if (s->audio) {
            if (swap_now && !s->swap_was)
                fa_audio_event(s->audio, out_char == 0 ? FA_SND_SWAP_P2M
                                                       : FA_SND_SWAP_M2P);
            /* alsf01 only on a REAL jump (jump_hold is set by the input-edge
             * jump). A hit / hazard knockback also puts the kid in JUMP state
             * but must not play the jump sound (owner: "plays the jump sound
             * when hurt") - the hit sound below owns that moment. */
            if (jump_now && !s->jump_was && s->pl.jump_hold > 0)
                fa_audio_event(s->audio, c1 ? FA_SND_JUMP_M : FA_SND_JUMP_P);
            if (thr_now && !s->thr_was)
                fa_audio_event(s->audio, c1 ? FA_SND_THROW_M : FA_SND_THROW_P);
            if (glide_now && !s->glide_was)         /* alsf02, glide entry */
                fa_audio_event(s->audio, FA_SND_GLIDE);
            if (!glide_now && s->glide_was)         /* exe stops lane 0 on */
                fa_audio_stop(s->audio, 0);         /* glide exit: 0x422e04(0) */
        }
        if (thr_now && !s->thr_was && s->ammo > 0) s->ammo--;   /* AC4: 0x45ED34-- */
        s->swap_was  = swap_now;
        s->jump_was  = jump_now;
        s->thr_was   = thr_now;
        s->glide_was = glide_now;

        /* --- RRR-50: player <-> object coupling --- */
        if (s->ents) {
            int px = fa_player_px(&s->pl), py = fa_player_py(&s->pl);
            int face = (s->pl.facing == FA_FACE_RIGHT) ? 1 : -1;

            /* the pickup / hit test spans the kid's whole sprite plus the
             * overhead reach (items sit in tree-tops and along the vines;
             * the exe collects during the climb too) - not the narrow 10 px
             * collision core (owner playtest) */
            int gw  = s->pl.t.body_hw + 26;              /* ~72 px wide     */
            int gcy = py - s->pl.t.body_h;               /* head-ish centre */
            int gh  = s->pl.t.body_h + 10;               /* reach overhead  */

            /* Fettalatte shoving a block (PL-135): a committed clip; on its
             * frame-176 event (~tick 8) one impulse sets the block's float
             * vx to +/-7.0 and beh_block slides + friction-decays it. The
             * kid does NOT ride the block - the shove ends the clip, then the
             * player must walk up to the block again to shove it once more
             * (owner request). */
            if (s->pl.state == FA_PST_PUSH && s->beh) {
                int probx = px + face * (s->pl.t.body_hw + 8);
                if (s->pl.push_timer == 8 &&
                    fa_beh_push(s->beh, probx, py - 100, face) && s->audio)
                    fa_audio_event(s->audio, FA_SND_PUSH);   /* sound id 6 */
            }

            /* POST-tick: carry the platform's horizontal drift and re-plant
             * the feet (fa_player_tick's collide may have nudged them).
             * Skipped once the kid jumps off (state JUMP / vy < 0). Every
             * action state is preserved. */
            if (on_lift && s->pl.state != FA_PST_JUMP) {
                s->pl.x += FA_FIX(lift_dx);
                s->pl.y  = FA_FIX(lift_top);
                if (s->pl.vy > 0) s->pl.vy = 0;
                s->pl.on_ground = 1;
                if (s->pl.state == FA_PST_FALL)
                    s->pl.state = (s->pl.vx != 0) ? FA_PST_WALK : FA_PST_STAND;
            }

            /* collect BONUS / POWERUP pickups (PL-131) */
            int got = fa_entity_collect(s->ents, px, gcy, gw, gh, beh_pickup, s);
            if (got) {
                if (s->audio) fa_audio_event(s->audio, FA_SND_PICKUP);
                printf("pickup x%d (score %d health %d ammo %d)\n",
                       got, s->score, s->health, s->ammo);
                /* AC4/boss gate (exe 0x411365): all 6 recipe pieces -> WeltNE */
                if (!s->in_end && s->world >= 1 && s->world <= 4) {
                    int all = 1;
                    for (int i = 0; i < 6; i++) if (!s->items[i]) { all = 0; break; }
                    if (all) s->end_pending = 1;
                }
            }

            /* RRR-51: enemy contact + enemy projectiles. No stomp - falling
             * onto an enemy is the same 20-damage overlap (0x41A3E0 never
             * reads player vy); only the 120-tick i-frames suppress it. */
            int kb = 0;
            int dmg = s->beh ? fa_beh_post(s->beh, &kb) : 0;
            slice_posloops(s);
            if (dmg && s->hurt_cd == 0 && !s->freemove) {
                s->health -= dmg;
                s->hurt_cd = 120;              /* 0x41A538: 0x78 ticks */
                if (kb) {
                    s->pl.vy = -FA_FIX(6);
                    s->pl.on_ground = 0;
                }
                if (s->audio)
                    fa_audio_event(s->audio, (s->pl.character & 1)
                                   ? FA_SND_HIT_M : FA_SND_HIT_P);
                printf("hit! health %d\n", s->health);
            }

            /* RRR-53 (fcn.0041A290): standing on a plane-2 hazard tile
             * (attr & 0x80) deals 20 + 120 i-frames when not already in
             * i-frames, plays the character hit sound, and ALWAYS bounces
             * the kid up (vy = -20.0) so he cannot sit in it. */
            if (!s->freemove && s->health > 0 &&
                slice_hazard(&s->map, fa_player_px(&s->pl),
                             fa_player_py(&s->pl))) {
                if (s->hurt_cd == 0) {
                    s->health -= 20;
                    s->hurt_cd = 120;
                    if (s->audio)
                        fa_audio_event(s->audio, (s->pl.character & 1)
                                       ? FA_SND_HIT_M : FA_SND_HIT_P);
                    printf("hazard! health %d\n", s->health);
                }
                s->pl.vy = -FA_FIX(20);        /* 0x431A20(0xFFFEC000) */
                s->pl.on_ground = 0;
            }
            if (s->hurt_cd > 0) s->hurt_cd--;

            /* RRR-59: the boss is down - its 7th recipe piece drops in
             * (fa_beh beh_i7); catch it to finish the level. */
            if (s->in_end && s->beh && fa_beh_boss_defeated(s->beh) &&
                !s->boss_win_timer) {
                s->boss_win_timer = 1;          /* one-shot: log it once */
                printf("boss defeated in Welt%dE (score %d) - catch the "
                       "7th recipe piece\n", s->world, s->score);
            }

            /* RRR-53: health hit 0 -> the run is over. Start the KO sequence
             * (exe 0x417419: player state -> KO, 0x4E0B44 = 0xF0). The exe
             * launches the body once (0x431A00/0x431A20: vx +/-18, vy -6, in
             * 20.12 -> px/tick); the shared integrator then arcs + lands it.
             * Handled by the fa_death branch at the top of the block from the
             * next tick. */
            if (s->health <= 0 && !fa_death_active(&s->death) && !s->freemove) {
                int face = (s->pl.character & 1) ? 1 : -1;   /* 0x4E1020 sign */
                s->health = 0;
                s->pl.vx = FA_FIX(18) * face;
                s->pl.vy = -FA_FIX(6);
                s->pl.on_ground = 0;
                fa_death_begin(&s->death, s->pl.character & 1,
                               s->pl.facing == FA_FACE_RIGHT ? 1 : -1);
                printf("run over in Welt%d%s (score %d) -> KO %d t, fade %d t, "
                       "then CLASSIFICA\n", s->world, s->in_end ? "E" : "",
                       s->score, FA_DEATH_HOLD_TICKS, FA_DEATH_FADE_TICKS);
            }
        }

        fa_camera_follow(&s->cam, fa_player_px(&s->pl), fa_player_py(&s->pl));
        if (s->have_kids) {
            int k = s->pl.character & 1;
            fa_cs_pose pose = kid_pose(&s->pl);
            /* Fettalatte's swap ends with a turn-back (150-159); the last
             * swap_end_c1 ticks of the c1 swap show it (PL-104) */
            if (pose == FA_CS_SWAP && (s->pl.character & 1) &&
                s->pl.swap_timer <= s->pl.t.swap_end_c1)
                pose = FA_CS_SWAP_END;
            /* play the stand-up frames once when DOWN is released on the ground */
            int crouch = (pose == FA_CS_CROUCH);
            if (s->kid_was_crouch && !crouch && s->pl.on_ground)
                s->kid_rise = 14;
            s->kid_was_crouch = crouch;
            if (s->kid_rise > 0) { s->kid_rise--; pose = FA_CS_CROUCH_RISE; }
            /* the climb pose freezes while the player is not moving (PL-102) */
            fa_cs_anim_set_period(&s->kid_anim[k],
                (s->pl.state == FA_PST_CLIMB && !s->pl.climb_moving) ? 0 : 2);
            /* the climb sheet is a front-on pose - pin it to one orientation so
             * it never mirrors with input. The correct look is the mirror of
             * the raw art (owner: Fettalatte must read white-left / red-right). */
            int cface = s->pl.facing;
            if (pose == FA_CS_CLIMB && s->kid_anim[k].sheet)
                cface = -s->kid_anim[k].sheet->base_facing;
            fa_cs_anim_set(&s->kid_anim[k], pose, cface);
            fa_cs_anim_tick(&s->kid_anim[k]);
        }
        return;
    }
    int vx = 0, vy = 0;
    if (m & (1u << FA_ACT_LEFT))  vx -= 4;
    if (m & (1u << FA_ACT_RIGHT)) vx += 4;
    if (m & (1u << FA_ACT_UP))    vy -= 4;
    if (m & (1u << FA_ACT_DOWN))  vy += 4;
    if (s->have_map) fa_camera_move(&s->cam, vx, vy);
    else { s->cam.x += vx; s->cam.y += vy; }
}

/* RRR-55: the exe installs the player renderer (0x41A780) as the PLANE 2
 * per-plane hook (0x417150 -> 0x432820(2, ...)), so the kid + thrown
 * snowballs draw mid-scene - AFTER plane-2 tiles and band-0 entities, and
 * BEFORE plane-2 band-2 entities and the foreground tile planes 3/4. That
 * is what lets the jungle spikes and the factory pipes pass in front of
 * the kid. Enemy projectiles, HUD and the death fade stay on top (their
 * exe passes run after the whole scene). */
static void slice_plane_hook(void *ud, const fa_surface *dst,
                             const fa_camera *cam, int plane)
{
    if (plane != 2) return;
    slice *s = (slice *)ud;
    if (!s->use_player) return;

    int px = fa_player_px(&s->pl) - cam->x;
    int py = fa_player_py(&s->pl) - cam->y;
    int k = s->pl.character & 1;
    long drawn = -1;
    /* RRR-53: blink the kid through the i-frame window - hidden on
     * alternate ~7 px of the countdown. Not while dying. */
    int blink = s->hurt_cd > 0 && !fa_death_active(&s->death) &&
                ((s->hurt_cd >> 2) & 1);
    if (s->have_kids && !blink)
        drawn = fa_cs_anim_draw(&s->kid_anim[k], dst, px, py, NULL);
    else if (blink)
        drawn = 0;
    if (drawn < 0) {
        fa_rect body = { px - 8, py - 40, 16, 40 };
        fa_fill(dst, &body, NULL, fa_rgb565(240, 210, 60));
    }
    /* thrown snowball = PINGUIN.W01 frame 261 (0x105, Schneeball); the "dirty"
     * black ball after collect_dirtyballs is frame 232 (0xE8) - the exe's
     * snowball-slot type doubles as the sprite frame (spawn 0x41A268). */
    const fa_w01 *proj_w = s->have_kids ? &s->kid_sheet[0].w01 : NULL;
    int proj_f = s->dirty_shot ? 232 : 261;
    for (int i = 0; i < FA_MAX_SNOWBALLS; i++) {
        if (!s->pl.snow[i].alive) continue;
        int sx = (int)(s->pl.snow[i].x >> 16) - cam->x;
        int sy = (int)(s->pl.snow[i].y >> 16) - cam->y;
        if (!blit_w01_centered(dst, proj_w, proj_f, sx, sy)) {
            fa_rect b = { sx - 3, sy - 3, 6, 6 };
            fa_fill(dst, &b, NULL,
                    s->dirty_shot ? fa_rgb565(40, 40, 48)
                                  : fa_rgb565(255, 255, 255));
        }
    }
}

static void s_render(double alpha, uint16_t *fb, int w, int h, size_t pitch,
                     void *user)
{
    slice *s = (slice *)user;
    (void)alpha;
    fa_surface dst;
    fa_surface_wrap(&dst, fb, w, h, pitch);

    if (s->credits) {
        fa_credits_render(s->credits, &dst);
        return;
    }

    if (s->scores) {
        fa_hiscore_render(s->scores, &dst);
        return;
    }

    if (s->menu) {
        fa_menu_render(s->menu, &dst);   /* hover / pressed art is the feedback */
        return;
    }

    if (s->have_map) {
        fa_scene sc = { &s->bg, &s->map, s->tiles, s->ents, s->grid, NULL, NULL };
        sc.on_plane = slice_plane_hook;   /* RRR-55: kid + snowballs at plane 2 */
        sc.on_plane_ud = s;
        fa_render_scene(&dst, &sc, &s->cam);
        if (s->use_player) {
            /* enemy projectiles render from the THROWER's own sheet at a
             * per-enemy frame (0x40AF80): kong/ape 5 -> frame 16 is a banana,
             * yeti 7 -> 25, snowman 8 -> 54, egg 12 -> 23, bear 15 -> 53,
             * gorilla boss 10 -> 87 (coconut). */
            for (int i = 0; s->beh && i < FA_BEH_PROJ_MAX; i++) {
                int wx, wy, oo = 0;
                if (!fa_beh_projectile(s->beh, i, &wx, &wy, &oo)) continue;
                int sx = wx - s->cam.x, sy = wy - s->cam.y;
                int pf;
                switch (oo) {
                    case 5:  pf = 16; break;
                    case 7:  pf = 25; break;
                    case 8:  pf = 54; break;
                    case 12: pf = 23; break;
                    case 15: pf = 53; break;
                    case 10: pf = 87; break;
                    case 14: pf = 188; break;   /* robot boss bolt (RRR-61) */
                    case 18: pf = 50; break;    /* octopus milk particle (RRR-62) */
                    default: pf = -1; break;
                }
                const fa_w01 *pw = (pf >= 0 && s->ents)
                                 ? fa_entity_obj_sheet(s->ents, oo) : NULL;
                if (!pw || !blit_w01_centered(&dst, pw, pf, sx, sy)) {
                    fa_rect b = { sx - 4, sy - 4, 8, 8 };
                    fa_fill(&dst, &b, NULL, fa_rgb565(150, 90, 40));
                }
            }
            /* RRR-51 AC5 + RRR-59: the status panel over the scene. In the
             * boss arena (exe hud_draw 0x408B9B, flag 0x45ECBC) the boss bar
             * REPLACES the 6 recipe-piece icons - BossInterface frame + a
             * Boss/Energy fill by HP + a Bosspics portrait. -1 boss_hp = the
             * normal 6-icon HUD. */
            int boss_hp = (s->in_end && s->beh) ? fa_beh_boss_hp(s->beh) : -1;
            if (s->hud)
                fa_hud_render(s->hud, &dst, s->score, s->health, s->ammo,
                              s->dirty_shot, s->items, s->pl.character,
                              boss_hp, s->world - 1);

            /* RRR-53: the end-of-run fade (exe 0x45ED42 = 0x10, step 1). No
             * alpha blend on fa_surface, so a 16-step screen-door dissolve to
             * black - the exe's fades are dither (RRR-6). fade counts 16 -> 0;
             * row y goes black once (y & 15) >= fade. */
            int fade = fa_death_fade_amount(&s->death);
            if (fade > 0 && fade < FA_DEATH_FADE_TICKS) {
                for (int y = 0; y < h; y++) {
                    if ((y & 15) < fade) continue;
                    fa_rect r = { 0, y, w, 1 };
                    fa_fill(&dst, &r, NULL, 0);
                }
            }
        }
        return;
    }
    for (int y = 0; y < h; y++) {
        uint16_t *row = (uint16_t *)((uint8_t *)fb + (size_t)y * pitch);
        for (int x = 0; x < w; x++) {
            int gx = x + s->cam.x, gy = y + s->cam.y;
            row[x] = fa_rgb565((uint8_t)(gx >> 2), (uint8_t)(gy >> 2),
                               (uint8_t)((gx ^ gy) >> 3));
        }
    }
}

static int s_audio(int16_t *buf, int max_frames, int rate, int channels,
                   void *user)
{
    slice *s = (slice *)user;
    if (rate <= 0 || channels <= 0) return 0;

    /* RRR-46: with GData the real mixer owns the buffer (music + SFX + voice).
     * The 440 Hz tone is only the no-GData dev aid. */
    if (s->audio && channels == 2) {
        if (max_frames > 4096) max_frames = 4096;
        return fa_audio_mix(s->audio, buf, max_frames);
    }

    int chime = s->want_chime && s->chime_pos < rate;   /* 1 s startup tone */
    if (!s->tone && !chime) return 0;

    int emit = max_frames;
    if (!s->tone && emit > rate - s->chime_pos) emit = rate - s->chime_pos;

    const double step = 2.0 * 3.14159265358979 * 440.0 / rate;
    const int    ramp = rate / 20;                      /* 50 ms fades */
    for (int i = 0; i < emit; i++) {
        double amp = 4800.0;                            /* ~15 % */
        if (!s->tone) {
            int pos = s->chime_pos + i;
            int d = pos < rate - pos ? pos : rate - pos;
            if (d < ramp) amp *= (double)d / ramp;
        }
        int16_t v = (int16_t)(sin(s->tone_phase) * amp);
        s->tone_phase += step;
        if (s->tone_phase > 6.28318530718) s->tone_phase -= 6.28318530718;
        for (int c = 0; c < channels; c++) buf[i * channels + c] = v;
    }
    if (!s->tone) s->chime_pos += emit;
    return emit;
}

/*
 * Load a level under `maps`, tolerating the shipped case mix. `w01suf` and
 * `w02suf` name the background / map variant: "" + "" = the main world,
 * "t" + "t" = the tutorial (WeltNt), "" + "E" = the boss arena (WeltNE.W02
 * on the parent world's WeltN.W01 - there is no WeltNE.W01).
 */
static int load_world(slice *s, const char *maps, int world,
                      const char *w01suf, const char *w02suf)
{
    const char *variants[] = { "Welt%d%s.W01", "WELT%d%s.W01", "welt%d%s.w01" };
    const char *pvariants[] = { "Welt%d%s.W02", "WELT%d%s.W02", "welt%d%s.w02" };
    char path[600];

    for (unsigned v = 0; v < 3; v++) {
        char name[64];
        snprintf(name, sizeof name, variants[v], world, w01suf);
        snprintf(path, sizeof path, "%s/Maps/%s", maps, name);
        if (fa_w01_open_file(&s->bg, path) == 0) break;
        if (v == 2) return -1;
    }
    for (unsigned v = 0; v < 3; v++) {
        char name[64];
        snprintf(name, sizeof name, pvariants[v], world, w02suf);
        snprintf(path, sizeof path, "%s/Maps/%s", maps, name);
        if (fa_map_load_file(&s->map, path) == 0) {
            fa_tileset_free(s->tiles);
            s->tiles = fa_tileset_build(&s->bg, &s->map);
            fa_beh_free(s->beh);        /* drop behaviours before their store */
            s->beh = NULL;
            fa_entity_free(s->ents);
            s->ents = fa_entity_load(&s->map, maps);
            if (s->ents)
                printf("entities: %d placed, %d drawable, %d AOM defs\n",
                       fa_entity_count(s->ents), fa_entity_drawable(s->ents),
                       fa_entity_def_count(s->ents));
            return 0;
        }
        if (v == 2) { fa_w01_close(&s->bg); return -1; }
    }
    return -1;
}

static int dir_has_gdata(const char *dir)
{
    char p[600];
    const char *probes[] = {
        "%s/Pics/StartBG.bmp", "%s/Pics/startbg.bmp", "%s/Maps/Welt2.W02"
    };
    for (unsigned i = 0; i < 3; i++) {
        snprintf(p, sizeof p, probes[i], dir);
        FILE *f = fopen(p, "rb");
        if (f) { fclose(f); return 1; }
    }
    return 0;
}

/* Fill `out` with a GData path to try: --gdata, <exe dir>/GData, ./GData. */
static void find_gdata(const char *arg, char *out, size_t cap)
{
    if (arg && *arg) { snprintf(out, cap, "%s", arg); return; }

#if defined(_WIN32)
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (n > 0 && n < sizeof exe) {
        char *slash = strrchr(exe, '\\');
        if (slash) *slash = 0;
        char cand[MAX_PATH + 16];
        snprintf(cand, sizeof cand, "%s\\GData", exe);
        if (dir_has_gdata(cand)) { snprintf(out, cap, "%s", cand); return; }
    }
#endif
    snprintf(out, cap, "GData");
}

int main(int argc, char **argv)
{
    long frames = 0;
    const char *gdata_arg = NULL;
    int world = 0, tut = 0, end = 0, grid = 0, tone = 0, silent = 0, mute = 0, vol = -1;
    int show_credits = 0;
    int win_scale = 0, fullscreen = 0, crisp = 0;
    double ov_g = 0, ov_j = 0, ov_j2 = 0, ov_r = 0, ov_a = 0;
    int ov_bw = 0, ov_bh = 0, ov_cx = 0, ov_cy = 0, ov_cb = 0;
    long seed_arg = -1;              /* RRR-57: >=0 pins the enemy RNG seed */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--gdata") && i + 1 < argc)
            gdata_arg = argv[++i];
        else if (!strcmp(argv[i], "--world") && i + 1 < argc)
            world = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tut"))    tut = 1;
        else if (!strcmp(argv[i], "--credits")) show_credits = 1;  /* RRR-54 */
        else if (!strcmp(argv[i], "--end"))    end = 1;   /* boot into WeltNE */
        else if (!strcmp(argv[i], "--grid"))   grid = 1;
        else if (!strcmp(argv[i], "--tone"))   tone = 1;
        else if (!strcmp(argv[i], "--silent")) silent = 1;
        else if (!strcmp(argv[i], "--mute"))   mute = 1;
        else if (!strcmp(argv[i], "--vol") && i + 1 < argc) vol = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed_arg = strtol(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--scale") && i + 1 < argc) win_scale = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fullscreen")) fullscreen = 1;
        else if (!strcmp(argv[i], "--crisp"))      crisp = 1;
        else if (!strcmp(argv[i], "--gravity")   && i + 1 < argc) ov_g  = atof(argv[++i]);
        else if (!strcmp(argv[i], "--jumpvel")   && i + 1 < argc) ov_j  = atof(argv[++i]);
        else if (!strcmp(argv[i], "--jumpvel2")  && i + 1 < argc) ov_j2 = atof(argv[++i]);
        else if (!strcmp(argv[i], "--runspeed")  && i + 1 < argc) ov_r  = atof(argv[++i]);
        else if (!strcmp(argv[i], "--airaccel")  && i + 1 < argc) ov_a  = atof(argv[++i]);
        else if (!strcmp(argv[i], "--bboxw")     && i + 1 < argc) ov_bw = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bboxh")     && i + 1 < argc) ov_bh = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--camdzx")    && i + 1 < argc) ov_cx = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--camdzy")    && i + 1 < argc) ov_cy = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cambias")   && i + 1 < argc) ov_cb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("fa_slice [--gdata DIR] [--world 1..4] [--tut|--end] "
                   "[--credits] [--grid] "
                   "[--tone|--silent|--mute] [--vol N] [--frames N]\n"
                    "  no --world: the menu - click a world circle, or use arrows / D-pad\n"
                    "           and Enter / controller A to play it\n"
                   "  --credits: roll the credits screen (Credit1..4.bmp + "
                   "ENDTITLES), then the menu\n"
                   "  a world with tut.ini byte 0 loads its tutorial (WeltNt) "
                   "automatically; --tut forces it\n"
                   "  in a level: P toggles free-move (no-clip fly; hold A to "
                   "dash); I skips to this world's boss arena\n"
                   "  audio: with GData the real mixer plays music + voice "
                   "(--mute off, --vol 0..255); no GData -> a 440 Hz tone "
                   "(--tone keeps it, --silent off)\n"
                   "  --seed N: pin the enemy RNG (default: wall clock, "
                   "like the retail exe)\n"
                   "  --scale N: open the window at N x 800x600 (render stays "
                   "800x600, scaled up to fill); --fullscreen fills the "
                   "display; --crisp keeps whole-number scaling (sharp pixels, "
                   "black bars)\n"
                   "  physics tuning (px/tick): --gravity 0.6 --jumpvel 11 "
                   "--jumpvel2 9 --runspeed 5 --airaccel 1.2\n"
                   "    (--jumpvel = penguin, --jumpvel2 = Fettalatte; D switches)\n"
                   "  collision + camera (px): --bboxw 10 --bboxh 44 "
                   "--camdzx 64 --camdzy 48 --cambias -120 (negative = look up)\n");
            return 0;
        }
    }

    slice s;
    memset(&s, 0, sizeof s);
    s.grid = grid;
    s.tone = tone;
    s.want_chime = !tone && !silent;
    s.mute = mute;
    s.vol = vol;
    s.hover = -1;
    s.menu_focus = -1;
    s.ov_gravity = ov_g; s.ov_jumpvel = ov_j; s.ov_jumpvel2 = ov_j2;
    s.ov_runspeed = ov_r; s.ov_airaccel = ov_a;
    s.ov_bboxw = ov_bw; s.ov_bboxh = ov_bh;
    s.ov_camdzx = ov_cx; s.ov_camdzy = ov_cy; s.ov_cambias = ov_cb;
    s.rng_seed_set = seed_arg >= 0;
    s.rng_seed = s.rng_seed_set ? (uint32_t)seed_arg : (uint32_t)time(NULL);
    printf("enemy rng seed: %u%s\n", s.rng_seed,
           s.rng_seed_set ? " (--seed)" : " (clock)");
    fa_camera_init(&s.cam, FA_FB_W, FA_FB_H, FA_FB_W, FA_FB_H);

    find_gdata(gdata_arg, s.gdata, sizeof s.gdata);
    char *gdata = s.gdata;

    /* RRR-46: the mixer needs the DIRECT GDATA LOADER (RRR-33). */
    if (!mute && dir_has_gdata(gdata)) {
        s.audio = fa_audio_create(gdata);
        if (s.audio) {
            /* shipped Option.ini defaults: music 75, sound 100 (PL-119) */
            fa_audio_set_music_volume_ini(s.audio, 75);
            fa_audio_set_sfx_volume_ini(s.audio, 100);
            if (vol >= 0) fa_audio_set_master(s.audio, vol);
        }
    }

    if (show_credits && dir_has_gdata(gdata)) {
        s.credits = fa_credits_load(gdata);
        if (s.credits) {
            fa_credits_begin(s.credits);
            if (s.audio) fa_audio_event(s.audio, FA_SND_MENU_MUSIC);
            printf("credits: %d pages, then ENDTITLES; any key advances a "
                   "page, Esc quits\n", fa_credits_page_count(s.credits));
        } else {
            printf("credits: Credit1.bmp not found under %s\n", gdata);
        }
    } else if (world >= 1 && world <= 4) {
        const char *w1s = end ? "" : slice_world_suffix(gdata, world, tut);
        const char *w2s = end ? "E" : w1s;
        if (load_world(&s, gdata, world, w1s, w2s) == 0) {
            s.have_map = 1;
            s.world = world;
            s.in_end = end;
            if (s.audio)
                fa_audio_event(s.audio, end
                    ? FA_SND_MUSIC_BOSS
                    : (fa_snd_event)(FA_SND_MUSIC_W1 + world - 1));
            fa_camera_init(&s.cam, FA_FB_W, FA_FB_H, s.map.world_w, s.map.world_h);
            s.use_player = 1;
            wire_level(&s);
            load_kids(&s);
            s.hud = fa_hud_load(gdata);
            printf("hud: %s\n", s.hud ? "loaded" : "absent");
            printf("loaded Welt%d%s from %s: grid %dx%d, world %dx%d px, "
                   "bg %d frames\n", world, end ? "E" : w1s, gdata,
                   s.map.info.grid_w, s.map.info.grid_h,
                   s.map.world_w, s.map.world_h, fa_w01_count(&s.bg));
        } else {
            printf("could not load Welt%d from %s\n", world, gdata);
        }
    } else {
        slice_set_menu(&s, fa_menu_load(gdata));
        if (s.menu) {
            if (s.audio) fa_audio_event(s.audio, FA_SND_MENU_MUSIC);
            printf("loaded the title / world-select menu from %s (%d buttons)\n",
                   gdata, fa_menu_button_count(s.menu));
            for (int i = 0; i < fa_menu_button_count(s.menu); i++) {
                int x, y, w, h;
                fa_menu_hit_rect(s.menu, i, &x, &y, &w, &h);
                printf("  %-7s at %d,%d  %dx%d\n",
                       fa_menu_label(s.menu, i), x, y, w, h);
            }
        } else {
            printf("no GData at '%s' - showing the test pattern. Put GData "
                   "beside the .exe, or pass --gdata DIR.\n", gdata);
        }
    }

    if (s.menu)
        printf("click a world circle to play it (GIUNGLA = world 1), or use "
               "arrows / D-pad + Enter / controller A; ESCI to quit.\n");

    fa_app_cbs cbs;
    memset(&cbs, 0, sizeof cbs);
    cbs.sim = s_sim; cbs.render = s_render; cbs.audio = s_audio;
    cbs.should_quit = s_quit; cbs.user = &s;

    fa_platform_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.title = "Fresh Adventures";
    cfg.want_audio = 1;
    cfg.integer_scale = crisp ? 1 : 0;   /* default: fill the window/screen */
    cfg.window_scale = win_scale;
    cfg.fullscreen = fullscreen;

    fa_app_stats st;
    int rc = fa_app_run(&cfg, &cbs, frames, &st);
    printf("backend=%s frames=%llu ticks=%llu clamped=%llu hash=0x%08x rc=%d\n",
           st.backend, (unsigned long long)st.frames,
           (unsigned long long)st.ticks, (unsigned long long)st.clamped_frames,
           st.final_frame_hash, rc);
    if (strcmp(st.backend, "null") == 0 && frames <= 0)
        printf("NOTE: ran on the headless backend (no SDL2 / no display).\n");

    if (s.audio) fa_audio_destroy(s.audio);
    if (s.credits) fa_credits_free(s.credits);
    if (s.scores) fa_hiscore_free(s.scores);
    if (s.menu) fa_menu_free(s.menu);
    free_kids(&s);
    fa_hud_free(s.hud);
    fa_beh_free(s.beh);
    fa_tileset_free(s.tiles);
    fa_entity_free(s.ents);
    if (s.have_map) { fa_map_free(&s.map); fa_w01_close(&s.bg); }
    return rc;
}
