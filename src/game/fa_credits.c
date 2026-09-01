/*
 * fa_credits.c - the credits sequence (RRR-54). See fa_credits.h.
 */
#include "fa/fa_credits.h"
#include "fa/fa_surface.h"
#include "fa/fa_bmp.h"
#include "fa/fa_w01.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PAGES 8            /* the exe advances credit1..4; keep headroom */

struct fa_credits {
    fa_surface pages[MAX_PAGES];
    int        npages;

    fa_surface *et;            /* decoded ENDTITLES frames, [n_et] */
    int        *et_x, *et_y;   /* per-frame table-A origin */
    int         n_et;

    fa_credits_phase phase;
    int        page;           /* current BMP page, 0-based */
    int        tick;           /* ticks in the current page / ET frame */
    int        et_frame;
    uint32_t   hash;
};

static void roll(uint32_t *h, uint32_t v)
{
    *h ^= v;
    *h *= 0x01000193u;
}

/* Try "<dir>/pics/CreditN.bmp" in the case variants the tree uses. */
static int load_page(fa_surface *s, const char *dir, int n)
{
    static const char *fmt[] = {
        "%s/Pics/Credit%d.bmp", "%s/pics/Credit%d.bmp",
        "%s/Pics/credit%d.bmp", "%s/pics/credit%d.bmp",
    };
    char p[600];
    for (unsigned v = 0; v < sizeof fmt / sizeof *fmt; v++) {
        snprintf(p, sizeof p, fmt[v], dir, n);
        if (fa_bmp_load_file(s, p) == 0) return 0;
    }
    return -1;
}

static int open_endtitles(fa_w01 *o, const char *dir)
{
    static const char *fmt[] = {
        "%s/Animation/ENDTITLES.W01", "%s/Animation/EndTitles.w01",
        "%s/Animation/endtitles.w01",
    };
    char p[600];
    for (unsigned v = 0; v < sizeof fmt / sizeof *fmt; v++) {
        snprintf(p, sizeof p, fmt[v], dir);
        if (fa_w01_open_file(o, p) == 0) return 0;
    }
    return -1;
}

fa_credits *fa_credits_load(const char *gdata_dir)
{
    if (!gdata_dir) return NULL;
    fa_credits *c = calloc(1, sizeof *c);
    if (!c) return NULL;

    for (int i = 0; i < MAX_PAGES; i++) {
        if (load_page(&c->pages[c->npages], gdata_dir, i + 1) != 0) break;
        c->npages++;
    }
    if (c->npages == 0) { free(c); return NULL; }

    fa_w01 w;
    if (open_endtitles(&w, gdata_dir) == 0) {
        c->n_et = fa_w01_count(&w);
        if (c->n_et > 0) {
            c->et   = calloc((size_t)c->n_et, sizeof *c->et);
            c->et_x = calloc((size_t)c->n_et, sizeof *c->et_x);
            c->et_y = calloc((size_t)c->n_et, sizeof *c->et_y);
        }
        if (c->et && c->et_x && c->et_y) {
            for (int i = 0; i < c->n_et; i++) {
                int fw = 0, fh = 0, ox = 0, oy = 0;
                fa_w01_frame_size(&w, i, &fw, &fh);
                fa_w01_frame_origin(&w, i, &ox, &oy);
                c->et_x[i] = ox;
                c->et_y[i] = oy;
                if (fw > 0 && fh > 0 &&
                    fa_surface_alloc(&c->et[i], fw, fh, 2) == 0)
                    fa_w01_decode(&w, i, c->et[i].px);
            }
        } else {
            c->n_et = 0;
        }
        fa_w01_close(&w);
    }

    fa_credits_begin(c);
    return c;
}

void fa_credits_free(fa_credits *c)
{
    if (!c) return;
    for (int i = 0; i < c->npages; i++) fa_surface_free(&c->pages[i]);
    for (int i = 0; i < c->n_et; i++) fa_surface_free(&c->et[i]);
    free(c->et); free(c->et_x); free(c->et_y);
    free(c);
}

void fa_credits_begin(fa_credits *c)
{
    if (!c) return;
    c->phase = FA_CREDITS_PAGE;
    c->page = 0;
    c->tick = 0;
    c->et_frame = 0;
    c->hash = 0x811c9dc5u;
}

static void to_endtitles(fa_credits *c)
{
    if (c->n_et > 0) {
        c->phase = FA_CREDITS_ENDTITLES;
        c->et_frame = 0;
        c->tick = 0;
    } else {
        c->phase = FA_CREDITS_DONE;
    }
}

void fa_credits_tick(fa_credits *c, int skip)
{
    if (!c || c->phase == FA_CREDITS_DONE) return;

    c->tick++;

    if (c->phase == FA_CREDITS_PAGE) {
        if (skip || c->tick >= FA_CREDITS_PAGE_DWELL) {
            c->page++;
            c->tick = 0;
            if (c->page >= c->npages) to_endtitles(c);
        }
    } else { /* FA_CREDITS_ENDTITLES */
        if (skip) { c->phase = FA_CREDITS_DONE; }
        else if (c->tick >= FA_CREDITS_ET_PERIOD) {
            c->tick = 0;
            c->et_frame++;
            if (c->et_frame >= c->n_et) c->phase = FA_CREDITS_DONE;
        }
    }

    roll(&c->hash, (uint32_t)c->phase);
    roll(&c->hash, (uint32_t)c->page);
    roll(&c->hash, (uint32_t)c->et_frame);
    roll(&c->hash, (uint32_t)c->tick);
}

fa_credits_phase fa_credits_phase_of(const fa_credits *c)
{
    return c ? c->phase : FA_CREDITS_DONE;
}
int fa_credits_page(const fa_credits *c) { return c ? c->page : 0; }
int fa_credits_page_count(const fa_credits *c) { return c ? c->npages : 0; }
int fa_credits_done(const fa_credits *c)
{
    return !c || c->phase == FA_CREDITS_DONE;
}
uint32_t fa_credits_hash(const fa_credits *c) { return c ? c->hash : 0; }

const char *fa_credits_phase_name(fa_credits_phase p)
{
    switch (p) {
        case FA_CREDITS_PAGE:      return "PAGE";
        case FA_CREDITS_ENDTITLES: return "ENDTITLES";
        case FA_CREDITS_DONE:      return "DONE";
    }
    return "?";
}

int fa_credits_render(const fa_credits *c, const fa_surface *dst)
{
    if (!c || !dst || !dst->px) return -1;

    if (c->phase == FA_CREDITS_PAGE) {
        int p = c->page;
        if (p < 0) p = 0;
        if (p >= c->npages) p = c->npages - 1;
        if (c->pages[p].px) fa_blit(dst, 0, 0, &c->pages[p], NULL, NULL);
        else                fa_fill(dst, NULL, NULL, fa_rgb565(0, 0, 0));
        return 0;
    }

    fa_fill(dst, NULL, NULL, fa_rgb565(0, 0, 0));
    if (c->phase == FA_CREDITS_ENDTITLES && c->n_et > 0) {
        int f = c->et_frame;
        if (f < 0) f = 0;
        if (f >= c->n_et) f = c->n_et - 1;
        if (c->et[f].px)
            fa_blit_keyed(dst, c->et_x[f], c->et_y[f], &c->et[f], NULL, NULL,
                          FA_COLORKEY);
    }
    return 0;
}
