/*
 * fa_input.c - input abstraction with a menu pointer (RRR-40, M4 gate).
 * See include/fa/fa_input.h. Zero external dependencies (uses fa_vfs for the
 * Option.ini round trip).
 */
#include "fa/fa_input.h"
#include "fa/fa_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const int FA_INPUT_DEFAULT_BINDS[FA_ACT_COUNT] = {
    FA_DIK_LEFT, FA_DIK_RIGHT, FA_DIK_UP, FA_DIK_DOWN,   /* 203 205 200 208 */
    FA_DIK_A,    FA_DIK_S,     FA_DIK_F,  FA_DIK_D        /*  30  31  33  32 */
};

/* Option.ini lines 9-22 when the file is absent (RRR-23 defaults, no pad). */
static const int OPT_TAIL_DEFAULT[FA_OPT_TAIL_LINES] = {
    -1, -1, -1, -1, -1, -1, -1, -1,   /* 9-16  controller slots  */
    -1, -1, -1, -1,                   /* 17-20 aux u16 pairs      */
    75, 100                           /* 21-22 SFX vol, music vol */
};

/* ------------------------------------------------------------- lifecycle */

void fa_input_init(fa_input *in)
{
    memset(in, 0, sizeof *in);
    in->ptr_w = 800;
    in->ptr_h = 600;
    in->ptr_x = 400;
    in->ptr_y = 300;
    in->ptr_speed = 8.0f;
    for (int i = 0; i < FA_ACT_COUNT; i++)
        in->binds[i] = FA_INPUT_DEFAULT_BINDS[i];
    memcpy(in->opt_tail, OPT_TAIL_DEFAULT, sizeof in->opt_tail);
    in->opt_tail_loaded = 0;
}

void fa_input_begin_frame(fa_input *in)
{
    memcpy(in->keys_prev, in->keys, sizeof in->keys);
    memcpy(in->btn_prev, in->btn, sizeof in->btn);
    memcpy(in->pad_buttons_prev, in->pad_buttons, sizeof in->pad_buttons);
    in->ptr_moved = 0;
}

/* ------------------------------------------------------------ backend feeds */

void fa_input_set_key(fa_input *in, int dik, int down)
{
    if (dik < 0 || dik > 255) return;
    in->keys[dik] = down ? 1u : 0u;
}

char fa_input_text_char(int dik)
{
    /* DIK scancode -> ASCII, for the high-score name field. */
    static const char row_q[] = "QWERTYUIOP";   /* 0x10..0x19 */
    static const char row_a[] = "ASDFGHJKL";    /* 0x1E..0x26 */
    static const char row_z[] = "ZXCVBNM";      /* 0x2C..0x32 */
    if (dik >= 0x10 && dik <= 0x19) return row_q[dik - 0x10];
    if (dik >= 0x1E && dik <= 0x26) return row_a[dik - 0x1E];
    if (dik >= 0x2C && dik <= 0x32) return row_z[dik - 0x2C];
    if (dik >= 0x02 && dik <= 0x0A) return (char)('1' + (dik - 0x02));  /* 1..9 */
    if (dik == 0x0B) return '0';
    if (dik == 0x0C) return '-';
    if (dik == 0x34) return '.';
    if (dik == FA_DIK_SPACE) return ' ';
    return 0;
}

void fa_input_set_pointer(fa_input *in, int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= in->ptr_w) x = in->ptr_w - 1;
    if (y >= in->ptr_h) y = in->ptr_h - 1;
    if (x != in->ptr_x || y != in->ptr_y) in->ptr_moved = 1u;
    in->ptr_x = x;
    in->ptr_y = y;
    in->ptr_acc_x = 0.0f;
    in->ptr_acc_y = 0.0f;
}

void fa_input_set_button(fa_input *in, int btn, int down)
{
    if (btn < 0 || btn > 2) return;
    in->btn[btn] = down ? 1u : 0u;
}

void fa_input_set_pad_connected(fa_input *in, int connected)
{
    fa_input_set_pad_connected_slot(in, 0, connected);
}

void fa_input_set_pad_button(fa_input *in, fa_pad_button btn, int down)
{
    fa_input_set_pad_button_slot(in, 0, btn, down);
}

void fa_input_set_pad_axis(fa_input *in, fa_pad_axis axis, float value)
{
    fa_input_set_pad_axis_slot(in, 0, axis, value);
}

int fa_input_pad_connected(const fa_input *in)
{
    return fa_input_pad_connected_slot(in, 0);
}

int fa_input_pad_button_down(const fa_input *in, fa_pad_button btn)
{
    return fa_input_pad_button_down_slot(in, 0, btn);
}

int fa_input_pad_button_pressed(const fa_input *in, fa_pad_button btn)
{
    return fa_input_pad_button_pressed_slot(in, 0, btn);
}

float fa_input_pad_axis(const fa_input *in, fa_pad_axis axis)
{
    return fa_input_pad_axis_slot(in, 0, axis);
}

