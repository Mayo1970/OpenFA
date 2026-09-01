/*
 * fa_options.c - the option-screen interaction model (RRR-47). See fa_options.h.
 */
#include "fa/fa_options.h"

/* 800x600 knob-origin X range per slider (menu-disasm.md section 2). */
static void slider_range(fa_slider s, int *lo, int *span)
{
    if (s == FA_SLIDER_MUSIC) { *lo = 45;  *span = 179; }
    else                      { *lo = 345; *span = 180; }
}

int fa_options_knob_x(fa_slider s, int v)
{
    int lo, span;
    slider_range(s, &lo, &span);
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return lo + (span * v) / 100;          /* floor, matches 0x405C34 */
}

int fa_options_value_from_x(fa_slider s, int origin_x)
{
    int lo, span;
    slider_range(s, &lo, &span);
    if (origin_x < lo) origin_x = lo;
    if (origin_x > lo + span) origin_x = lo + span;
    /* round to nearest, matching the exe's (dx*100 + span/2)/span */
    return ((origin_x - lo) * 100 + span / 2) / span;
}

int fa_options_value_from_cursor(fa_slider s, int cursor_x, int knob_w)
{
    return fa_options_value_from_x(s, cursor_x - knob_w / 2);
}

int fa_options_rebind(fa_opt_file *opt, int idx, int dik)
{
    if (!opt || idx < 0 || idx > 7) return -1;
    int old = opt->line[idx];
    for (int i = 0; i < 8; i++)
        if (i != idx && opt->line[i] == dik) {
            opt->line[i] = old;            /* the duplicate takes the old bind */
            break;
        }
    opt->line[idx] = dik;
    return 0;
}
