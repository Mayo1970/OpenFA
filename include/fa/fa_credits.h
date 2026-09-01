/*
 * fa_credits.h - the credits sequence (RRR-54)
 *
 * Reverse-engineered from JR_FERRERO.exe credits handler fcn.0040FBC0
 * (0x40FBC0..0x40FD65), scenes 21/22/23 of the global scene selector
 * 0x45F008. See RRR-54/menu-disasm.md (PL-143/144).
 *
 * The screen:
 *   1. show GData\pics\Credit1.bmp .. Credit4.bmp full-screen, in order
 *      (string "GData\pics\credit%d.bmp" @ 0x4561F4). Scene 21 loads page 1
 *      and sets 0x45F008 = 0x16; scene 22 shows a page and, on a skip request
 *      folded into 0x4DAB44, advances the page index 0x4DAB50 and reloads
 *      credit(n+1).bmp while n < 4, else tears down.
 *   2. then play GData\Animation\ENDTITLES.W01 once (string @ 0x45620C),
 *      drawn by scene 23 through the resolution-specific renderers.
 *   3. then hand back to the world-select menu.
 *
 * There is NO compiled per-page timer in the exe - each BMP page holds until
 * the player advances it. This module models that with an auto-advance dwell
 * (FA_CREDITS_PAGE_DWELL) PLUS a skip flag, so the screen runs unattended in a
 * headless test and can still be paged by hand in fa_slice.
 *
 * Integer-only and deterministic (the RRR-34 replay contract).
 */
#ifndef FA_CREDITS_H
#define FA_CREDITS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fa_surface;

typedef struct fa_credits fa_credits;

typedef enum {
    FA_CREDITS_PAGE = 0,   /* showing Credit(page+1).bmp                     */
    FA_CREDITS_ENDTITLES,  /* playing ENDTITLES.W01 once                     */
    FA_CREDITS_DONE        /* finished - the host returns to the menu        */
} fa_credits_phase;

/* Ticks a BMP page holds before it auto-advances (not an exe constant - the
 * original waits for input; 6 s at 60 Hz here). A skip advances immediately. */
#define FA_CREDITS_PAGE_DWELL 360
/* Ticks per ENDTITLES.W01 frame (RRR-9 frame lock is 1/tick; the credits roll
 * is slower - 3 ticks/frame reads at a comfortable scroll). */
#define FA_CREDITS_ET_PERIOD  3

/* Load the sequence from `gdata_dir` (the GData folder). Needs at least
 * Credit1.bmp; ENDTITLES.W01 is optional (skipped if absent). Returns a heap
 * object or NULL. */
fa_credits *fa_credits_load(const char *gdata_dir);
void        fa_credits_free(fa_credits *c);

/* (Re)start at page 1. */
void fa_credits_begin(fa_credits *c);

/* Advance one 60 Hz tick. `skip` != 0 = the player asked for the next page
 * (or, in ENDTITLES, to end the roll). No-op once DONE. */
void fa_credits_tick(fa_credits *c, int skip);

/* --- queries -------------------------------------------------------------- */

fa_credits_phase fa_credits_phase_of(const fa_credits *c);
int  fa_credits_page(const fa_credits *c);        /* 0-based, PAGE phase only  */
int  fa_credits_page_count(const fa_credits *c);
int  fa_credits_done(const fa_credits *c);        /* phase == FA_CREDITS_DONE  */
uint32_t fa_credits_hash(const fa_credits *c);    /* rolling FNV-1a-32 (tests) */
const char *fa_credits_phase_name(fa_credits_phase p);

/* Draw the current frame into `dst` (800x600 RGB565). Returns 0/-1. */
int fa_credits_render(const fa_credits *c, const struct fa_surface *dst);

#ifdef __cplusplus
}
#endif

#endif /* FA_CREDITS_H */
