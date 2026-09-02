/*
 * fa_beh.h - per-ObjNr enemy behaviour layer (RRR-51)
 *
 * The exe installs one behaviour callback per placed object at record +0x5E
 * (fcn.004311C0 walks the records; the switch at 0x4119F7..0x411F9C picks the
 * handler). DetailGroup-3 records are enemies. fa_entity.c (RRR-50) runs the
 * generic patrol / animation / lifetime only; this module binds the real
 * per-type behaviour through fa_entity_set_behaviour().
 *
 * Reverse-engineered from JR_FERRERO.exe by hand off jr_disasm.txt (the
 * per-ObjNr handlers 0x415FF0 papagei / 0x413CB0 kong / 0x40A700 adler / ...,
 * the shared updater 0x4335A0, the animation-range selector 0x430B20, the
 * player-contact helper 0x41A3E0, the projectile test 0x41A5E0). See
 * RRR-51/enemy-behaviour-disasm.md (Codex, corrected by the by-hand pass) and
 * RRR-51/RRR-51-report.md.
 *
 * FACTS THE IMPLEMENTATION FOLLOWS (each verified in the raw disassembly):
 *   - Movement is float pos +0x94/+0x98 and float vel +0x9c/+0xa0. Ground
 *     enemies run vx = 3.0, flyers 2.0. They reverse ONLY at the record's
 *     min/max X (rec[0x1F]/rec[0x23]); no wall or ledge probe.
 *   - Kong / small yeti / snowman / egg robot / bear alternate a ROAM phase
 *     (logic state 1, timer 120..239) with a READY/IDLE phase (state 2, timer
 *     60..119); the throw fires from state 2 when the walk animation wraps.
 *   - Parrot / eagle / bee dive: on the gate vy = +12.0, each tick vy -= 0.3
 *     down to -12.0, X keeps drifting; the dive ends when Y climbs back to the
 *     record's min Y (rec[0x21]) and sets cooldown 120.
 *   - No stomp. 0x41A3E0 never reads player vy. Contact is a flat 20 damage
 *     against the box [px-40, px+40] x [py-190, py] (py-100 while crouching),
 *     then 120 ticks of player i-frames (0x41A538).
 *   - AOM def+0x39 is a flat {u16 start, u16 end} table indexed by state*4:
 *     state 16 Attack, 17 Freeze, 18 KO. The defeat animation is the FREEZE
 *     range (state 17), played once while the body is launched (vx +/-1.0,
 *     vy -12.0, gravity +0.6, terminal +20.0), then the record is gone.
 *   - An ordinary enemy dies in one accepted projectile hit: +100 at score
 *     0x45ED2C, the launched Freeze, no drop, no in-level respawn. The snowman
 *     is the exception - it freezes ~120 ticks and thaws.
 *   - Bosses (9/10/14/18): 10 accepted hits, +10000, then KO. The gorilla (10)
 *     is hurt only by a reflected coconut; the yeti (9) and octopus (18) take a
 *     direct body snowball. The robot (14) is not implemented yet (RRR-61).
 */
#ifndef FA_BEH_H
#define FA_BEH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RRR-52: the default seed for the enemy RNG stream. The exe seeds rand()
 * from the wall clock (fcn @0x42AE27), so its stream differs every run; the
 * port fixes it for a deterministic replay (RRR-34). fa_beh_seed overrides. */
#define FA_BEH_RNG_DEFAULT_SEED 1u

struct fa_entity_store;
struct fa_snowball;

/* Sound events fa_beh asks the host to play (mapped to fa_audio in fa_slice). */
enum {
    FA_BEH_SFX_ENEMY_KO = 1,   /* an enemy took a fatal hit (0x422b60 id 3) */
    FA_BEH_SFX_ENEMY_LAUNCH,   /* the launched-death phase begins (id 4)    */
    FA_BEH_SFX_ENEMY_ATTACK,   /* an enemy started its attack / dive (id a) */
    FA_BEH_SFX_ENEMY_SHOT,     /* an enemy threw a projectile               */
    FA_BEH_SFX_FREEZE,         /* the snowman froze                         */
    FA_BEH_SFX_BOSS_HIT,       /* a boss took an accepted hit               */
    FA_BEH_SFX_BOSS_KO,        /* a boss was defeated                       */
    FA_BEH_SFX_BROESEL_BREAK,  /* a crumbling platform broke (0x422B60 id 6,
                                * knusper.wav, slot 6)                       */
    FA_BEH_SFX_BOSS_CHARGE,    /* RRR-61: the robot boss winds up to fire   */
    FA_BEH_SFX_BOSS_KO_ANIM    /* RRR-61: a beat inside the boss KO anim
                                * (robot: w3sf04b ch10 at RBKO frame 56)    */
};

