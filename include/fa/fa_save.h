/*
 * fa_save.h - Option.ini and Highscore1-4.dat text formats (RRR-47)
 *
 * Ported verbatim from the RRR-23 reference (save_format.h/.c), which round-
 * trips all 5 shipped files byte-identical. Lifted from JR_FERRERO.exe
 * (base 0x400000):
 *   Option.ini  write   fcn.00409a60  (C_Option::Save)
 *   Option.ini  default fcn.004096e0  (written when the file is missing)
 *   Option.ini  load    fcn.00409bf0 -> fcn.00409870
 *   Highscore   write   fcn.00409cd0 / fcn.00409f37 (per-world rewrite)
 *   Highscore   read    fcn.00409e00
 *
 * Both files are plain CRLF text; the writer joins records with "\r\n" and
 * emits NO trailing newline, so a shipped file has exactly
 * (line_count - 1) CRLF pairs.
 *
 * Volume-line labels corrected per RRR-46 (PL-119): line 21 (index 20,
 * global 0x45ECC0) is MUSIC volume, line 22 (index 21, 0x45EFE4) is SOUND
 * volume - both 0..100. RRR-23 had the two swapped. The shipped values are
 * 75 (line 21) and 100 (line 22): music 75, sound 100.
 */
#ifndef FA_SAVE_H
#define FA_SAVE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Option.ini ---------------------------------------------------- */

#define FA_OPT_LINES 22

/*
 *   [ 0.. 3]  movement key binds : Left, Right, Up, Down   (DIK scancodes)
 *   [ 4.. 7]  action  key binds  : Jump, Fire, Action, spare (DIK)
 *   [ 8..15]  8 controller-action validation slots (primary/alt x4)
 *   [16..19]  four 16-bit aux slots
 *   [20]      MUSIC volume  0..100   (0x45ECC0, shipped 75)
 *   [21]      SOUND volume  0..100   (0x45EFE4, shipped 100)
 */
typedef struct { int line[FA_OPT_LINES]; } fa_opt_file;

/* Parse 22 decimal integers, one per line. 0 on success, -1 on wrong count. */
int  fa_opt_parse(const unsigned char *buf, size_t len, fa_opt_file *out);

/* Serialise exactly as fcn.00409a60. Returns bytes written, or -1. */
int  fa_opt_write(const fa_opt_file *in, unsigned char *out, size_t cap);

/* The hard-coded default line set (fcn.004096e0), game-pad absent. */
void fa_opt_default(fa_opt_file *out);

/* ---- Highscore1-4.dat -------------------------------------------- */

#define FA_HS_ENTRIES  10
#define FA_HS_NAME_MAX 28          /* read buffer 0x1d incl NUL */

typedef struct {
    char          name[FA_HS_NAME_MAX + 1];
    unsigned long score;
} fa_hs_entry;

typedef struct { fa_hs_entry entry[FA_HS_ENTRIES]; } fa_hs_file;

/* Read 10 (name-line, score-line) pairs. 0 on success, -1 if incomplete. */
int  fa_hs_parse(const unsigned char *buf, size_t len, fa_hs_file *out);

/* Serialise as fcn.00409cd0. Returns bytes written, or -1. */
int  fa_hs_write(const fa_hs_file *in, unsigned char *out, size_t cap);

/* Built-in default table: "Player 1".."Player 10", 10000..1000 step 1000. */
void fa_hs_default(fa_hs_file *out);

/* Insert `score` under `name` in descending order (fcn.004091b0). Returns the
 * 0-based rank it landed at, or -1 if it does not beat the 10th entry. */
int  fa_hs_insert(fa_hs_file *hs, const char *name, unsigned long score);

#ifdef __cplusplus
}
#endif

#endif /* FA_SAVE_H */
