/*
 * fa_aom.h - animated-object (AOM) contract + runtime
 *
 * The original parses every `SCRIPT_TYP = "AOM"` file in `GData\Scripts\`
 * through `C_ScriptAom::DoScript` (`fcn.00436830`). Each line is one
 * `Key = value` pair looked up in a 46-entry token table (jump table at
 * `0x437108`); every value is a 16-bit integer except `FileName`, which is a
 * string. The result is a fixed struct that the level maps spawn objects
 * from, keyed by `ObjNr`.
 *
 *   Required          ObjNr, FileName. Missing or invalid -> the original
 *                     raises a severity-9 diagnostic and drops the script;
 *                     fa_aom_parse returns -1.
 *   Defaults          CorrectAlignToNull and UseVideoMem default to 1 when
 *                     the key is absent; every other key defaults to 0.
 *   Animation ranges  inclusive [start, end] frame ranges into the one .W01
 *                     sheet named by FileName. 8 movement directions, 8 idle
 *                     directions, and Attack / Freeze / KO. The 8-direction
 *                     and idle machinery exists in the parser but is dormant
 *                     in shipped content - only Left/Right/Up/Do and
 *                     Attack/Freeze/KO ranges are ever set. The runtime still
 *                     implements the whole machine.
 *   DetailGroup       0..4 selects a behaviour class from DetailGroup.jrs:
 *                     0 LIFT/PLATFORM, 1 BONUS, 2 POWERUP, 3 ENEMY, 4 DETAIL.
 *                     Collision resolves against group 0; group 3 drives
 *                     Attack/Freeze/KO. The runtime exposes a hook table per
 *                     class so behaviour binds without touching this module.
 *
 * Playback timing: the original advances exactly one step per refresh-locked
 * frame at a 60 Hz reference. Neither the AOM script nor the WESTKA
 * `Animation *.txt` sidecar carries a per-frame duration, so the runtime
 * advances one sheet frame per FA_AOM_TICKS_PER_FRAME ticks (default 1). A
 * per-object override is available via fa_aom_set_frame_period.
 */
#ifndef FA_AOM_H
#define FA_AOM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One sheet frame per this many 60 Hz simulation ticks (frame-locked 1:1). */
#define FA_AOM_TICKS_PER_FRAME  1u

/* Longest FileName value the token table stores (generous; shipped values
 * are well under 64 bytes). */
#define FA_AOM_FILENAME_MAX     260

/* 8 movement directions, in the order the DoScript token table lists them. */
typedef enum {
    FA_DIR_LEFT = 0, FA_DIR_RIGHT, FA_DIR_UP, FA_DIR_DOWN,
    FA_DIR_LEFT_UP, FA_DIR_LEFT_DOWN, FA_DIR_RIGHT_UP, FA_DIR_RIGHT_DOWN,
    FA_DIR_COUNT
} fa_dir;

/* The three special states (Attack / Freeze / KO), plus "none". */
typedef enum {
    FA_AOM_SP_NONE = 0, FA_AOM_SP_ATTACK, FA_AOM_SP_FREEZE, FA_AOM_SP_KO
} fa_aom_special;

/* DetailGroup.jrs behaviour classes. */
typedef enum {
    FA_AOM_DG_LIFT = 0,   /* LIFT/PLATTFORM - moving/solid platforms          */
    FA_AOM_DG_BONUS,      /* BONUS   - collectibles / score pickups           */
    FA_AOM_DG_POWERUP,    /* POWERUP                                          */
    FA_AOM_DG_ENEMY,      /* ENEMY   - drives Attack/Freeze/KO                */
    FA_AOM_DG_DETAIL,     /* DETAIL  - non-interacting scenery                */
    FA_AOM_DG_COUNT
} fa_aom_detail_group;

const char *fa_dir_name(fa_dir d);
const char *fa_aom_detail_group_name(fa_aom_detail_group g);

/* --- the parsed contract ------------------------------------------- */

typedef struct fa_aom_range {
    int start;      /* first sheet frame, inclusive */
    int end;        /* last  sheet frame, inclusive */
    int set;        /* 1 if the script named this range (either key) */
} fa_aom_range;

typedef struct fa_aom_def {
    int  obj_nr;                              /* ObjNr (required)            */
    char file_name[FA_AOM_FILENAME_MAX];      /* FileName, raw value token   */

    int  file_anim_start;                     /* FileAnimStart               */
    int  file_anim_end;                       /* FileAnimEnd                 */
    int  correct_align_to_null;               /* default 1                   */
    int  reference_spr_width;                 /* ReferenceSprWidth           */
    int  use_video_mem;                       /* default 1                   */
    int  detail_group_raw;                    /* DetailGroup as written      */

    fa_aom_range move[FA_DIR_COUNT];          /* StartAnim<Dir>/EndAnim<Dir> */
    fa_aom_range idle[FA_DIR_COUNT];          /* StartAnimIdle<Dir>/End...   */
    fa_aom_range attack;                      /* StartAnimAttack/EndAnimAttack */
    fa_aom_range freeze;
    fa_aom_range ko;
} fa_aom_def;

/*
 * Parse one AOM script. `src` need not be NUL-terminated; `len` is its byte
 * length. Fills `def` (zeroed first, then defaults applied). Diagnostics -
 * both the hard error and any non-fatal warnings (unknown-but-ignored lines,
 * DetailGroup outside 0..4) - are written to `diag` (may be NULL). A line
 * that starts "warning:" did not fail the parse.
 *
 * Returns 0 on success, -1 on a hard error: not an AOM script, a missing or
 * invalid ObjNr / FileName, an unknown key, a non-integer value, or a range
 * whose end precedes its start. This mirrors the original's severity-9 abort.
 */
