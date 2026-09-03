/*
 * fa_charspr.h - character sprite sheet + animation playback
 *
 * Draws the playable kids (PINGUIN.W01, MILCHSCHNITTE.W01) and, later, any
 * hand-animated actor that is NOT an AOM script object. AOM objects go
 * through fa_aom.h instead.
 *
 * PARITY BASIS (JR_FERRERO.exe)
 *   - There is NO clip-id table. The player state machine (dispatch table at
 *     0x41A128) writes literal W01 frame numbers - {current, loop_start,
 *     inclusive_end, repeat} - into a 0x64-byte animation record per
 *     character (Penguin 0x4E0F30, Milch 0x4E0FA8). fa_charspr reproduces
 *     that as a per-pose clip the caller sets with the disassembled constants
 *     (fa_cs_anim_bind_frames). repeat: loop (-1 in the exe) vs hold the last
 *     frame (0 in the exe).
 *   - Advance rate: the record's delay is 1, and the updater counts it down
 *     before each step, so a frame advances every SECOND 60 Hz tick = 30
 *     sheet fps. Set period 2.
 *   - Facing (global 0x4E0F94): the raw art is LEFT-facing. Left uses it
 *     as-is (orientation mode 0); right is the engine's horizontal-flip blit
 *     (mode 2), same frame range. Mirrored placement reflects about the
 *     anchor: x = anchor_x - origin_x - width, y unchanged. So
 *     base_facing = -1.
 *   - Origin: the signed .W01 table-A value, added to the anchor.
 *
 * The WESTKA "Animation *.txt" sidecar is kept only as a human-readable clip
 * name source (fa_cs_parse_sidecar) - it is NOT the runtime selection data
 * and several of its ranges disagree with the engine.
 */
#ifndef FA_CHARSPR_H
#define FA_CHARSPR_H

#include <stddef.h>
#include <stdint.h>

#include "fa/fa_w01.h"

