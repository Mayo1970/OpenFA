/*
 * fa_menu.c - the title / world-select screen (RRR-47). See fa_menu.h.
 */
#include "fa/fa_menu.h"
#include "fa/fa_surface.h"
#include "fa/fa_bmp.h"
#include "fa/fa_w01.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_BTN 6

/* Load / world / stage order (PL-122). Index i here is world i+1 for i<4. */
static const char *SPRITE_NAMES[N_BTN] = {
    "GIUNGLA", "MONTAGNA", "FABBRICA", "VALLE", "CLASSIFICA", "ESCI"
};
static const char *SPRITE_LABELS[N_BTN] = {
    "world1", "world2", "world3", "world4", "scores", "quit"
};

/* Frame index per visual state. Worlds: 2 rest / 3 hover / 4 pressed (PL-121,
 * RRR-12 = every world available). CLASSIFICA / ESCI: frame 0 for every
 * state. */
static const int WORLD_FRAME[3] = { 2, 3, 4 };

typedef struct {
    fa_surface px[3];       /* decoded frame per state; [k] valid if px!=NULL */
    int        x[3], y[3], w[3], h[3];
    int        nstates;     /* 3 for worlds, 1 for scores/quit */
    int        state;       /* current FA_MENU_* */
} menu_btn;

struct fa_menu {
    fa_surface bg;
    menu_btn   btn[N_BTN];
};

static int open_w01(fa_w01 *o, const char *dir, const char *name)
{
    static const char *fmt[] = {
        "%s/Animation/Interface/%s.W01",
        "%s/Animation/Interface/%s.w01",
    };
    char lower[64], path[600];
    size_t k = 0;
    for (; name[k] && k < sizeof lower - 1; k++) {
        char c = name[k];
        lower[k] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lower[k] = 0;
    for (unsigned v = 0; v < 2; v++) {
        snprintf(path, sizeof path, fmt[v], dir, name);
        if (fa_w01_open_file(o, path) == 0) return 0;
        snprintf(path, sizeof path, fmt[v], dir, lower);
        if (fa_w01_open_file(o, path) == 0) return 0;
    }
    return -1;
}

static int load_frame(menu_btn *b, int slot, fa_w01 *w, int frame)
{
    int cnt = fa_w01_count(w);
    if (frame >= cnt) frame = cnt - 1;
    if (frame < 0) frame = 0;

    int fw = 0, fh = 0, ox = 0, oy = 0;
    fa_w01_frame_size(w, frame, &fw, &fh);
    fa_w01_frame_origin(w, frame, &ox, &oy);
    if (fw <= 0 || fh <= 0) return -1;

    /* align 2 = a tight pitch: fa_w01_decode writes a tight w*h plane. */
    if (fa_surface_alloc(&b->px[slot], fw, fh, 2) != 0) return -1;
    fa_w01_decode(w, frame, b->px[slot].px);
    b->x[slot] = ox; b->y[slot] = oy;
    b->w[slot] = fw; b->h[slot] = fh;
    return 0;
}

fa_menu *fa_menu_load(const char *gdata_dir)
{
    if (!gdata_dir) return NULL;
    fa_menu *m = (fa_menu *)calloc(1, sizeof *m);
    if (!m) return NULL;

    char bgpath[600];
    const char *bgfmt[] = { "%s/Pics/StartBG.bmp", "%s/Pics/startbg.bmp" };
    int bg_ok = -1;
    for (unsigned v = 0; v < 2 && bg_ok != 0; v++) {
        snprintf(bgpath, sizeof bgpath, bgfmt[v], gdata_dir);
        bg_ok = fa_bmp_load_file(&m->bg, bgpath);
    }
    if (bg_ok != 0) { free(m); return NULL; }

    for (int i = 0; i < N_BTN; i++) {
        fa_w01 w;
        if (open_w01(&w, gdata_dir, SPRITE_NAMES[i]) != 0) {
            fa_menu_free(m);
            return NULL;
        }
        menu_btn *b = &m->btn[i];
        if (i < 4) {
            b->nstates = 3;
            for (int s = 0; s < 3; s++) {
                if (load_frame(b, s, &w, WORLD_FRAME[s]) != 0) {
                    fa_w01_close(&w);
                    fa_menu_free(m);
                    return NULL;
                }
            }
        } else {
            b->nstates = 1;
            if (load_frame(b, 0, &w, 0) != 0) {
                fa_w01_close(&w);
                fa_menu_free(m);
                return NULL;
            }
        }
        fa_w01_close(&w);
    }
    return m;
}

void fa_menu_free(fa_menu *m)
{
    if (!m) return;
    fa_surface_free(&m->bg);
    for (int i = 0; i < N_BTN; i++)
        for (int s = 0; s < 3; s++) fa_surface_free(&m->btn[i].px[s]);
    free(m);
}

int fa_menu_button_count(const fa_menu *m) { return m ? N_BTN : 0; }

const char *fa_menu_label(const fa_menu *m, int i)
{
    return (m && i >= 0 && i < N_BTN) ? SPRITE_LABELS[i] : NULL;
}

void fa_menu_set_state(fa_menu *m, int i, fa_menu_state st)
{
    if (!m || i < 0 || i >= N_BTN) return;
    int s = (int)st;
    if (s < 0) s = 0;
    if (s >= m->btn[i].nstates) s = m->btn[i].nstates - 1;
    m->btn[i].state = s;
}

fa_menu_state fa_menu_get_state(const fa_menu *m, int i)
{
    if (!m || i < 0 || i >= N_BTN) return FA_MENU_REST;
    return (fa_menu_state)m->btn[i].state;
}

int fa_menu_hit_rect(const fa_menu *m, int i, int *x, int *y, int *w, int *h)
{
    if (!m || i < 0 || i >= N_BTN) return -1;
    const menu_btn *b = &m->btn[i];
    int s = b->state;
    if (x) *x = b->x[s]; if (y) *y = b->y[s];
    if (w) *w = b->w[s]; if (h) *h = b->h[s];
    return 0;
}

int fa_menu_pick(const fa_menu *m, int px, int py)
{
    if (!m) return -1;
    int hit = -1;
    for (int i = 0; i < N_BTN; i++) {
        const menu_btn *b = &m->btn[i];
        int s = b->state;
        if (px >= b->x[s] && px < b->x[s] + b->w[s] &&
            py >= b->y[s] && py < b->y[s] + b->h[s])
            hit = i;                     /* later buttons win an overlap */
    }
    return hit;
}

int fa_menu_render(const fa_menu *m, const fa_surface *dst)
{
    if (!m || !dst || !dst->px) return -1;

    if (m->bg.px) fa_blit(dst, 0, 0, &m->bg, NULL, NULL);
    else          fa_fill(dst, NULL, NULL, fa_rgb565(0, 0, 0));

    for (int i = 0; i < N_BTN; i++) {
        const menu_btn *b = &m->btn[i];
        int s = b->state;
        if (b->px[s].px)
            fa_blit_keyed(dst, b->x[s], b->y[s], &b->px[s], NULL, NULL,
                          FA_COLORKEY);
    }
    return 0;
}
