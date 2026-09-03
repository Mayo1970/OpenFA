/*
 * fa_hiscore.c - the CLASSIFICA / high-score screen. See fa_hiscore.h.
 */
#include "fa/fa_hiscore.h"
#include "fa/fa_save.h"
#include "fa/fa_surface.h"
#include "fa/fa_bmp.h"
#include "fa/fa_w01.h"
#include "fa/fa_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROWS      FA_HS_ENTRIES
#define ROW_Y0    128
#define ROW_DY    32
#define X_RANK    180
#define X_NAME    230
#define X_SCORE_R 490          /* score right edge */
#define SCORE_ADV 13           /* px per score glyph (0x4038E0) */
#define EDIT_MAX  28           /* exe caps name entry at 0x1C (0x4094A7) */

#define TAB_REST  2            /* Schrift-less world tab: dim frame */
#define TAB_LIT   4            /* shown / hovered: lit frame */

enum { HS_VIEW = 0, HS_NAME = 1 };

/* The four world tab sprites, left column. Order = world 1..4. */
static const char *const TAB_UPPER[4] = {
    "Animation/Interface/Giungla.w01",
    "Animation/Interface/Montagna.w01",
    "Animation/Interface/Fabbrica1.w01",
    "Animation/Interface/valle.w01",
};
static const char *const TAB_LOWER[4] = {
    "Animation/Interface/giungla.w01",
    "Animation/Interface/montagna.w01",
    "Animation/Interface/fabbrica1.w01",
    "Animation/Interface/valle.w01",
};

struct fa_hiscore {
    fa_surface  bg;
    fa_surface *glyph;         /* decoded Schrift frames, [nglyph] */
    int         nglyph;
    fa_hs_file  world[4];
    int         cur;

    fa_surface  boss[4];       /* Gegner.w01 frames, one per world (may be 0) */
    int         boss_ox[4];
    int         boss_oy[4];
    int         boss_nframes;
    int         boss_pic;      /* -1 = none; 0..3 = draw boss[boss_pic]        */

    fa_surface  tab[4][2];     /* [world][0=rest 1=lit]                        */
    int         tab_ox[4][2];
    int         tab_oy[4][2];
    int         tab_ok[4];

    fa_surface  back[2];       /* indietro.w01 frame 0 rest / 1 hover          */
    int         back_ox, back_oy;
    int         back_ok;

    int         state;
    int         place_rank;    /* row a run's score landed on, or -1           */
    char        editname[EDIT_MAX + 2];

    int         hover;         /* 0..3 = a tab, 4 = back, -1 = none            */

    fa_vfs      vfs;           /* user: = beside the game (RRR-39), asset: GData */
    int         vfs_ok;
    char        gdata[600];    /* asset-read fallback for the shipped tables    */
};

/*
 * Schrift.w01 glyph map, exe table 0x4551B4 (mode 0). Letters case-fold to
 * 0..25, digits to 26..35, then the punctuation run to 36..54. Space and any
 * other character return -1 (the caller advances the pen 5 px).
 */
int fa_hiscore_glyph_frame(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return c - 'A';            /* 0..25  */
    if (c >= '0' && c <= '9') return 26 + (c - '0');     /* 26..35 */
    switch (c) {
        case '!': return 36;
        case '$': return 37;
        case '%': return 38;
        case '(': return 39;
        case ')': return 40;
        case ',': return 41;
        case '.': return 42;
        case '-': return 43;
        case ';': return 44;
        case '_': return 45;
        case '+': return 46;
        case '=': return 47;
        case '"': return 48;
        case ':': return 49;
        case '?': return 50;
        case '>': return 51;
        case '<': return 52;
        case '/': return 53;
        case '*': return 54;
    }
    return -1;                                           /* space / unknown */
}

/* Vertical drop for a glyph, relative to the row Y (exe jump table 0x40140C).
 *   ",", ".", "_", "="  -> +12   (low punctuation)
 *   "-", ";", "+", ":", "*" -> +5
 *   everything else      -> 0 */
static int glyph_dy(int frame)
{
    switch (frame) {
        case 41: case 42: case 45: case 47: return 12;
        case 43: case 44: case 46: case 49: case 54: return 5;
        default: return 0;
    }
}

static int open_w01(fa_w01 *o, const char *dir, const char *rel_upper,
                    const char *rel_lower)
{
    char p[700];
    snprintf(p, sizeof p, "%s/%s", dir, rel_upper);
    if (fa_w01_open_file(o, p) == 0) return 0;
    snprintf(p, sizeof p, "%s/%s", dir, rel_lower);
    return fa_w01_open_file(o, p);
}

