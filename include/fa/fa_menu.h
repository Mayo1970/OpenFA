/*
 * fa_menu.h - the title / world-select screen (RRR-47)
 *
 * Reverse-engineered from JR_FERRERO.exe: world-select state handler
 * 0x4042B0..0x404F72, the shared sprite hit test 0x409130, and the frame
 * selection at 0x404343..0x404CDB (RRR-47/menu-disasm.md, PL-121/122).
 *
 * The screen: blit GData\Pics\StartBG.bmp, then place six
 * GData\Animation\Interface sprites at each frame's table-A origin
 * (fa_w01_frame_origin, PL-079). Per button:
 *
 *   file            world / action     click stage (0x4DABD4)
 *   GIUNGLA.W01     world 1 (jungle)   0
 *   MONTAGNA.W01    world 2 (mountain) 1
 *   FABBRICA.W01    world 3 (factory)  2
 *   VALLE.W01       world 4 (valley)   3
 *   CLASSIFICA.W01  high-score screen  -> menu state 6
 *   ESCI.W01        quit               -> menu state 9
 *
 * On-screen top-to-bottom the icons read world 1, 4, 2, 3.
 *
 * Frame states (PL-121): the exe picks frame 0/1 for an "unavailable" world
 * and 2/3 for an "available" one, +1 more on hover, and frame 4 while
 * pressed. RRR-12 found no progression state, so every world is available:
 * the port uses frame 2 rest / 3 hover / 4 pressed. CLASSIFICA and ESCI have
 * no hover or pressed art (their hit rect never changes); the exe's cosmetic
 * "select frame 1" on their click is not reproduced.
 *
 * The hit rectangle is always the CURRENT frame's origin + size, so a world's
 * clickable area grows when it shows its hover frame (0x409141..0x40919F).
 * Rectangles are half-open Win32 rects. CLASSIFICA is [523,535)-[653,585)
 * with NO extra handler offset (PL-122; corpus C04's implied offset is not in
 * the binary).
 */
#ifndef FA_MENU_H
#define FA_MENU_H

#ifdef __cplusplus
extern "C" {
#endif

struct fa_surface;

typedef struct fa_menu fa_menu;

typedef enum fa_menu_state {
    FA_MENU_REST  = 0,
    FA_MENU_HOVER = 1,
    FA_MENU_PRESS = 2
} fa_menu_state;

/* Load the screen from `gdata_dir` (the GData folder). Returns a heap object,
 * or NULL if the background or any sprite is missing. */
fa_menu *fa_menu_load(const char *gdata_dir);
void     fa_menu_free(fa_menu *m);

/* Number of buttons (4 worlds + scores + quit = 6). */
int fa_menu_button_count(const fa_menu *m);

/* Button `i`'s label: "world1".."world4", "scores", "quit". */
const char *fa_menu_label(const fa_menu *m, int i);

/* Set / get button `i`'s visual state. A button with no hover/pressed art
 * clamps to what it has. */
void fa_menu_set_state(fa_menu *m, int i, fa_menu_state st);
fa_menu_state fa_menu_get_state(const fa_menu *m, int i);

/* Button `i`'s hit rect for its CURRENT state (origin + size of the frame it
 * is showing). Returns 0/-1. */
int fa_menu_hit_rect(const fa_menu *m, int i, int *x, int *y, int *w, int *h);

/* The button whose current-state rect contains (px,py), or -1. Later buttons
 * win an overlap, matching the exe's iteration order. */
int fa_menu_pick(const fa_menu *m, int px, int py);

/* Draw the whole screen into `dst` (expected 800x600 RGB565). Returns 0/-1. */
int fa_menu_render(const fa_menu *m, const struct fa_surface *dst);

#ifdef __cplusplus
}
#endif

#endif /* FA_MENU_H */
