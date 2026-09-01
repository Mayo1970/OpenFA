/*
 * fa_death.c - the KO-hold + fade timer for a lost run (RRR-53). See
 * fa_death.h for the disassembly this mirrors (PL-141).
 */
#include "fa/fa_death.h"

#include <string.h>

/* KO clip spans: {first, loop, last}. Play first..last once (1 frame/tick,
 * RRR-9 frame-lock), then loop..last. Same values fa_slice binds as FA_CS_KO. */
static const int SPAN_PENGUIN[3] = { 150, 158, 162 };  /* state 0x22/0x23 */
static const int SPAN_MILCH[3]   = {  36,  44,  48 };  /* state 0x24/0x25 */

static void mix(fa_death *d, uint32_t w)
{
    for (int i = 0; i < 4; i++) {
        d->hash ^= (w & 0xffu);
        d->hash *= 16777619u;
        w >>= 8;
    }
}

void fa_death_init(fa_death *d)
{
    memset(d, 0, sizeof *d);
    d->phase = FA_DEATH_ALIVE;
    d->facing = 1;
    d->hash = 2166136261u;
}

void fa_death_begin(fa_death *d, int character, int facing)
{
    if (d->phase != FA_DEATH_ALIVE) return;   /* already dying */

    uint32_t keep = d->hash ? d->hash : 2166136261u;
    memset(d, 0, sizeof *d);
    d->hash      = keep;
    d->phase     = FA_DEATH_LAUNCH;
    d->tick      = 0;
    d->character = character ? 1 : 0;
    d->facing    = (facing < 0) ? -1 : 1;
}

void fa_death_tick(fa_death *d)
{
    if (d->phase == FA_DEATH_ALIVE || d->phase == FA_DEATH_DONE) return;

    d->over = 0;
    d->tick++;

    switch (d->phase) {
    case FA_DEATH_LAUNCH:
        if (d->tick >= FA_DEATH_LAUNCH_TICKS) d->phase = FA_DEATH_HOLD;
        break;

    case FA_DEATH_HOLD:
        if (d->tick >= FA_DEATH_HOLD_TICKS) {
            /* the exe sets 0x45F008 = 2 here - the run is over */
            d->phase = FA_DEATH_FADE;
            d->fade = FA_DEATH_FADE_TICKS;
            d->over = 1;
        }
        break;

    case FA_DEATH_FADE:
        if (d->fade > 0) d->fade--;
        if (d->fade == 0) d->phase = FA_DEATH_DONE;
        break;

    default:
        break;
    }

    mix(d, (uint32_t)d->phase);
    mix(d, (uint32_t)d->tick);
    mix(d, (uint32_t)fa_death_anim_frame(d));
    mix(d, (uint32_t)((d->fade << 4) | (d->character << 1) | d->over));
}

int fa_death_active(const fa_death *d)
{
    return d->phase != FA_DEATH_ALIVE && d->phase != FA_DEATH_DONE;
}

int fa_death_over(const fa_death *d) { return d->over; }

fa_death_phase fa_death_phase_of(const fa_death *d) { return d->phase; }

int fa_death_anim_frame(const fa_death *d)
{
    if (d->phase == FA_DEATH_ALIVE) return -1;
    const int *s = d->character ? SPAN_MILCH : SPAN_PENGUIN;
    int run = s[2] - s[0];              /* first..last length in ticks */
    int t = d->tick;
    if (t <= run) return s[0] + t;      /* one pass */
    int loop = s[2] - s[1] + 1;         /* loop..last inclusive */
    return s[1] + (t - run - 1) % loop;
}

int fa_death_fade_amount(const fa_death *d)
{
    return (d->phase == FA_DEATH_FADE) ? d->fade : 0;
}

const char *fa_death_phase_name(fa_death_phase p)
{
    switch (p) {
    case FA_DEATH_ALIVE:  return "alive";
    case FA_DEATH_LAUNCH: return "launch";
    case FA_DEATH_HOLD:   return "hold";
    case FA_DEATH_FADE:   return "fade";
    case FA_DEATH_DONE:   return "done";
    default:              return "?";
    }
}
