/*
 * fa_input.h - input abstraction with a menu pointer
 *
 * Gameplay still uses the original keyboard model: a 256-entry DirectInput
 * keyboard-state array once per tick with zero buffered events, and
 * derives "just pressed" by diffing the previous tick. This module keeps that
 * model exactly: a 256-byte current array and a 256-byte previous array,
 * indexed by DIK scancode, with the previous array taken once per frame by
 * fa_input_begin_frame(). Up to two SDL_GameController states are kept
 * alongside it as logical button/axis models, ready for the host to assign
 * gameplay actions.
 *
 * The menu / front-end uses a pointer and button-DOWN edges only. The
 * original reads Win32 messages 512/513/515/516/256 and never handles
 * WM_LBUTTONUP. This module carries a pointer (x, y) and three
 * button states, again with a per-frame previous copy for the down edge.
 *
 * Binds are DIK scancodes in Option.ini lines 1-8:
 *   line 1-4   203 205 200 208   Left Right Up Down
 *   line 5-8    30  31  33  32   Jump(A) Fire(S) Action(F) spare(D)
 * fa_input_load_binds() / fa_input_save_binds() read and write that file
 * through the VFS, touching only lines 1-8 and preserving lines 9-22
 * (controller slots + volumes) byte for byte. Duplicate or unset (-1) binds
 * are rejected, exactly as the original reader does.
 *
 * Console fallback: fa_input_stick() integrates a virtual menu
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
#define FA_DIK_BACK     14        /* Backspace */
#define FA_DIK_A        30
#define FA_DIK_S        31
#define FA_DIK_D        32
#define FA_DIK_F        33
#define FA_DIK_T        20
#define FA_DIK_U        22
#define FA_DIK_O        24
#define FA_DIK_J        36
#define FA_DIK_K        37
#define FA_DIK_L        38
#define FA_DIK_P        25
#define FA_DIK_I        23
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

/* SDL's GameController layer exposes these logical buttons independent of
 * the physical layout (Xbox, PlayStation, Switch, etc.).  Keep the core free
 * of SDL types so headless and console backends can feed the same model. */
typedef enum {
    FA_PAD_A = 0,
    FA_PAD_B,
    FA_PAD_X,
    FA_PAD_Y,
    FA_PAD_BACK,
    FA_PAD_GUIDE,
    FA_PAD_START,
    FA_PAD_LEFTSTICK,
    FA_PAD_RIGHTSTICK,
    FA_PAD_LEFTSHOULDER,
    FA_PAD_RIGHTSHOULDER,
    FA_PAD_DPAD_UP,
    FA_PAD_DPAD_DOWN,
    FA_PAD_DPAD_LEFT,
    FA_PAD_DPAD_RIGHT,
    FA_PAD_BUTTON_COUNT
} fa_pad_button;

typedef enum {
    FA_PAD_AXIS_LEFT_X = 0,
    FA_PAD_AXIS_LEFT_Y,
    FA_PAD_AXIS_RIGHT_X,
    FA_PAD_AXIS_RIGHT_Y,
    FA_PAD_AXIS_COUNT
} fa_pad_axis;

#define FA_INPUT_MAX_PADS 2

/* Option.ini lines 9-22: 8 controller slots + 4 aux u16 + SFX vol + music
 * vol. Preserved verbatim across a save. */
#define FA_OPT_TAIL_LINES  14

typedef struct fa_input {
    unsigned char keys[256];
    unsigned char keys_prev[256];

    unsigned char pad_buttons[FA_INPUT_MAX_PADS][FA_PAD_BUTTON_COUNT];
    unsigned char pad_buttons_prev[FA_INPUT_MAX_PADS][FA_PAD_BUTTON_COUNT];
    float         pad_axes[FA_INPUT_MAX_PADS][FA_PAD_AXIS_COUNT]; /* -1..1 */
    unsigned char pad_connected[FA_INPUT_MAX_PADS];

    int           ptr_x, ptr_y;
    unsigned char btn[3];
    unsigned char btn_prev[3];

    int           ptr_w, ptr_h;        /* pointer clamp box (default 800x600) */
    unsigned char ptr_moved;            /* set by a feed during this frame */
    float         ptr_speed;           /* px / tick at full stick (default 8) */
    float         ptr_acc_x, ptr_acc_y;

    int           binds[FA_ACT_COUNT]; /* DIK scancode per action            */

    int           opt_tail[FA_OPT_TAIL_LINES];
    int           opt_tail_loaded;     /* 0 until a full Option.ini was read */
} fa_input;

/* The original defaults: arrows then A/S/F/D. */
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

/* Feed the SDL-agnostic GameController state for pad slot 0. Button and edge
 * queries are useful to menus; gameplay mappings remain an explicit policy in
 * the host. */
void  fa_input_set_pad_connected(fa_input *in, int connected);
void  fa_input_set_pad_button(fa_input *in, fa_pad_button btn, int down);
void  fa_input_set_pad_axis(fa_input *in, fa_pad_axis axis, float value);
int   fa_input_pad_connected(const fa_input *in);
int   fa_input_pad_button_down(const fa_input *in, fa_pad_button btn);
int   fa_input_pad_button_pressed(const fa_input *in, fa_pad_button btn);
float fa_input_pad_axis(const fa_input *in, fa_pad_axis axis);
int   fa_input_pointer_moved(const fa_input *in);

/* The same feed/query API for an explicit local-controller slot (0..1). */
void  fa_input_set_pad_connected_slot(fa_input *in, int slot, int connected);
void  fa_input_set_pad_button_slot(fa_input *in, int slot,
                                    fa_pad_button btn, int down);
void  fa_input_set_pad_axis_slot(fa_input *in, int slot,
                                  fa_pad_axis axis, float value);
int   fa_input_pad_connected_slot(const fa_input *in, int slot);
int   fa_input_pad_button_down_slot(const fa_input *in, int slot,
                                    fa_pad_button btn);
int   fa_input_pad_button_pressed_slot(const fa_input *in, int slot,
                                       fa_pad_button btn);
float fa_input_pad_axis_slot(const fa_input *in, int slot, fa_pad_axis axis);

/* --- console fallback --------------------------------------------- */

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

/*
 * The printable ASCII character a DIK scancode produces for text entry (the
 * high-score name field). Letters are always upper-case (the Schrift font
 * folds case anyway); digits, space and '-' pass through. Any non-text key
 * returns 0. The original reads the raw DirectInput array on that screen and
 * maps it itself (0x4093A0); this is the port's equivalent table.
 */
char fa_input_text_char(int dik);
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
 * trailing newline. Lines 1-8 are the current binds; lines 9-22 are
 * the verbatim tail from the last successful load, or the original defaults
 * (-1 x12, 75, 100) if none was loaded. Returns 0 or -1.
 */
int  fa_input_save_binds(fa_input *in, const struct fa_vfs *v, const char *vpath);

#ifdef __cplusplus
}
#endif

#endif /* FA_INPUT_H */
