/*
 * fa_input.h - input abstraction with a menu pointer (RRR-40, M4 gate)
 *
 * Parity / design basis: ENGINE-ARCH section 7, RRR-14 (PL-031/032) and
 * RRR-23 (PL-071).
 *
 * Gameplay is keyboard only. The original polls a 256-entry DirectInput
 * keyboard-state array once per tick with zero buffered events (PL-031) and
 * derives "just pressed" by diffing the previous tick. This module keeps that
 * model exactly: a 256-byte current array and a 256-byte previous array,
 * indexed by DIK scancode, with the previous array taken once per frame by
 * fa_input_begin_frame(). Backends fill the current array - an SDL backend
 * translates SDL scancodes to DIK, a console backend maps face buttons to the
 * bound scancodes.
 *
 * The menu / front-end uses a pointer and button-DOWN edges only. The
 * original reads Win32 messages 512/513/515/516/256 and never handles
 * WM_LBUTTONUP (PL-032). This module carries a pointer (x, y) and three
 * button states, again with a per-frame previous copy for the down edge.
 *
 * Binds are DIK scancodes in Option.ini lines 1-8 (PL-071):
 *   line 1-4   203 205 200 208   Left Right Up Down
 *   line 5-8    30  31  33  32   Jump(A) Fire(S) Action(F) spare(D)
 * fa_input_load_binds() / fa_input_save_binds() read and write that file
 * through the RRR-39 VFS, touching only lines 1-8 and preserving lines 9-22
 * (controller slots + volumes) byte for byte. Duplicate or unset (-1) binds
 * are rejected, exactly as the original reader does (RRR-23).
 *
 * Console fallback (RRR-40 AC2): fa_input_stick() integrates a virtual menu
 * pointer from a -1..1 stick vector; fa_input_pad() maps a face button to a
 * gameplay action.
 */
#ifndef FA_INPUT_H
#define FA_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

struct fa_vfs;

/* A handful of named DIK scancodes, for backends and tests. The array is
 * indexed by the full 0..255 range; these are just the ones the port uses. */
#define FA_DIK_ESCAPE   1
#define FA_DIK_RETURN   28
#define FA_DIK_A        30
#define FA_DIK_S        31
#define FA_DIK_D        32
#define FA_DIK_F        33
#define FA_DIK_P        25
#define FA_DIK_SPACE    57
#define FA_DIK_UP       200
#define FA_DIK_LEFT     203
#define FA_DIK_RIGHT    205
#define FA_DIK_DOWN     208

typedef enum {
    FA_ACT_LEFT = 0,
    FA_ACT_RIGHT,
    FA_ACT_UP,
    FA_ACT_DOWN,
    FA_ACT_JUMP,     /* A */
    FA_ACT_FIRE,     /* S */
    FA_ACT_ACTION,   /* F */
    FA_ACT_SPARE,    /* D */
    FA_ACT_COUNT
} fa_action;

/* Option.ini lines 9-22: 8 controller slots + 4 aux u16 + SFX vol + music
 * vol (RRR-23). Preserved verbatim across a save. */
#define FA_OPT_TAIL_LINES  14

typedef struct fa_input {
    unsigned char keys[256];
    unsigned char keys_prev[256];

    int           ptr_x, ptr_y;
    unsigned char btn[3];
    unsigned char btn_prev[3];

    int           ptr_w, ptr_h;        /* pointer clamp box (default 800x600) */
    float         ptr_speed;           /* px / tick at full stick (default 8) */
    float         ptr_acc_x, ptr_acc_y;

    int           binds[FA_ACT_COUNT]; /* DIK scancode per action            */

    int           opt_tail[FA_OPT_TAIL_LINES];
    int           opt_tail_loaded;     /* 0 until a full Option.ini was read */
} fa_input;

/* The original defaults (PL-071): arrows then A/S/F/D. */
extern const int FA_INPUT_DEFAULT_BINDS[FA_ACT_COUNT];

/* --- lifecycle ------------------------------------------------------- */

/* Default binds, 800x600 pointer box, speed 8, everything else cleared. */
void fa_input_init(fa_input *in);

/* Latch the frame: keys_prev = keys, btn_prev = btn. Call once per frame
 * before the backend feeds this frame's state. */
void fa_input_begin_frame(fa_input *in);

/* --- backend feeds -------------------------------------------------- */

void fa_input_set_key(fa_input *in, int dik, int down);
void fa_input_set_pointer(fa_input *in, int x, int y);
void fa_input_set_button(fa_input *in, int btn, int down);   /* btn 0..2 */

/* --- console fallback (RRR-40 AC2) --------------------------------- */

/* Move the virtual pointer by one tick's worth of a -1..1 stick vector.
 * A radius below 0.15 is treated as centred. */
void fa_input_stick(fa_input *in, float ax, float ay);

/* Map a held face button to a gameplay action (sets the bound scancode). */
void fa_input_pad(fa_input *in, fa_action act, int down);

void fa_input_set_pointer_bounds(fa_input *in, int w, int h);
void fa_input_set_pointer_speed(fa_input *in, float px_per_tick);

/* --- gameplay queries -------------------------------------------- */

int  fa_input_key_down(const fa_input *in, int dik);
int  fa_input_key_pressed(const fa_input *in, int dik);   /* down edge  */
int  fa_input_key_released(const fa_input *in, int dik);  /* up edge    */
int  fa_input_action_down(const fa_input *in, fa_action a);
int  fa_input_action_pressed(const fa_input *in, fa_action a);

/* --- menu queries ---------------------------------------------- */

void fa_input_pointer(const fa_input *in, int *x, int *y);
int  fa_input_button_down(const fa_input *in, int btn);
int  fa_input_button_pressed(const fa_input *in, int btn);   /* down edge */

/* --- binds --------------------------------------------------- */

int  fa_input_get_bind(const fa_input *in, fa_action a);

/* Set one bind. Returns 0, or -1 if `dik` is outside 1..255 or already bound
 * to another action (the original rejects duplicates). */
int  fa_input_set_bind(fa_input *in, fa_action a, int dik);

void fa_input_reset_binds(fa_input *in);

/*
 * Read Option.ini through the VFS. `vpath` NULL means "user:Option.ini".
 * Returns 0 on a clean read, 1 if the file is absent (defaults kept), or -1
 * if it is present but malformed (fewer than 22 integer lines). A duplicate
 * or -1 bind line is not an error: that action keeps its default and the
 * rest of the file still loads.
 */
int  fa_input_load_binds(fa_input *in, const struct fa_vfs *v, const char *vpath);

/*
 * Write Option.ini through the VFS: 22 CRLF-separated integer lines, no
 * trailing newline (RRR-23). Lines 1-8 are the current binds; lines 9-22 are
 * the verbatim tail from the last successful load, or the original defaults
 * (-1 x12, 75, 100) if none was loaded. Returns 0 or -1.
 */
int  fa_input_save_binds(fa_input *in, const struct fa_vfs *v, const char *vpath);

#ifdef __cplusplus
}
#endif

#endif /* FA_INPUT_H */
