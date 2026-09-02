/*
 * fa_hiscore.h - the CLASSIFICA / high-score screen (RRR-47 + the offline
 * scoreboard pass, 2026-09-02)
 *
 * Reverse-engineered from JR_FERRERO.exe scene 15: tick handler 0x402DB0,
 * teardown 0x402CE0, the Schrift glyph map 0x401190 / the text draw 0x4012F0,
 * the table draw 0x40388D..0x403936, the per-world insert+write 0x4091B0 /
 * 0x409EE0, and the name-entry field 0x4093A0.  See the memory note
 * "scoreboard-classifica-oracle" for the full trace.
 *
 * Screen: blit GData\Pics\HighscoreBG.bmp, then
 *   - four world TAB buttons on the left (Giungla / Montagna / Fabbrica1 /
 *     valle .w01, at their table-A origins) - hover / click switches which
 *     world's table is shown, with alsf08.wav on the hover edge; the shown
 *     world's tab is drawn lit (frame 4), the rest dim (frame 2)
 *   - the beaten world's boss portrait (Gegner.w01 frame = world) on the
 *     right, only after a run (owner-signed-off 2026-09-02)
 *   - a BACK button (indietro.w01) bottom-right
 *   - ten name / score rows for the shown world, Y = 128 + 32*row:
 *       rank  X = 180   "1." .. "10."
 *       name  X = 230
 *       score right edge X = 490, 13 px / glyph estimate
 *
 * When a run's score places in the shown world's table it is inserted and the
 * new row becomes an editable name field (type A-Z / 0-9 / space / '-', max
 * 28 chars, Backspace deletes, Enter confirms).  On confirm the table is
 * written to  <install>/Highscore{world}.dat  (beside the game, RRR-39; the
 * Save\ prefix the .exe used is dropped).  The reader prefers that file over
 * the shipped GData\Save\ copy.
 *
 * Esc, Enter (outside name entry) or the BACK button leave to the menu.
 */
#ifndef FA_HISCORE_H
#define FA_HISCORE_H

#ifdef __cplusplus
extern "C" {
#endif

struct fa_surface;

typedef struct fa_hiscore fa_hiscore;

/* Load the background, the Schrift font, the four world tab sprites, the back
 * button, the boss portraits and all four Highscore tables from `gdata_dir`.
 * A missing / unreadable table falls back to the built-in default. Returns
 * NULL only if the background or the font is missing. */
fa_hiscore *fa_hiscore_load(const char *gdata_dir);
void        fa_hiscore_free(fa_hiscore *h);

/* Which world's table to show, 0..3 (world 1..4). Out-of-range is ignored. */
void fa_hiscore_set_world(fa_hiscore *h, int world0);
int  fa_hiscore_world(const fa_hiscore *h);

/* Show the beaten world's boss portrait on the right (Gegner.w01, frame =
 * world0). world0 0..3 picks the frame; -1 draws no portrait - the screen
 * opened cold from the menu's CLASSIFICA button. */
void fa_hiscore_set_boss_pic(fa_hiscore *h, int world0);

/*
 * Enter the screen. `world0` 0..3 is the world to show. If `run_score` > 0 it
 * is offered to that world's table: when it places, the new row becomes an
 * editable name field. `show_boss` non-zero draws world0's boss portrait
 * (pass it for any run, 0 from the menu). Equivalent to set_world +
 * set_boss_pic + the insert.
 */
void fa_hiscore_begin(fa_hiscore *h, int world0, long run_score, int show_boss);

/* One frame of input for the screen. */
typedef struct fa_hiscore_in {
    int  ptr_x, ptr_y;
    int  click;          /* left mouse-button down edge this frame */
    const char *text;    /* printable chars typed this frame (may be NULL)  */
    int  text_n;
    int  backspace;      /* Backspace down edge */
    int  enter;          /* Enter / Return down edge */
    int  escape;         /* Esc down edge */
} fa_hiscore_in;

/* What the frame produced, for the caller to act on. */
typedef struct fa_hiscore_ev {
    int leave;           /* 1 = the player left -> show the menu */
    int hover_sound;     /* 1 = a tab / button hover started -> play alsf08 */
    int wrote;           /* 1 = a name was confirmed; persist the table */
    int wrote_world;     /* 0..3: which world's table was written */
} fa_hiscore_ev;

/* Advance the screen one frame. `out` may be NULL. */
void fa_hiscore_tick(fa_hiscore *h, const fa_hiscore_in *in, fa_hiscore_ev *out);

/* Serialise world `world0`'s current table exactly as Highscore{n}.dat (10
 * "name CRLF score" records, no trailing newline). Returns the byte count, or
 * -1. The caller writes it beside the game. */
int fa_hiscore_world_bytes(const fa_hiscore *h, int world0,
                           unsigned char *buf, unsigned long cap);

/* Draw the whole screen into `dst` (expected 800x600 RGB565). 0/-1. */
int  fa_hiscore_render(const fa_hiscore *h, const struct fa_surface *dst);

/* Map a character to a Schrift.w01 frame index, or -1 for a space / unknown.
 * Table 0x4551B4: A-Z / a-z -> 0..25, 0-9 -> 26..35, then the punctuation
 * "! $ % ( ) , . - ; _ + = \" : ? > < / *" -> 36..54. Exposed for the test. */
int  fa_hiscore_glyph_frame(char c);

#ifdef __cplusplus
}
#endif

#endif /* FA_HISCORE_H */