typedef struct fa_beh_hooks {
    /* Terrain solidity at a world pixel: 0 none / 1 solid / 2 one-way.
     * Enemy projectiles die on a solid hit (0x434180 query type 2). */
    int  (*terrain)(int px, int py, void *user);
    /* Add to the score global (0x45ED2C += 100 / += 10000). */
    void (*score)(int add, void *user);
    /* Play a FA_BEH_SFX_* cue for the given enemy ObjNr (the throw / dive
     * sound differs per type - see fa_slice beh_sfx). */
    void (*sfx)(int ev, int obj_nr, void *user);
    /* Play a Kinder Paradiso voice line, streamed on lane 17 (0x415550).
     * `rel_wav` is a GData-relative path. In a NORMAL level it is the one
     * world line PA00NN.wav; in a TUTORIAL level (WeltNt) it is a PAT00NN.wav
     * checkpoint line picked by the placement's rec[+0x2A] (0x415B70).
     * Optional - NULL = silent. */
    void (*voice)(const char *rel_wav, void *user);
    /* Non-zero while the last `voice` line is still playing (0x4231E5) - the
     * Paradiso loops its mouth animation until this goes 0. Optional. */
    int  (*voice_busy)(void *user);
    /* The tutorial-end Kinder Paradiso (the placement with rec[+0x2A] == 7,
     * always near the level exit) finished its line (pat0020). The exe then
     * writes tut.ini[world] = 1 and requests scene 20 (0x4159BD/0x4159C5),
     * which reloads the same world as its normal level. The host marks the
     * tutorial cleared and does that reload. Optional. */
    void (*tutorial_done)(void *user);
    void  *user;
} fa_beh_hooks;

typedef struct fa_beh fa_beh;

/* Create the behaviour layer over an entity store and register every
 * per-ObjNr callback. NULL on OOM / a bad store. Free before fa_entity_free. */
fa_beh *fa_beh_create(struct fa_entity_store *store, const fa_beh_hooks *hooks);
void    fa_beh_free(fa_beh *b);

/* The world index (1..4), for the level-specific Kinder Paradiso cue
 * (ds:0x4DABD4 in the exe: world 1 PA0011, 2 PA0014, 3 PA0013, 4 PA0012). */
void    fa_beh_set_world(fa_beh *b, int world);

/* The active character (0 penguin / 1 Milchschnitte, ds:0x4E1020). Two
 * tutorial Kinder Paradiso checkpoints (rec[+0x2A] 6 and 8) speak a
 * per-character line. Call each tick before fa_entity_tick; default 0. */
void    fa_beh_set_character(fa_beh *b, int character);

/* RRR-60: the current snowball type - 1 = "dirty" (black) balls from
 * collect_dirtyballs (ObjNr 60, ds:0x4E1044), 0 = normal. Only dirty balls
 * hurt the World-2 yeti boss (exe 0x41A5E0 returns 2 for a non-0x105 ball,
 * which the yeti's state-1/3 check accepts as a hit). Call each tick before
 * fa_entity_tick; default 0. */
void    fa_beh_set_ammo_dirty(fa_beh *b, int dirty);

/* RRR-52: reseed the enemy RNG stream (the exe's single rand() stream, the
 * source of every random roam / ready / burst-length timer). Call once per
 * level load, after fa_beh_create, for a reproducible run. */
void    fa_beh_seed(fa_beh *b, uint32_t seed);

/*
 * Publish this tick's player state, BEFORE fa_entity_tick. `feet_x/feet_y` is
 * the player origin (feet). `half_w`/`height` give the contact box
 * ([feet_x +/- half_w] x [feet_y - height, feet_y]) - pass 40 and 190
 * (100 while crouching) to match 0x41A3E0. `facing` is +1 right / -1 left.
 * `iframes` > 0 means the player is invulnerable this tick. `snow` is the
 * player's live snowball pool (mutable - a hit consumes one), `snow_max` its
 * length.
 */
