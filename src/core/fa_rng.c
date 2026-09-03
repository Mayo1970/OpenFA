/*
 * fa_rng.c - the game's PRNG. See fa_rng.h.
 *
 * Exact port of JR_FERRERO.exe rand/srand (fcn @0x4395B0 / @0x4395A6):
 * the stock MSVC linear congruential generator.
 */
#include "fa/fa_rng.h"

void fa_rng_seed(fa_rng *r, uint32_t seed)
{
    if (r) r->state = seed;
}

int fa_rng_next(fa_rng *r)
{
    if (!r) return 0;
    /* 0x4395B5 imul eax,eax,0x343FD ; 0x4395BB add eax,0x269EC3
     * 0x4395C5 sar eax,0x10        ; 0x4395C8 and eax,0x7FFF        */
    r->state = r->state * FA_RNG_MULT + FA_RNG_INC;
    return (int)((r->state >> 16) & (uint32_t)FA_RNG_MAX);
}

int fa_rng_below(fa_rng *r, int n)
{
    if (n <= 0) return 0;
    /* rand() is 0..32767, so the signed idiv in the exe and an unsigned
     * modulo agree here. */
    return fa_rng_next(r) % n;
}

int fa_rng_range(fa_rng *r, int lo, int hi)
{
    if (hi <= lo) return lo;
    return lo + fa_rng_below(r, hi - lo + 1);
}

uint32_t fa_rng_state(const fa_rng *r)
{
    return r ? r->state : 0u;
}

void fa_rng_set_state(fa_rng *r, uint32_t state)
{
    if (r) r->state = state;
}