int fa_aom_parse(fa_aom_def *def, const char *src, size_t len,
                 char *diag, size_t diag_cap);

/* Read a file and parse it. -1 (with a message in `diag`) on an I/O error. */
int fa_aom_parse_file(fa_aom_def *def, const char *path,
                      char *diag, size_t diag_cap);

/* DetailGroup as an enum. A raw value outside 0..4 maps to FA_AOM_DG_DETAIL
 * (and fa_aom_parse already emitted a warning). */
fa_aom_detail_group fa_aom_def_group(const fa_aom_def *def);

/* Basename of FileName with any drive/dir prefix and doubled backslashes
 * removed (e.g. "GData\\Animation\\Adler.w01" -> "Adler.w01"). Writes at most
 * `cap` bytes incl. the NUL; returns the length written, or -1 on bad args. */
int fa_aom_def_basename(const fa_aom_def *def, char *buf, size_t cap);

/* --- behaviour hooks (DetailGroup coupling) ----------------------- */

typedef struct fa_aom_obj fa_aom_obj;

typedef struct fa_aom_hooks {
    /* Fired by fa_aom_obj_spawn, once. */
    void (*on_spawn)(fa_aom_obj *o, void *ctx);
    /* Fired by fa_aom_tick, after the animation frame advances. */
    void (*on_tick)(fa_aom_obj *o, void *ctx);
    /* Fired by fa_aom_obj_player_touch (called by the collision/pickup code). */
    void (*on_player_touch)(fa_aom_obj *o, void *ctx);
} fa_aom_hooks;

typedef struct fa_aom_registry {
    fa_aom_hooks hooks[FA_AOM_DG_COUNT];
    void        *ctx[FA_AOM_DG_COUNT];
} fa_aom_registry;

void fa_aom_registry_init(fa_aom_registry *r);

/* Bind a hook set + context to one behaviour class. `h` is copied; pass NULL
 * to clear. Returns 0, or -1 on a bad group. */
int  fa_aom_registry_set(fa_aom_registry *r, fa_aom_detail_group g,
                         const fa_aom_hooks *h, void *ctx);

/* --- the runtime object ------------------------------------------- */

/* Which range is currently playing, for introspection and tests. */
typedef enum {
    FA_AOM_ACT_NONE = 0,
    FA_AOM_ACT_MOVE,          /* move[dir]                                  */
    FA_AOM_ACT_MOVE_FALLBACK, /* diagonal requested, a cardinal used        */
    FA_AOM_ACT_IDLE,          /* idle[dir]                                  */
    FA_AOM_ACT_IDLE_STAND,    /* no idle range: hold move[dir].start        */
    FA_AOM_ACT_FILE,          /* no move/idle range: FileAnimStart..End     */
    FA_AOM_ACT_ATTACK,
    FA_AOM_ACT_FREEZE,
    FA_AOM_ACT_KO
} fa_aom_act;

struct fa_aom_obj {
    const fa_aom_def *def;
    fa_aom_registry  *reg;     /* may be NULL */
    void             *user;    /* gameplay-layer pointer, opaque here */

    /* logical inputs, set by the gameplay layer */
    fa_dir         facing;
    int            moving;
    fa_aom_special special;
    unsigned       frame_period;   /* ticks per sheet frame; default above */

    /* resolved animation state */
    fa_aom_act   act;
    fa_aom_range active;
    int          frame;       /* current sheet frame, in [active.start,end] */
    unsigned     sub;         /* ticks accumulated toward the next frame */
    uint64_t     cycles;      /* completed loops of the active range */
};

/* Initialise an object from a parsed def. `reg` and `user` may be NULL.
 * Starts facing FA_DIR_LEFT, not moving, no special. Returns 0 / -1. */
int  fa_aom_obj_init(fa_aom_obj *o, const fa_aom_def *def,
                     fa_aom_registry *reg, void *user);

/* Fire the DetailGroup on_spawn hook (if any). Call once after init. */
void fa_aom_obj_spawn(fa_aom_obj *o);

/* Fire the DetailGroup on_player_touch hook (if any). */
void fa_aom_obj_player_touch(fa_aom_obj *o);

void fa_aom_set_facing(fa_aom_obj *o, fa_dir d);
void fa_aom_set_moving(fa_aom_obj *o, int moving);
void fa_aom_set_special(fa_aom_obj *o, fa_aom_special sp);
/* Override ticks-per-frame for this object; 0 resets to FA_AOM_TICKS_PER_FRAME. */
void fa_aom_set_frame_period(fa_aom_obj *o, unsigned ticks);

/* Advance one 60 Hz simulation tick: re-resolve the active range (resetting
 * to its first frame on a change), step the frame if the period elapsed,
 * loop within the range, then fire the DetailGroup on_tick hook. */
void fa_aom_tick(fa_aom_obj *o);

int                 fa_aom_frame(const fa_aom_obj *o);
uint64_t            fa_aom_cycles(const fa_aom_obj *o);
fa_aom_act          fa_aom_active(const fa_aom_obj *o);
const char         *fa_aom_active_name(const fa_aom_obj *o);
fa_aom_detail_group fa_aom_group(const fa_aom_obj *o);

#ifdef __cplusplus
}
#endif

#endif /* FA_AOM_H */