#ifdef __cplusplus
extern "C" {
#endif

struct fa_surface;
struct fa_rect;

#define FA_CS_MAX_CLIPS   64
#define FA_CS_CODE_MAX    16

/* One animation clip: an inclusive sheet-frame range plus a loop sub-range
 * (frames [first, loop_first-1] are the lead-in, played once). `loop` = 1 to
 * repeat [loop_first, last] forever, 0 to advance to `last` and hold. */
typedef struct fa_cs_clip {
    char code[FA_CS_CODE_MAX];   /* "" for a frame-built clip */
    int  first, last;
    int  loop_first, loop_last;
    int  loop;                   /* 1 = repeat, 0 = hold last */
} fa_cs_clip;

typedef struct fa_cs_sheet {
    fa_w01     w01;
    int        owns_w01;
    fa_cs_clip clips[FA_CS_MAX_CLIPS];   /* parsed sidecar clips (optional) */
    int        clip_count;
    int        base_facing;   /* +1 art faces right, -1 art faces left */
} fa_cs_sheet;

/* High-level pose the gameplay layer asks for. */
typedef enum {
    FA_CS_STAND = 0,
    FA_CS_WALK,
    FA_CS_CROUCH,       /* down held: play in, hold the low frame */
    FA_CS_CROUCH_RISE,  /* down released: play the stand-up frames once */
    FA_CS_JUMP_RISE,
    FA_CS_JUMP_FALL,
    FA_CS_GLIDE,
    FA_CS_THROW_FWD,
    FA_CS_THROW_UP,
    FA_CS_KO,
    FA_CS_IDLE_A,       /* short idle fidget */
    FA_CS_IDLE_B,       /* long idle animation */
    FA_CS_CLIMB,        /* on a ladder; freeze the frame when not moving */
    FA_CS_PUSH,         /* Fettalatte shoving a heavy object */
    FA_CS_SWAP,         /* the turn-to-camera + wave while the kid swaps out */
    FA_CS_SWAP_END,     /* Fettalatte's turn-back, played at the end of the swap */
    FA_CS_POSE_COUNT
} fa_cs_pose;

const char *fa_cs_pose_name(fa_cs_pose p);

/* --- sheet ---------------------------------------------------------- */

/*
 * Parse a WESTKA "Animation *.txt" sidecar into named clips (loop = 1). Each
 * data line is  CODE <ws> A - B [ ... [Anim C - D] ... ]. Header / prose
 * lines are skipped. Fills `out` (up to `cap`); returns the count, or -1.
 */
int fa_cs_parse_sidecar(fa_cs_clip *out, int cap, const char *text, size_t len);

/*
 * Open a character sheet. `sidecar_path` may be NULL. `base_facing` is +1 or
 * -1. Case-tolerant on the basename. Returns 0, or -1.
 */
int  fa_cs_sheet_open(fa_cs_sheet *s, const char *w01_path,
                      const char *sidecar_path, int base_facing);

void fa_cs_sheet_close(fa_cs_sheet *s);

/* Add or replace a sidecar-style clip by code. Returns the index or -1. */
int  fa_cs_sheet_add_clip(fa_cs_sheet *s, const char *code,
                          int first, int last, int loop_first, int loop_last);

/* Clip index for `code`, or -1. */
int  fa_cs_sheet_find(const fa_cs_sheet *s, const char *code);

int  fa_cs_sheet_frame_count(const fa_cs_sheet *s);

/* --- animation instance ------------------------------------------- */

typedef struct fa_cs_anim {
    const fa_cs_sheet *sheet;
    fa_cs_clip pose_clip[FA_CS_POSE_COUNT];
    int        pose_set[FA_CS_POSE_COUNT];

    fa_cs_pose pose;
    fa_cs_clip active;      /* the clip currently playing */
    int        has_active;
    int        frame;       /* current sheet frame */
    unsigned   sub;         /* ticks accumulated toward the next frame */
    unsigned   period;      /* ticks per frame; 0 = hold the first frame */
    int        facing;      /* -1 / +1 */
    uint64_t   cycles;      /* completed loops of the active clip */

    int        cached_frame;
    int        cache_w, cache_h;
    uint16_t  *cache;
} fa_cs_anim;

/* Init with no poses bound. period defaults to 1; call fa_cs_anim_set_period
 * for the player (2 = the 30 fps engine rate). facing starts at base_facing. */
void fa_cs_anim_init(fa_cs_anim *a, const fa_cs_sheet *s);

void fa_cs_anim_free(fa_cs_anim *a);

/*
 * Bind a pose to an explicit frame range (the RE'd engine constants).
 *   current      first frame shown on entry
 *   loop_first   start of the repeating sub-range (== current for no lead-in)
 *   last         inclusive end
 *   loop         1 = repeat [loop_first, last], 0 = advance to last and hold
 * Returns 0, or -1 (bad pose / range).
 */
int  fa_cs_anim_bind_frames(fa_cs_anim *a, fa_cs_pose pose,
                            int current, int loop_first, int last, int loop);

/* Bind a pose to a parsed sidecar clip by code (loop = 1). Returns 0/-1. */
int  fa_cs_anim_bind(fa_cs_anim *a, fa_cs_pose pose, const char *code);

/* Convenience: bind every pose from a code array (NULL/unknown = skip).
 * Returns the number bound. */
int  fa_cs_anim_bind_all(fa_cs_anim *a, const char *codes[FA_CS_POSE_COUNT]);

/* Ticks per sheet frame. 0 freezes on the first frame. */
void fa_cs_anim_set_period(fa_cs_anim *a, unsigned ticks);

/*
 * Set the pose and facing for this tick. A pose change (or a facing change)
 * restarts the clip at its first frame. An unbound pose falls back, in
 * order, to STAND, WALK, JUMP_FALL, then the first bound pose.
 */
void fa_cs_anim_set(fa_cs_anim *a, fa_cs_pose pose, int facing);

/* Advance one simulation tick. */
void fa_cs_anim_tick(fa_cs_anim *a);

int  fa_cs_anim_frame(const fa_cs_anim *a);   /* current sheet frame, or -1 */

/*
 * Draw the current frame into `dst` with (anchor_x, anchor_y) at the sprite's
 * null point (the player's feet). Adds the signed table-A origin; when facing
 * != base_facing, horizontally mirrors and places at
 * anchor_x - origin_x - width. Keys out 0x0000. `clip` may be NULL.
 * Returns pixels written (>= 0) or -1.
 */
long fa_cs_anim_draw(fa_cs_anim *a, const struct fa_surface *dst,
                     int anchor_x, int anchor_y, const struct fa_rect *clip);

#ifdef __cplusplus
}
#endif

#endif /* FA_CHARSPR_H */