void fa_input_set_pad_connected_slot(fa_input *in, int slot, int connected)
{
    if (!in || slot < 0 || slot >= FA_INPUT_MAX_PADS) return;
    if (connected) {
        in->pad_connected[slot] = 1u;
        return;
    }

    in->pad_connected[slot] = 0u;
    memset(in->pad_buttons[slot], 0, sizeof in->pad_buttons[slot]);
    memset(in->pad_axes[slot], 0, sizeof in->pad_axes[slot]);
}

void fa_input_set_pad_button_slot(fa_input *in, int slot,
                                   fa_pad_button btn, int down)
{
    if (!in || slot < 0 || slot >= FA_INPUT_MAX_PADS ||
        btn < 0 || btn >= FA_PAD_BUTTON_COUNT) return;
    in->pad_buttons[slot][btn] = down ? 1u : 0u;
}

void fa_input_set_pad_axis_slot(fa_input *in, int slot,
                                 fa_pad_axis axis, float value)
{
    if (!in || slot < 0 || slot >= FA_INPUT_MAX_PADS ||
        axis < 0 || axis >= FA_PAD_AXIS_COUNT) return;
    if (value < -1.0f) value = -1.0f;
    if (value >  1.0f) value = 1.0f;
    in->pad_axes[slot][axis] = value;
}

int fa_input_pad_connected_slot(const fa_input *in, int slot)
{
    return in && slot >= 0 && slot < FA_INPUT_MAX_PADS &&
           in->pad_connected[slot] != 0;
}

int fa_input_pad_button_down_slot(const fa_input *in, int slot,
                                  fa_pad_button btn)
{
    if (!in || slot < 0 || slot >= FA_INPUT_MAX_PADS ||
        btn < 0 || btn >= FA_PAD_BUTTON_COUNT) return 0;
    return in->pad_buttons[slot][btn] != 0;
}

int fa_input_pad_button_pressed_slot(const fa_input *in, int slot,
                                     fa_pad_button btn)
{
    if (!in || slot < 0 || slot >= FA_INPUT_MAX_PADS ||
        btn < 0 || btn >= FA_PAD_BUTTON_COUNT) return 0;
    return in->pad_buttons[slot][btn] &&
           !in->pad_buttons_prev[slot][btn];
}

float fa_input_pad_axis_slot(const fa_input *in, int slot, fa_pad_axis axis)
{
    if (!in || slot < 0 || slot >= FA_INPUT_MAX_PADS ||
        axis < 0 || axis >= FA_PAD_AXIS_COUNT) return 0.0f;
    return in->pad_axes[slot][axis];
}

int fa_input_pointer_moved(const fa_input *in)
{
    return in->ptr_moved != 0;
}

/* ------------------------------------------------------- console fallback */

void fa_input_set_pointer_bounds(fa_input *in, int w, int h)
{
    if (w > 0) in->ptr_w = w;
    if (h > 0) in->ptr_h = h;
    if (in->ptr_x >= in->ptr_w) in->ptr_x = in->ptr_w - 1;
    if (in->ptr_y >= in->ptr_h) in->ptr_y = in->ptr_h - 1;
}

void fa_input_set_pointer_speed(fa_input *in, float px_per_tick)
{
    if (px_per_tick > 0.0f) in->ptr_speed = px_per_tick;
}

void fa_input_stick(fa_input *in, float ax, float ay)
{
    float r2 = ax * ax + ay * ay;
    if (r2 < 0.15f * 0.15f) return;          /* deadzone */

    if (ax > 1.0f) ax = 1.0f;
    if (ax < -1.0f) ax = -1.0f;
    if (ay > 1.0f) ay = 1.0f;
    if (ay < -1.0f) ay = -1.0f;

    in->ptr_acc_x += ax * in->ptr_speed;
    in->ptr_acc_y += ay * in->ptr_speed;

    int dx = (int)in->ptr_acc_x;
    int dy = (int)in->ptr_acc_y;
    in->ptr_acc_x -= (float)dx;
    in->ptr_acc_y -= (float)dy;

    int nx = in->ptr_x + dx;
    int ny = in->ptr_y + dy;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx >= in->ptr_w) nx = in->ptr_w - 1;
    if (ny >= in->ptr_h) ny = in->ptr_h - 1;
    if (nx != in->ptr_x || ny != in->ptr_y) in->ptr_moved = 1u;
    in->ptr_x = nx;
    in->ptr_y = ny;
}

void fa_input_pad(fa_input *in, fa_action act, int down)
{
    if (act < 0 || act >= FA_ACT_COUNT) return;
    int dik = in->binds[act];
    if (dik >= 1 && dik <= 255)
        in->keys[dik] = down ? 1u : 0u;
}

/* ---------------------------------------------------------- gameplay query */

int fa_input_key_down(const fa_input *in, int dik)
{
    if (dik < 0 || dik > 255) return 0;
    return in->keys[dik] != 0;
}

int fa_input_key_pressed(const fa_input *in, int dik)
{
    if (dik < 0 || dik > 255) return 0;
    return in->keys[dik] && !in->keys_prev[dik];
}

int fa_input_key_released(const fa_input *in, int dik)
{
    if (dik < 0 || dik > 255) return 0;
    return !in->keys[dik] && in->keys_prev[dik];
}