void fa_beh_begin_frame(fa_beh *b, int feet_x, int feet_y,
                        int half_w, int height, int facing, int iframes,
                        struct fa_snowball *snow, int snow_max);

/*
 * Run AFTER fa_entity_tick: advance enemy-owned projectiles (integrate,
 * terrain-kill, player damage). Returns the damage the player should take
 * this tick (0 or 20); *knockback is set to 1 when a contact happened.
 */
int fa_beh_post(fa_beh *b, int *knockback);

/*
 * The Fettalatte shove (PL-135). On the push animation's frame-176 event the
 * host calls this with the kind-5 probe point (body_x +/- 32, body_y - 100)
 * and the facing (+1 right / -1 left). If an active pushable block (ObjNr
 * 76/78/86/87) contains that point, its float vx is set to +/-7.0 for one
 * impulse; the block's own behaviour then slides + friction-decays it. The
 * player is NOT carried. Returns 1 if a block took the impulse.
 */
int fa_beh_push(fa_beh *b, int probe_x, int probe_y, int facing);

/* The X pixels a pushable block at the probe point moved this tick (call
 * after fa_beh_post). The host adds it to the player so he stays in contact
 * while shoving - the exe vx-locks the player, but its wider body keeps
 * touching; this narrow-box port needs the carry. 0 if no block there. */
int fa_beh_push_carry(fa_beh *b, int probe_x, int probe_y);

/* Introspection for tests / the HUD. */
int fa_beh_enemies_alive(const fa_beh *b);
int fa_beh_projectiles_live(const fa_beh *b);

/* Enemy projectile slot `i` (0..FA_BEH_PROJ_MAX-1): 1 and fills the world
 * pixel centre if it is live, 0 otherwise. For the host renderer. */
#define FA_BEH_PROJ_MAX 24
/* Also fills *owner_obj with the throwing enemy's ObjNr - render the
 * projectile from fa_entity_obj_sheet(that) at its per-enemy frame:
 * kong/ape 5->16 (banana), yeti 7->25, snowman 8->54, egg 12->23,
 * bear 15->53, gorilla boss 10->87 (coconut). */
int fa_beh_projectile(const fa_beh *b, int i, int *wx, int *wy, int *owner_obj);
int fa_beh_boss_hp(const fa_beh *b);          /* -1 if no boss in the level */

/*
 * RRR-59: the World-1 gorilla boss (ObjNr 10, Welt1E). The boss is stationary
 * and lobs coconuts (0x40C4E0 states 1/2/10, callback 0x40C2C0). A player
 * snowball that strikes an incoming coconut sends it back (vx -> +/-9.0,
 * vy -> -9.0); a returned coconut on the boss body is one accepted hit. Ten
 * hits -> +10000 -> KO. `fa_beh_boss_defeated` is 1 once that KO has fired.
 *
 * RRR-60: the World-2 yeti boss (ObjNr 9, Welt2E, 0x40E350). Stationary; holds
 * a frozen pose and periodically KICKS or HOPS or talks. Only a DIRTY ("black")
 * snowball (collect_dirtyballs, ObjNr 60) hurts it, and only while it is idle
 * or kicking. Same 10 hits -> +10000 -> KO and the same 7th-piece (ObjNr 59)
 * -> CLASSIFICA chain as the gorilla. Call fa_beh_set_ammo_dirty each tick.
 *   - KICK sends a floor ice block (ObjNr 265) sliding left at the kid.
 *   - HOP (and every hit-recoil jump) drops a pattern of ceiling icicles
 *     (ObjNr 79).
 *   - the ice platform under it (ObjNr 80) stops animating on the KO.
 * fa_beh binds behaviours to ObjNr 265 / 79 / 80 automatically. The robot (14)
 * boss is still a statue (RRR-61); the octopus (18) takes any direct snowball.
 */
int fa_beh_boss_defeated(const fa_beh *b);

/* RRR-59: 1 once the player has caught the 7th recipe piece (ObjNr 59) that
 * drops when the World-1 boss is defeated. The host then ends the level
 * (owner: -> the CLASSIFICA / high-score screen). */
int fa_beh_recipe_done(const fa_beh *b);

#ifdef __cplusplus
}
#endif

#endif /* FA_BEH_H */
