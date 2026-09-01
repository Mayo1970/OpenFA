/*
 * fa_death.h - what happens when the kid's health hits 0 (RRR-53)
 *
 * The kid has ONE survival meter: health (0x45F014, RRR-51). There is no
 * lives counter. When health reaches 0 the run is OVER:
 *
 *   1. the player is locked into the KO animation for 240 ticks (4.0 s at
 *      60 Hz) - the rest of the level keeps running (enemies, platforms,
 *      projectiles all keep moving);
 *   2. then the exe plays Start.wav (the title music) and does a 16-tick fade;
 *   3. then it goes to the CLASSIFICA / high-score screen for the world that
 *      was being played (scene 15), showing the run's score;
 *   4. then the world-select menu.
 *
 * There is NO in-place level restart and NO "game over" screen - death just
 * ends the run and sends you back to the front-end. The score resets when the
 * menu comes up (0x41154C).
 *
 * Reverse-engineered by hand off jr_disasm.txt + the exe .rdata (PL-141,
 * corrected 2026-08-31 - Codex was unavailable):
 *   - trigger: player update fcn.00417370 @ 0x417419 - health <= 0 and the
 *     death countdown 0x4E0B44 == -1 -> 0x4E0B44 = 0xF0 (240), sub-timer
 *     0x4E102C = 0x1E (30), player anim state 0x4E1028 = 0x22 (penguin) or
 *     0x24 (Milchschnitte) - selected by 0x4E1020, the ACTIVE CHARACTER index
 *     (0/1; set at player init @0x417294 and on a completed swap @0x417DCF).
 *     Camera mode 0x4DABA8 = 1 (the level-intro pan). Each tick 0x4E0B44--;
 *     at 0 -> scene request 0x45F008 = 2.
 *   - anim: state 0x22 -> 0x23 = PINGUIN.W01 frames 150..162 (loop 158);
 *     0x24 -> 0x25 = MILCHSCHNITTE.W01 36..48 (loop 44). Same clips fa_slice
 *     binds as FA_CS_KO. The shared player integrator (0x432350 -> 0x431BF0 +
 *     terrain sweep 0x431E90) still runs, so the KO body is launched once
 *     (0x431A00/0x431A20: vx +/-18.0, vy -6.0 in 20.12) and then arcs down
 *     and LANDS on the terrain - it is not a free-fall. Ticks 0..30 spawn 5
 *     debris (0x430470) per tick - cosmetic, not modelled.
 *   - the run keeps ticking: fcn.004110C0 case 1 (0x41110B) calls the player
 *     update then loops the whole entity table (0x40AE10) with NO death guard.
 *   - end of run: 0x45F008 = 2 -> fcn.004110C0 case @ 0x41272D: teardown
 *     (0x410D50), Start.wav on ch16 (0x4243B1 "GData\SDat\Start.wav"), fade
 *     0x45ED42 = 0x10 step 1, then 0x45F008 = 0xF and 0x4DABD4 &= 3. Scene 15
 *     = fcn.00402DB0 @ 0x402DE4 loads HighscoreBG.bmp + Schrift.w01 + the four
 *     Highscore{1-4}.dat, formats the score ("%d" @0x455358) - the CLASSIFICA
 *     screen - then hands on to the menu.
 *   See RRR-53/death-and-restart-disasm.md.
 *
 * This module is just the deterministic phase + timer for steps 1-2 (LAUNCH
 * 30, HOLD to 240, FADE 16, DONE). Integer-only. The host (fa_slice) drives
 * the KO pose, keeps the level running, and on DONE tears the level down and
 * shows the CLASSIFICA screen. The KO launch velocity is set on the player
 * body by the host, not here.
 */
#ifndef FA_DEATH_H
#define FA_DEATH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Timing, straight from the disassembly. */
#define FA_DEATH_LAUNCH_TICKS  30   /* 0x4E102C = 0x1E - launch + debris beat  */
#define FA_DEATH_HOLD_TICKS    240  /* 0x4E0B44 = 0xF0 - KO hold -> end of run */
#define FA_DEATH_FADE_TICKS    16   /* 0x45ED42 = 0x10, step 1 - the fade      */

typedef enum {
    FA_DEATH_ALIVE = 0,
    FA_DEATH_LAUNCH,   /* ticks   0..30  - KO clip plays, body launched         */
    FA_DEATH_HOLD,     /* ticks  30..240 - KO clip holds, level still running   */
    FA_DEATH_FADE,     /* ticks 240..256 - Start.wav + fade to black            */
    FA_DEATH_DONE      /* the run is over - host shows the CLASSIFICA screen     */
} fa_death_phase;

typedef struct fa_death {
    fa_death_phase phase;
    int      tick;           /* ticks since fa_death_begin (0 while ALIVE)     */
    int      character;      /* 0 penguin (frames 150..162) / 1 Milch (36..48) */
    int      facing;         /* -1 / +1 latched at death                       */
    int      over;           /* 1 for exactly the tick the run ends (-> fade)  */
    int      fade;           /* FADE phase: FA_DEATH_FADE_TICKS -> 0           */
    uint32_t hash;           /* rolling FNV-1a-32 over each tick (tests)       */
} fa_death;

/* Reset to ALIVE. Call on every level (re)load and when the run ends. */
void fa_death_init(fa_death *d);

/* Enter the death sequence. `character` (0 penguin / 1 Milchschnitte) picks
 * the KO clip; `facing` is -1 or +1. No-op if already dying. */
void fa_death_begin(fa_death *d, int character, int facing);

/* Advance one 60 Hz tick. No-op while ALIVE or DONE. */
void fa_death_tick(fa_death *d);

/* --- queries ---------------------------------------------------------- */

int  fa_death_active(const fa_death *d);   /* phase is not ALIVE and not DONE  */
int  fa_death_over(const fa_death *d);     /* one-shot: the 240-tick hold ended*/
fa_death_phase fa_death_phase_of(const fa_death *d);

/* Current player .W01 frame for the KO animation (-1 while ALIVE). */
int  fa_death_anim_frame(const fa_death *d);

/* Fade cover for the host: 0 (clear) .. FA_DEATH_FADE_TICKS (opaque). 0
 * outside the FADE phase. */
int  fa_death_fade_amount(const fa_death *d);

const char *fa_death_phase_name(fa_death_phase p);

#ifdef __cplusplus
}
#endif

#endif /* FA_DEATH_H */