static int load_bmp(fa_surface *s, const char *dir, const char *a, const char *b)
{
    char p[700];
    snprintf(p, sizeof p, "%s/%s", dir, a);
    if (fa_bmp_load_file(s, p) == 0) return 0;
    snprintf(p, sizeof p, "%s/%s", dir, b);
    return fa_bmp_load_file(s, p);
}

/* Decode one frame of `w` into `dst`, and record its table-A origin. */
static int decode_frame(const fa_w01 *w, int i, fa_surface *dst,
                        int *ox, int *oy)
{
    int fw = 0, fh = 0, x = 0, y = 0;
    if (i < 0 || i >= fa_w01_count(w)) return -1;
    if (fa_w01_frame_size(w, i, &fw, &fh) != 0 || fw <= 0 || fh <= 0) return -1;
    if (fa_surface_alloc(dst, fw, fh, 2) != 0) return -1;
    if (fa_w01_decode(w, i, dst->px) != 0) { fa_surface_free(dst); return -1; }
    fa_w01_frame_origin(w, i, &x, &y);
    if (ox) *ox = (int16_t)x;
    if (oy) *oy = (int16_t)y;
    return 0;
}

/* user:Highscore{n}.dat (RRR-39, beside the game - what this screen writes)
 * then the shipped GData\Save\ copy, else the built-in default. */
static void load_world_table(fa_hiscore *h, fa_hs_file *hs, int w1)
{
    unsigned char buf[1024];

    if (h->vfs_ok) {
        char vp[64];
        void *fb = NULL;
        size_t fn = 0;
        snprintf(vp, sizeof vp, "user:Highscore%d.dat", w1);
        if (fa_vfs_read_all(&h->vfs, vp, &fb, &fn) == 0) {
            int ok = (fa_hs_parse(fb, fn, hs) == 0);
            free(fb);
            if (ok) return;
        }
    }
    for (int lc = 0; lc < 2; lc++) {
        char p[700];
        snprintf(p, sizeof p, "%s/Save/%sighscore%d.dat",
                 h->gdata, lc ? "h" : "H", w1);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        if (fa_hs_parse(buf, n, hs) == 0) return;
    }
    fa_hs_default(hs);
}

/* Persist world `w` (0..3) to user:Highscore{w+1}.dat. Silent on failure - a
 * read-only install just keeps the run in memory for this session. */
static void write_world_table(fa_hiscore *h, int w)
{
    if (!h->vfs_ok || w < 0 || w >= 4) return;
    unsigned char buf[1024];
    int n = fa_hs_write(&h->world[w], buf, sizeof buf);
    if (n <= 0) return;
    char vp[64];
    snprintf(vp, sizeof vp, "user:Highscore%d.dat", w + 1);
    fa_vfs_write_all(&h->vfs, vp, buf, (size_t)n);
}

fa_hiscore *fa_hiscore_load(const char *gdata_dir)
{
    if (!gdata_dir) return NULL;
    fa_hiscore *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->boss_pic = -1;
    h->hover = -1;
    h->place_rank = -1;
    snprintf(h->gdata, sizeof h->gdata, "%s", gdata_dir);
    h->vfs_ok = (fa_vfs_init_default(&h->vfs, gdata_dir, "FreshAdventures") == 0);

    if (load_bmp(&h->bg, gdata_dir, "Pics/HighscoreBG.bmp",
                 "Pics/highscorebg.bmp") != 0) { free(h); return NULL; }

    fa_w01 sw;
    if (open_w01(&sw, gdata_dir, "Animation/Interface/Schrift.w01",
                 "Animation/Interface/schrift.w01") != 0) {
        fa_surface_free(&h->bg);
        free(h);
        return NULL;
    }
    h->nglyph = fa_w01_count(&sw);
    h->glyph = calloc((size_t)(h->nglyph > 0 ? h->nglyph : 1), sizeof *h->glyph);
    if (h->glyph) {
        for (int i = 0; i < h->nglyph; i++)
            decode_frame(&sw, i, &h->glyph[i], NULL, NULL);
    }
    fa_w01_close(&sw);

    /* The boss portraits (Gegner.w01). Optional. */
    fa_w01 gw;
    if (open_w01(&gw, gdata_dir, "Animation/Interface/Gegner.w01",
                 "Animation/Interface/gegner.w01") == 0) {
        int gn = fa_w01_count(&gw);
        if (gn > 4) gn = 4;
        for (int i = 0; i < gn; i++) {
            if (decode_frame(&gw, i, &h->boss[i], &h->boss_ox[i],
                             &h->boss_oy[i]) == 0)
                h->boss_nframes = i + 1;
        }
        fa_w01_close(&gw);
    }

    /* The four world tab buttons: frame 2 (rest) + frame 4 (lit). Optional. */
    for (int w = 0; w < 4; w++) {
        fa_w01 tw;
        if (open_w01(&tw, gdata_dir, TAB_UPPER[w], TAB_LOWER[w]) != 0) continue;
        int f_rest = fa_w01_count(&tw) > TAB_REST ? TAB_REST : 0;
        int f_lit  = fa_w01_count(&tw) > TAB_LIT  ? TAB_LIT  : f_rest;
        int a = decode_frame(&tw, f_rest, &h->tab[w][0],
                             &h->tab_ox[w][0], &h->tab_oy[w][0]);
        int b = decode_frame(&tw, f_lit,  &h->tab[w][1],
                             &h->tab_ox[w][1], &h->tab_oy[w][1]);
        h->tab_ok[w] = (a == 0 && b == 0);
        fa_w01_close(&tw);
    }

    /* The BACK button (indietro.w01). Optional. */
    fa_w01 bw;
    if (open_w01(&bw, gdata_dir, "Animation/Interface/indietro.w01",
                 "Animation/Interface/indietro.w01") == 0) {
        int n = fa_w01_count(&bw);
        int a = decode_frame(&bw, 0, &h->back[0], &h->back_ox, &h->back_oy);
        int b = decode_frame(&bw, n > 1 ? 1 : 0, &h->back[1], NULL, NULL);
        h->back_ok = (a == 0 && b == 0);
        fa_w01_close(&bw);
    }

    for (int w = 0; w < 4; w++)
        load_world_table(h, &h->world[w], w + 1);
    h->cur = 0;
    return h;
}