int fa_input_action_down(const fa_input *in, fa_action a)
{
    if (a < 0 || a >= FA_ACT_COUNT) return 0;
    return fa_input_key_down(in, in->binds[a]);
}

int fa_input_action_pressed(const fa_input *in, fa_action a)
{
    if (a < 0 || a >= FA_ACT_COUNT) return 0;
    return fa_input_key_pressed(in, in->binds[a]);
}

/* -------------------------------------------------------------- menu query */

void fa_input_pointer(const fa_input *in, int *x, int *y)
{
    if (x) *x = in->ptr_x;
    if (y) *y = in->ptr_y;
}

int fa_input_button_down(const fa_input *in, int btn)
{
    if (btn < 0 || btn > 2) return 0;
    return in->btn[btn] != 0;
}

int fa_input_button_pressed(const fa_input *in, int btn)
{
    if (btn < 0 || btn > 2) return 0;
    return in->btn[btn] && !in->btn_prev[btn];
}

/* ------------------------------------------------------------------- binds */

int fa_input_get_bind(const fa_input *in, fa_action a)
{
    if (a < 0 || a >= FA_ACT_COUNT) return -1;
    return in->binds[a];
}

int fa_input_set_bind(fa_input *in, fa_action a, int dik)
{
    if (a < 0 || a >= FA_ACT_COUNT) return -1;
    if (dik < 1 || dik > 255) return -1;
    for (int i = 0; i < FA_ACT_COUNT; i++)
        if (i != a && in->binds[i] == dik) return -1;   /* no duplicates */
    in->binds[a] = dik;
    return 0;
}

void fa_input_reset_binds(fa_input *in)
{
    for (int i = 0; i < FA_ACT_COUNT; i++)
        in->binds[i] = FA_INPUT_DEFAULT_BINDS[i];
}

/* --------------------------------------------------------- Option.ini I/O */

#define OPT_LINES  22

/* Parse up to OPT_LINES signed integers from whitespace-separated text.
 * Returns the count parsed. */
static int parse_ints(const char *s, int *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n'))
            s++;
        if (!*s) break;
        char *end = NULL;
        long v = strtol(s, &end, 10);
        if (end == s) break;
        out[n++] = (int)v;
        s = end;
    }
    return n;
}

int fa_input_load_binds(fa_input *in, const struct fa_vfs *v, const char *vpath)
{
    if (!vpath) vpath = "user:Option.ini";

    void *buf = NULL;
    size_t len = 0;
    if (!v || fa_vfs_read_all(v, vpath, &buf, &len) != 0)
        return 1;                        /* absent: keep defaults */

    int line[OPT_LINES];
    int got = parse_ints((const char *)buf, line, OPT_LINES);
    free(buf);
    if (got < OPT_LINES)
        return -1;                       /* malformed */

    /* lines 1-8: binds, per-slot validated (range, no duplicates) */
    int fresh[FA_ACT_COUNT];
    for (int i = 0; i < FA_ACT_COUNT; i++)
        fresh[i] = FA_INPUT_DEFAULT_BINDS[i];

    for (int i = 0; i < FA_ACT_COUNT; i++) {
        int dik = line[i];
        if (dik < 1 || dik > 255) continue;          /* -1 / out of range */
        int dup = 0;
        for (int j = 0; j < i; j++)
            if (fresh[j] == dik) { dup = 1; break; }
        if (dup) continue;
        fresh[i] = dik;
    }
    /* a later default could still collide with an accepted earlier bind;
     * drop it back to a free scancode-free state by leaving the default only
     * when it does not clash, else keep the accepted value. Re-scan once. */
    for (int i = 0; i < FA_ACT_COUNT; i++) {
        for (int j = 0; j < FA_ACT_COUNT; j++) {
            if (i != j && fresh[i] == fresh[j]) {
                fresh[i] = FA_INPUT_DEFAULT_BINDS[i];
                break;
            }
        }
    }
    for (int i = 0; i < FA_ACT_COUNT; i++)
        in->binds[i] = fresh[i];

    /* lines 9-22: keep verbatim for the next save */
    for (int i = 0; i < FA_OPT_TAIL_LINES; i++)
        in->opt_tail[i] = line[FA_ACT_COUNT + i];
    in->opt_tail_loaded = 1;
    return 0;
}

int fa_input_save_binds(fa_input *in, const struct fa_vfs *v, const char *vpath)
{
    if (!v) return -1;
    if (!vpath) vpath = "user:Option.ini";

    char text[512];
    size_t o = 0;
    for (int i = 0; i < OPT_LINES; i++) {
        int val = (i < FA_ACT_COUNT) ? in->binds[i]
                                     : in->opt_tail[i - FA_ACT_COUNT];
        int n = snprintf(text + o, sizeof text - o,
                         "%s%d", (i == 0) ? "" : "\r\n", val);
        if (n < 0 || (size_t)n >= sizeof text - o) return -1;
        o += (size_t)n;
    }
    return fa_vfs_write_all(v, vpath, text, o);
}
