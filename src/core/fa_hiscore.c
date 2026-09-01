/*
 * fa_hiscore.c - the high-score screen (RRR-47). See fa_hiscore.h.
 */
#include "fa/fa_hiscore.h"
#include "fa/fa_save.h"
#include "fa/fa_surface.h"
#include "fa/fa_bmp.h"
#include "fa/fa_w01.h"

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

struct fa_hiscore {
    fa_surface  bg;
    fa_surface *glyph;         /* decoded Schrift frames, [nglyph] */
    int         nglyph;
    fa_hs_file  world[4];
    int         cur;
};

/*
 * Best-effort Schrift.w01 map. The exact table at exe 0x401190 was not
 * extracted; this is the conventional sprite-font order and is the one thing
 * in this screen that still needs the disasm. Letters are case-folded (the
 * exe aliases lower-case to upper, PL-125). Space -> -1 (advance 5 px).
 */
int fa_hiscore_glyph_frame(char c)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return c - 'A';           /* 0..25  */
    if (c >= '0' && c <= '9') return 26 + (c - '0');    /* 26..35 */
    switch (c) {
        case '.': return 36;
        case ',': return 37;
        case '!': return 38;
        case '?': return 39;
        case '-': return 40;
        case ':': return 41;
        case '\'': return 42;
    }
    return -1;                                          /* space / unknown */
}

static int open_w01(fa_w01 *o, const char *dir, const char *rel_upper,
                    const char *rel_lower)
{
    char p[600];
    snprintf(p, sizeof p, "%s/%s", dir, rel_upper);
    if (fa_w01_open_file(o, p) == 0) return 0;
    snprintf(p, sizeof p, "%s/%s", dir, rel_lower);
    return fa_w01_open_file(o, p);
}

static int load_bmp(fa_surface *s, const char *dir, const char *a, const char *b)
{
    char p[600];
    snprintf(p, sizeof p, "%s/%s", dir, a);
    if (fa_bmp_load_file(s, p) == 0) return 0;
    snprintf(p, sizeof p, "%s/%s", dir, b);
    return fa_bmp_load_file(s, p);
}

static void load_world_table(fa_hs_file *hs, const char *dir, int w1)
{
    char p[600];
    for (int lc = 0; lc < 2; lc++) {
        snprintf(p, sizeof p, "%s/Save/%sighscore%d.dat",
                 dir, lc ? "h" : "H", w1);
        FILE *f = fopen(p, "rb");
        if (!f) continue;
        unsigned char buf[1024];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        if (fa_hs_parse(buf, n, hs) == 0) return;
    }
    fa_hs_default(hs);
}

fa_hiscore *fa_hiscore_load(const char *gdata_dir)
{
    if (!gdata_dir) return NULL;
    fa_hiscore *h = calloc(1, sizeof *h);
    if (!h) return NULL;

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
        for (int i = 0; i < h->nglyph; i++) {
            int gw = 0, gh = 0;
            fa_w01_frame_size(&sw, i, &gw, &gh);
            if (gw > 0 && gh > 0 &&
                fa_surface_alloc(&h->glyph[i], gw, gh, 2) == 0)
                fa_w01_decode(&sw, i, h->glyph[i].px);
        }
    }
    fa_w01_close(&sw);

    for (int w = 0; w < 4; w++) load_world_table(&h->world[w], gdata_dir, w + 1);
    h->cur = 0;
    return h;
}

void fa_hiscore_free(fa_hiscore *h)
{
    if (!h) return;
    fa_surface_free(&h->bg);
    for (int i = 0; i < h->nglyph; i++) fa_surface_free(&h->glyph[i]);
    free(h->glyph);
    free(h);
}

void fa_hiscore_set_world(fa_hiscore *h, int world0)
{
    if (h && world0 >= 0 && world0 < 4) h->cur = world0;
}
int fa_hiscore_world(const fa_hiscore *h) { return h ? h->cur : 0; }

/* Draw `text` with the top-left of the first glyph at (x, y). Returns the pen
 * X after the last glyph. */
static int draw_text(const fa_hiscore *h, const fa_surface *dst, int x, int y,
                     const char *text)
{
    for (const char *p = text; *p; p++) {
        int fr = fa_hiscore_glyph_frame(*p);
        if (fr < 0 || fr >= h->nglyph || !h->glyph[fr].px) { x += 5; continue; }
        const fa_surface *g = &h->glyph[fr];
        fa_blit_keyed(dst, x, y, g, NULL, NULL, FA_COLORKEY);
        x += g->w;
    }
    return x;
}

int fa_hiscore_render(const fa_hiscore *h, const fa_surface *dst)
{
    if (!h || !dst || !dst->px) return -1;

    if (h->bg.px) fa_blit(dst, 0, 0, &h->bg, NULL, NULL);
    else          fa_fill(dst, NULL, NULL, fa_rgb565(0, 0, 0));

    const fa_hs_file *t = &h->world[h->cur];
    for (int r = 0; r < ROWS; r++) {
        int y = ROW_Y0 + r * ROW_DY;
        char rank[8], score[16];
        snprintf(rank, sizeof rank, "%d.", r + 1);
        snprintf(score, sizeof score, "%lu", t->entry[r].score);

        draw_text(h, dst, X_RANK, y, rank);
        draw_text(h, dst, X_NAME, y, t->entry[r].name);
        int sx = X_SCORE_R - SCORE_ADV * (int)strlen(score);
        draw_text(h, dst, sx, y, score);
    }
    return 0;
}