void fa_hiscore_free(fa_hiscore *h)
{
    if (!h) return;
    fa_surface_free(&h->bg);
    for (int i = 0; i < h->nglyph; i++) fa_surface_free(&h->glyph[i]);
    for (int i = 0; i < 4; i++) {
        fa_surface_free(&h->boss[i]);
        fa_surface_free(&h->tab[i][0]);
        fa_surface_free(&h->tab[i][1]);
    }
    fa_surface_free(&h->back[0]);
    fa_surface_free(&h->back[1]);
    free(h->glyph);
    free(h);
}

void fa_hiscore_set_world(fa_hiscore *h, int world0)
{
    if (h && world0 >= 0 && world0 < 4) h->cur = world0;
}
int fa_hiscore_world(const fa_hiscore *h) { return h ? h->cur : 0; }

void fa_hiscore_set_boss_pic(fa_hiscore *h, int world0)
{
    if (h) h->boss_pic = (world0 >= 0 && world0 < 4) ? world0 : -1;
}

void fa_hiscore_begin(fa_hiscore *h, int world0, long run_score, int show_boss)
{
    if (!h) return;
    fa_hiscore_set_world(h, world0);
    fa_hiscore_set_boss_pic(h, show_boss ? h->cur : -1);
    h->state = HS_VIEW;
    h->place_rank = -1;
    h->editname[0] = 0;
    h->hover = -1;

    if (run_score > 0) {
        int r = fa_hs_insert(&h->world[h->cur], "", (unsigned long)run_score);
        if (r >= 0) {
            h->place_rank = r;
            h->state = HS_NAME;
        }
    }
}

int fa_hiscore_world_bytes(const fa_hiscore *h, int world0,
                           unsigned char *buf, unsigned long cap)
{
    if (!h || world0 < 0 || world0 >= 4 || !buf) return -1;
    return fa_hs_write(&h->world[world0], buf, (size_t)cap);
}

/* --- hit tests --------------------------------------------------------- */

static int pt_in(int px, int py, int x, int y, int w, int hh)
{
    return px >= x && px < x + w && py >= y && py < y + hh;
}

/* Which element (0..3 = tab, 4 = back) is under (px,py), or -1. */
static int pick(const fa_hiscore *h, int px, int py)
{
    for (int w = 0; w < 4; w++) {
        if (!h->tab_ok[w]) continue;
        int lit = (w == h->cur);
        const fa_surface *s = &h->tab[w][lit];
        if (pt_in(px, py, h->tab_ox[w][lit], h->tab_oy[w][lit], s->w, s->h))
            return w;
    }
    if (h->back_ok &&
        pt_in(px, py, h->back_ox, h->back_oy, h->back[0].w, h->back[0].h))
        return 4;
    return -1;
}

