/*
 * fa_options.h - the option-screen interaction model
 *
 * Reverse-engineered from JR_FERRERO.exe option handler 0x405740..0x4081ED.
 *
 * KNOWN GAP: the shipped `GData\Animation\Dummy\Option Screen\` art is ABSENT
 * from this install, so a faithful option SCREEN cannot be drawn. What IS
 * fully static and reproduced here is the interaction model: two 0..100
 * volume sliders, keyboard + controller rebinding with the duplicate-swap
 * rule, and writing all 22 Option.ini lines on exit (fa_save.h). The exact
 * back-button and knob pixel sizes come from the missing W01 and are not
 * reproduced.
 *
 * Slider geometry, 800x600 (knob-origin X):
 *     music  Y=542, origin X 45..224   (v -> 45  + floor(179*v/100))
 *     sound  Y=542, origin X 345..525  (v -> 345 + floor(180*v/100))
 * Dragging subtracts half the knob width from the cursor before clamping;
 * the knob width is asset data, so this model takes it as a parameter.
 */
#ifndef FA_OPTIONS_H
#define FA_OPTIONS_H

#include "fa/fa_save.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { FA_SLIDER_MUSIC = 0, FA_SLIDER_SOUND = 1 } fa_slider;

/* Option.ini index for a slider's value: music = line 21 (idx 20),
 * sound = line 22 (idx 21). */
#define FA_OPT_IDX_MUSIC 20
#define FA_OPT_IDX_SOUND 21

/* Knob-origin X for value `v` (0..100), 800x600 (init formula 0x405C34 /
 * 0x405C89). */
int fa_options_knob_x(fa_slider s, int v);

/* Value 0..100 from a knob-origin X (clamp + map, 0x407153 / 0x4072B4). */
int fa_options_value_from_x(fa_slider s, int origin_x);

/* Value 0..100 from a raw cursor X, given the knob width (drag path
 * 0x4070AE: subtract half the knob width, then clamp + map). */
int fa_options_value_from_cursor(fa_slider s, int cursor_x, int knob_w);

/*
 * Rebind action `idx` (0..7 -> Option.ini lines 1..8) to DirectInput scancode
 * `dik`. If another action already holds `dik`, that action takes the value
 * `idx` had (the exe's swap-a-duplicate rule, 0x407628..0x4076D1). Returns 0,
 * or -1 on a bad index.
 */
int fa_options_rebind(fa_opt_file *opt, int idx, int dik);

#ifdef __cplusplus
}
#endif

#endif /* FA_OPTIONS_H */
