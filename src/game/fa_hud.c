/*
 * fa_hud.c - the in-level status display (RRR-51 AC5). See fa_hud.h.
 *
 * Reverse-engineered from JR_FERRERO.exe by hand off jr_disasm.txt:
 *   - the sheet loader at 0x4083f0..0x4086d0 (LoadW01 0x41e7eb into the
 *     0x4daa8c.. handle block; Energy set to frame 4, Schuss to frame 9)
 *   - hud_draw at 0x4089c0: sprintf("%d", 0x45ED2C); the Energy threshold
 *     ladder at 0x408a56 (0x50/0x3c/0x28/0x14); Schuss frame = 0x45ED34 - 1
 *     (SchussD when 0x4E1044 != 0); the 6 item flags at 0x45EFD4 (word each,
 *     > 0 -> frame 1); Actors frame = 0x45ED1C; every sprite blitted at its
 *     own frame origin ([handle+0x21], [handle+0x23]) via 0x41d615.
 */
#include "fa/fa_hud.h"
#include "fa/fa_surface.h"
#include "fa/fa_w01.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HUD_MAX_FRAMES 16

/* One decoded .W01 sheet: each frame as its own tight RGB565 surface plus the
 * table-A screen origin the exe positions it with. */
typedef struct {
    fa_surface frame[HUD_MAX_FRAMES];
    int        ox[HUD_MAX_FRAMES];
    int        oy[HUD_MAX_FRAMES];
    int        n;
} hud_sheet;

struct fa_hud {
    hud_sheet panel;      /* InterfaceItaly.w01 */
    hud_sheet energy;     /* Energy.w01, 5 frames */
    hud_sheet schuss;     /* Schuss.w01, 10 frames */
    hud_sheet schussd;    /* SchussD.w01, 10 frames */
    hud_sheet item[6];    /* Item1..6.w01, 2 frames each */
    hud_sheet actors;     /* Actors.w01, 2 frames */
    hud_sheet digit;      /* Schrift.w01, 10 frames 0..9 */

    /* RRR-59: the boss bar - swaps in for the 6 item icons in a boss arena */
    hud_sheet boss_frame; /* Boss/BossInterface.w01, 1 frame */
    hud_sheet boss_fill;  /* Boss/Energy.w01, 10 frames (one per HP)        */
    hud_sheet boss_pic;   /* Boss/Bosspics.w01, 4 frames (one per boss)     */
};