void fa_hiscore_tick(fa_hiscore *h, const fa_hiscore_in *in, fa_hiscore_ev *out)
{
    fa_hiscore_ev ev;
    memset(&ev, 0, sizeof ev);
    if (!h || !in) { if (out) *out = ev; return; }

    if (h->state == HS_NAME) {
        size_t n = strlen(h->editname);
        for (int i = 0; in->text && i < in->text_n; i++) {
            char c = in->text[i];
            if (c >= ' ' && c < 127 && n < EDIT_MAX) h->editname[n++] = c;
        }
        h->editname[n] = 0;
        if (in->backspace && n > 0) h->editname[--n] = 0;

        if (in->enter || in->escape) {
            if (h->place_rank >= 0 && h->place_rank < ROWS) {
                fa_hs_entry *e = &h->world[h->cur].entry[h->place_rank];
                memset(e->name, 0, sizeof e->name);
                strncpy(e->name, h->editname, FA_HS_NAME_MAX);
                e->name[FA_HS_NAME_MAX] = 0;
                write_world_table(h, h->cur);
                ev.wrote = 1;
                ev.wrote_world = h->cur;
            }
            h->place_rank = -1;
            h->state = HS_VIEW;
            /* confirm the name AND leave to the menu - a console port has no
             * pointer to click the BACK button or a world tab from here */
            ev.leave = 1;
        }
        if (out) *out = ev;
        return;
    }

    /* HS_VIEW */
    int hv = pick(h, in->ptr_x, in->ptr_y);
    if (hv >= 0 && hv != h->hover) ev.hover_sound = 1;
    h->hover = hv;

    if (in->click) {
        if (hv >= 0 && hv < 4)      h->cur = hv;
        else if (hv == 4)           ev.leave = 1;
    }
    if (in->enter || in->escape) ev.leave = 1;

    if (out) *out = ev;
}

/* --- render ---------------------------------------------------------- */

/* Draw `text` with the first glyph's cell top-left at (x, y). Returns the pen
 * X after the last glyph. */
static int draw_text(const fa_hiscore *h, const fa_surface *dst, int x, int y,
                     const char *text)
{
    for (const char *p = text; *p; p++) {
        int fr = fa_hiscore_glyph_frame(*p);
        if (fr < 0 || fr >= h->nglyph || !h->glyph[fr].px) { x += 5; continue; }
        const fa_surface *g = &h->glyph[fr];
        fa_blit_keyed(dst, x, y + glyph_dy(fr), g, NULL, NULL, FA_COLORKEY);
        x += g->w;
    }
    return x;
}

int fa_hiscore_render(const fa_hiscore *h, const fa_surface *dst)
{
    if (!h || !dst || !dst->px) return -1;

    if (h->bg.px) fa_blit(dst, 0, 0, &h->bg, NULL, NULL);
    else          fa_fill(dst, NULL, NULL, fa_rgb565(0, 0, 0));

    /* boss portrait, under the rows (exe draws Gegner before the text) */
    if (h->boss_pic >= 0 && h->boss_pic < h->boss_nframes &&
        h->boss[h->boss_pic].px) {
        int p = h->boss_pic;
        fa_blit_keyed(dst, h->boss_ox[p], h->boss_oy[p], &h->boss[p],
                      NULL, NULL, FA_COLORKEY);
    }

    /* the four world tabs: lit for the shown world or the hovered one */
    for (int w = 0; w < 4; w++) {
        if (!h->tab_ok[w]) continue;
        int lit = (w == h->cur) || (h->hover == w);
        fa_blit_keyed(dst, h->tab_ox[w][lit], h->tab_oy[w][lit],
                      &h->tab[w][lit], NULL, NULL, FA_COLORKEY);
    }

    /* the BACK button */
    if (h->back_ok) {
        int f = (h->hover == 4) ? 1 : 0;
        fa_blit_keyed(dst, h->back_ox, h->back_oy, &h->back[f],
                      NULL, NULL, FA_COLORKEY);
    }

    const fa_hs_file *t = &h->world[h->cur];
    for (int r = 0; r < ROWS; r++) {
        int y = ROW_Y0 + r * ROW_DY;
        char rank[8], score[16];
        snprintf(rank, sizeof rank, "%d.", r + 1);
        snprintf(score, sizeof score, "%lu", t->entry[r].score);

        draw_text(h, dst, X_RANK, y, rank);

        if (h->state == HS_NAME && r == h->place_rank) {
            char shown[EDIT_MAX + 2];
            snprintf(shown, sizeof shown, "%s_", h->editname);
            draw_text(h, dst, X_NAME, y, shown);
        } else {
            draw_text(h, dst, X_NAME, y, t->entry[r].name);
        }

        int sx = X_SCORE_R - SCORE_ADV * (int)strlen(score);
        draw_text(h, dst, sx, y, score);
    }
    return 0;
}
