/*
 * fa_rng.h - the game's pseudo-random number generator (RRR-52)
 *
 * Parity basis: JR_FERRERO.exe rand/srand, identified by hand off jr_disasm.txt.
 *
 *   srand  fcn @0x4395A6 : ds:0x45AD24 = seed                (one 32-bit word)
 *   rand   fcn @0x4395B0 : ds:0x45AD24 = ds:0x45AD24 * 0x343FD + 0x269EC3
 *                          return (int)((ds:0x45AD24 >> 16) & 0x7FFF)
 *
 * That is the stock Microsoft C run-time linear congruential generator
 * (multiplier 214013, increment 2531011, 15-bit output 0..32767). The exe
 * keeps ONE global stream. It is seeded once at start-up (fcn @0x42AE27:
 * srand(wall_clock_field & 0xFFFF)), so the original's sequence is different
 * on every run. Callers reduce a draw with a signed `cdq; idiv` - i.e.
 * `rand() % n` (verified at 0x4193CE..0x4193DB and the enemy handlers, PL-137).
 *
 * The port keeps the exact algorithm (AC1) but seeds it deterministically
 * (a fixed default, override with fa_rng_seed): the fixed-timestep sim must
 * replay bit-for-bit (RRR-34). Every fa_rng instance is independent; the game
 * layer owns one shared instance to mirror the exe's single stream.
 *
 * PL-140.
 */
#ifndef FA_RNG_H
#define FA_RNG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FA_RNG_MULT  214013u    /* 0x343FD  */
#define FA_RNG_INC   2531011u   /* 0x269EC3 */
#define FA_RNG_MAX   0x7FFF     /* rand() upper bound, inclusive */

typedef struct fa_rng {
    uint32_t state;
} fa_rng;

/* srand: set the stream state. Any value is valid (0 included). */
void fa_rng_seed(fa_rng *r, uint32_t seed);

/* rand: advance the stream and return the next value in 0..FA_RNG_MAX. */
int  fa_rng_next(fa_rng *r);

/* rand() % n, matching the exe's signed reduction. n <= 0 returns 0. */
int  fa_rng_below(fa_rng *r, int n);

/* An integer in [lo, hi] inclusive (lo + rand() % (hi - lo + 1)).
 * hi <= lo returns lo. */
int  fa_rng_range(fa_rng *r, int lo, int hi);

/* Raw state, for snapshot / replay checks. */
uint32_t fa_rng_state(const fa_rng *r);
void     fa_rng_set_state(fa_rng *r, uint32_t state);

#ifdef __cplusplus
}
#endif

#endif /* FA_RNG_H */