/* Open the first path that exists (the shipped tree mixes case). */
static int sheet_load(hud_sheet *s, const char *dir, const char *rel)
{
    memset(s, 0, sizeof *s);

    char lower[160], p[600];
    size_t i = 0;
    for (; rel[i] && i < sizeof lower - 1; i++) {
        char c = rel[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    lower[i] = 0;

    fa_w01 w;
    int opened = 0;
    snprintf(p, sizeof p, "%s/%s", dir, rel);
    if (fa_w01_open_file(&w, p) == 0) opened = 1;
    if (!opened) {
        snprintf(p, sizeof p, "%s/%s", dir, lower);
        if (fa_w01_open_file(&w, p) == 0) opened = 1;
    }
    if (!opened) return -1;

    int n = fa_w01_count(&w);
    if (n > HUD_MAX_FRAMES) n = HUD_MAX_FRAMES;
    for (int f = 0; f < n; f++) {
        int fw = 0, fh = 0, x = 0, y = 0;
        if (fa_w01_frame_size(&w, f, &fw, &fh) != 0 || fw <= 0 || fh <= 0)
            continue;
        if (fa_surface_alloc(&s->frame[f], fw, fh, 2) != 0) continue;
        if (fa_w01_decode(&w, f, s->frame[f].px) != 0) {
            fa_surface_free(&s->frame[f]);
            continue;
        }
        fa_w01_frame_origin(&w, f, &x, &y);
        s->ox[f] = (int16_t)x;      /* table-A words are signed */
        s->oy[f] = (int16_t)y;
        s->n = f + 1;
    }
    fa_w01_close(&w);
    return s->n > 0 ? 0 : -1;
}

static void sheet_free(hud_sheet *s)
{
    for (int i = 0; i < HUD_MAX_FRAMES; i++) fa_surface_free(&s->frame[i]);
    s->n = 0;
}

/* Blit sheet frame `f` at its own origin. */
static void sheet_draw(const hud_sheet *s, const struct fa_surface *dst, int f)
{
    if (f < 0 || f >= s->n || !s->frame[f].px) return;
    fa_blit_keyed(dst, s->ox[f], s->oy[f], &s->frame[f], NULL, NULL,
                  FA_COLORKEY);
}

fa_hud *fa_hud_load(const char *gdata_dir)
{
    if (!gdata_dir) return NULL;
    fa_hud *h = calloc(1, sizeof *h);
    if (!h) return NULL;

    const char *base = "Animation/Interface/Ingame";
    char rel[160];

    snprintf(rel, sizeof rel, "%s/Energy.w01", base);
    if (sheet_load(&h->energy, gdata_dir, rel) != 0) { free(h); return NULL; }

    snprintf(rel, sizeof rel, "%s/InterfaceItaly.w01", base);
    sheet_load(&h->panel, gdata_dir, rel);
    snprintf(rel, sizeof rel, "%s/Schuss.w01", base);
    sheet_load(&h->schuss, gdata_dir, rel);
    snprintf(rel, sizeof rel, "%s/SchussD.w01", base);
    sheet_load(&h->schussd, gdata_dir, rel);
    snprintf(rel, sizeof rel, "%s/Actors.w01", base);
    sheet_load(&h->actors, gdata_dir, rel);
    snprintf(rel, sizeof rel, "%s/Schrift.w01", base);
    sheet_load(&h->digit, gdata_dir, rel);
    for (int i = 0; i < 6; i++) {
        snprintf(rel, sizeof rel, "%s/Item%d.w01", base, i + 1);
        sheet_load(&h->item[i], gdata_dir, rel);
    }
    /* RRR-59: the boss bar sheets (optional - absent in a non-boss install) */
    snprintf(rel, sizeof rel, "%s/Boss/BossInterface.w01", base);
    sheet_load(&h->boss_frame, gdata_dir, rel);
    snprintf(rel, sizeof rel, "%s/Boss/Energy.w01", base);
    sheet_load(&h->boss_fill, gdata_dir, rel);
    snprintf(rel, sizeof rel, "%s/Boss/Bosspics.w01", base);
    sheet_load(&h->boss_pic, gdata_dir, rel);
    return h;
}

void fa_hud_free(fa_hud *h)
{
    if (!h) return;
    sheet_free(&h->panel);
    sheet_free(&h->energy);
    sheet_free(&h->schuss);
    sheet_free(&h->schussd);
    sheet_free(&h->actors);
    sheet_free(&h->digit);
    for (int i = 0; i < 6; i++) sheet_free(&h->item[i]);
    sheet_free(&h->boss_frame);
    sheet_free(&h->boss_fill);
    sheet_free(&h->boss_pic);
    free(h);
}

/* health -> Energy frame, the exe ladder at 0x408a56 (> 80/60/40/20/0). */
static int energy_frame(int health)
{
    if (health > 80) return 4;
    if (health > 60) return 3;
    if (health > 40) return 2;
    if (health > 20) return 1;
    if (health >  0) return 0;
    return -1;                        /* dead: no bar */
}

void fa_hud_render(const fa_hud *h, const struct fa_surface *dst,
                   int score, int health, int ammo, int dirty,
                   const int items[6], int character,
                   int boss_hp, int boss_pic)
{
    if (!h || !dst || !dst->px) return;

    /* 1. the framed panel (colour-keyed, mostly transparent) */
    sheet_draw(&h->panel, dst, 0);

    /* 2. the health bar */
    sheet_draw(&h->energy, dst, energy_frame(health));

    /* 3. the snowball gauge - SchussD after a collect_dirtyballs */
    if (ammo > 0) {
        const hud_sheet *g = (dirty && h->schussd.n) ? &h->schussd : &h->schuss;
        int f = ammo - 1;
        if (f > g->n - 1) f = g->n - 1;
        sheet_draw(g, dst, f);
    }

    /* 4. the 6 recipe pieces - OR the boss bar in a boss arena (exe 0x408B9B:
     * cmp 0x45ECBC,0 / jle -> the 6 icons; else BossInterface + Boss/Energy
     * frame [0x45ED24] + Bosspics, each at its own origin). Mutually
     * exclusive - the boss bar sits where the icons would. */
    if (boss_hp >= 0) {
        sheet_draw(&h->boss_frame, dst, 0);
        if (h->boss_fill.n) {
            int f = boss_hp;
            if (f > h->boss_fill.n - 1) f = h->boss_fill.n - 1;
            sheet_draw(&h->boss_fill, dst, f);
        }
        if (h->boss_pic.n) {
            int f = boss_pic < 0 ? 0 : boss_pic;
            if (f > h->boss_pic.n - 1) f = h->boss_pic.n - 1;
            sheet_draw(&h->boss_pic, dst, f);
        }
    } else {
        for (int i = 0; i < 6; i++) {
            const hud_sheet *it = &h->item[i];
            if (!it->n) continue;
            int f = (items && items[i]) ? 1 : 0;
            if (f > it->n - 1) f = it->n - 1;
            sheet_draw(it, dst, f);
        }
    }

    /* 5. the active-kid portrait */
    if (h->actors.n)
        sheet_draw(&h->actors, dst, (character & 1) ? 1 : 0);

    /* 6. the score - Schrift digits (frame == digit), left-anchored at the
     * "PUNTI" label. The exe draws the comic16f.ddf string from x=0x1B8 (440),
     * y=7 growing right (hud_draw 0x408b85). */
    if (h->digit.n >= 10) {
        char buf[16];
        snprintf(buf, sizeof buf, "%d", score < 0 ? 0 : score);
        int x = 440, y = 7;   /* exe push 0x1B8, push 0x7 (hud_draw 0x408b85) */
        for (int i = 0; buf[i]; i++) {
            int d = buf[i] - '0';
            if (d < 0 || d >= h->digit.n || !h->digit.frame[d].px) { x += 12; continue; }
            fa_blit_keyed(dst, x, y, &h->digit.frame[d], NULL, NULL, FA_COLORKEY);
            x += h->digit.frame[d].w + 2;
        }
    }
}
