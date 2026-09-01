/*
 * fa_hiscore.h - the high-score screen (RRR-47, PL-125/126)
 *
 * Reverse-engineered from JR_FERRERO.exe: handler 0x402DB0, the Schrift.w01
 * glyph mapper 0x401190 / 0x4012F0, and the table draw 0x40387D..0x403936.
 *
 * Screen: blit GData\Pics\HighscoreBG.bmp, then draw the SELECTED world's ten
 * name/score rows with GData\Animation\Interface\Schrift.w01 glyphs. One of
 * four world tables is shown at a time (four sprite hit branches switch it;
 * default is the last-played world in 0x4DABD4, else the first non-zero world
 * flag). Ten rows, Y = 128 + 32*row (128..416):
 *
 *     rank   X = 180   "1."   .. "10."
 *     name   X = 230   in-memory 30-byte slot
 *     score  right edge X = 490; drawn from X = 490 - 13*strlen(score)
 *
 * The score files are alternating name/score text lines (RRR-23 / fa_save,
 * PL-125) - the "160-byte record" in RRR-23 was a coincidence of the default
 * data.
 *
 * KNOWN GAP: the exact Schrift.w01 character -> frame table (0x401190) was not
 * extracted (the follow-up disasm was rate-limited). fa_hiscore uses a
 * documented best-effort mapping in fa_hiscore.c; glyph identity may be off
 * until the table lands. The row layout and the file parsing are exact.
 */
#ifndef FA_HISCORE_H
#define FA_HISCORE_H

#ifdef __cplusplus
extern "C" {
#endif

struct fa_surface;

typedef struct fa_hiscore fa_hiscore;

/* Load the background, the Schrift font and all four Highscore{1-4}.dat
 * tables from `gdata_dir`. A missing / unreadable .dat falls back to the
 * built-in default table. Returns NULL only if the background or the font is
 * missing. */
fa_hiscore *fa_hiscore_load(const char *gdata_dir);
void        fa_hiscore_free(fa_hiscore *h);

/* Which world's table to show, 0..3 (world 1..4). Out-of-range is ignored. */
void fa_hiscore_set_world(fa_hiscore *h, int world0);
int  fa_hiscore_world(const fa_hiscore *h);

/* Draw the whole screen into `dst` (expected 800x600 RGB565). 0/-1. */
int  fa_hiscore_render(const fa_hiscore *h, const struct fa_surface *dst);

/* Map a character to a Schrift.w01 frame index, or -1 for a space / unknown
 * (advance the pen 5 px, per 0x4012F0). Exposed for the test. */
int  fa_hiscore_glyph_frame(char c);

#ifdef __cplusplus
}
#endif

#endif /* FA_HISCORE_H */
