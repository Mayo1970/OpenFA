/*
 * fa_charspr.c - character sprite sheet + animation playback. See fa_charspr.h.
 */
#include "fa/fa_charspr.h"
#include "fa/fa_surface.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *fa_cs_pose_name(fa_cs_pose p)
{
    switch (p) {
    case FA_CS_STAND:       return "stand";
    case FA_CS_WALK:        return "walk";
    case FA_CS_CROUCH:      return "crouch";
    case FA_CS_CROUCH_RISE: return "crouch-rise";
    case FA_CS_JUMP_RISE:   return "jump-rise";
    case FA_CS_JUMP_FALL:  return "jump-fall";
    case FA_CS_GLIDE:      return "glide";
    case FA_CS_THROW_FWD:  return "throw-fwd";
    case FA_CS_THROW_UP:   return "throw-up";
    case FA_CS_KO:         return "ko";
    case FA_CS_IDLE_A:     return "idle-a";
    case FA_CS_IDLE_B:     return "idle-b";
    case FA_CS_CLIMB:      return "climb";
    case FA_CS_PUSH:       return "push";
    case FA_CS_SWAP:       return "swap";
    case FA_CS_SWAP_END:   return "swap-end";
    default:               return "?";
    }
}

/* --- sidecar parser ---------------------------------------------------- */

/* Skip to the next line. Returns a pointer at or past `end`. */
static const char *next_line(const char *p, const char *end)
{
    while (p < end && *p != '\n') p++;
    return p < end ? p + 1 : end;
}

/* Read a non-negative decimal at *pp (skipping leading blanks). Returns 1 and
 * advances *pp on success, 0 otherwise. */
static int read_uint(const char **pp, const char *end, int *out)
{
    const char *p = *pp;
    while (p < end && (*p == ' ' || *p == '\t')) p++;
    if (p >= end || !isdigit((unsigned char)*p)) return 0;
    long v = 0;
    while (p < end && isdigit((unsigned char)*p)) {
        v = v * 10 + (*p - '0');
        if (v > 1000000) v = 1000000;
        p++;
    }
    *out = (int)v;
    *pp = p;
    return 1;
}

/* Find "A - B" starting at or after `p` (within the line). On success writes
 * a,b and returns a pointer just past B; returns NULL otherwise. */
static const char *read_range(const char *p, const char *end, int *a, int *b)
{
    for (; p < end; p++) {
        if (!isdigit((unsigned char)*p)) continue;
        const char *q = p;
        if (!read_uint(&q, end, a)) continue;
        const char *r = q;
        while (r < end && (*r == ' ' || *r == '\t')) r++;
        if (r >= end || *r != '-') { p = q - 1; continue; }
        r++;
        if (!read_uint(&r, end, b)) { p = q - 1; continue; }
        return r;
    }
    return NULL;
}

/* Case-insensitive search for "anim" within [p,end). */
static const char *find_anim(const char *p, const char *end)
{
    for (; p + 4 <= end; p++) {
        if ((p[0] == 'a' || p[0] == 'A') && (p[1] == 'n' || p[1] == 'N') &&
            (p[2] == 'i' || p[2] == 'I') && (p[3] == 'm' || p[3] == 'M'))
            return p + 4;
    }
    return NULL;
}

int fa_cs_parse_sidecar(fa_cs_clip *out, int cap, const char *text, size_t len)
{
    if (!out || cap <= 0 || !text) return -1;
    const char *p = text, *end = text + len;
    int n = 0;

    while (p < end && n < cap) {
        const char *ls = p;
        const char *le = next_line(p, end);
        const char *lend = le;
        if (lend > ls && lend[-1] == '\n') lend--;
        if (lend > ls && lend[-1] == '\r') lend--;
        p = le;

        /* a data line starts with a code token: letters/digits, then blank */
        const char *c = ls;
        while (c < lend && (*c == ' ' || *c == '\t')) c++;
        const char *cs = c;
        while (c < lend && (isalnum((unsigned char)*c) || *c == '_')) c++;
        size_t clen = (size_t)(c - cs);
        if (clen == 0 || clen >= FA_CS_CODE_MAX) continue;
        if (c >= lend || (*c != ' ' && *c != '\t')) continue;
        /* the code must contain a letter (rejects "50: Milchpartikel") */
        int has_alpha = 0;
        for (const char *k = cs; k < c; k++)
            if (isalpha((unsigned char)*k)) { has_alpha = 1; break; }
        if (!has_alpha) continue;

        int a, b;
        const char *after = read_range(c, lend, &a, &b);
        if (!after) continue;
        if (b < a) continue;

        fa_cs_clip *cl = &out[n];
        memcpy(cl->code, cs, clen);
        cl->code[clen] = 0;
        cl->first = a;
        cl->last = b;
        cl->loop_first = a;
        cl->loop_last = b;
        cl->loop = 1;

        const char *ap = find_anim(after, lend);
        if (ap) {
            int la, lb;
            if (read_range(ap, lend, &la, &lb) && lb >= la) {
                cl->loop_first = la;
                cl->loop_last = lb;
            }
        }
        n++;
    }
    return n;
}

/* --- sheet ----------------------------------------------------------- */

static void upper_ascii(char *s) { for (; *s; s++) *s = (char)toupper((unsigned char)*s); }
static void lower_ascii(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }

/* Open a file, tolerating the shipped case mix on the basename. */
static int open_case_tolerant(fa_w01 *w, const char *path)
{
    if (fa_w01_open_file(w, path) == 0) return 0;

    char buf[600];
    size_t n = strlen(path);
    if (n >= sizeof buf) return -1;
    memcpy(buf, path, n + 1);
    char *slash = strrchr(buf, '/');
    char *bslash = strrchr(buf, '\\');
    char *base = slash > bslash ? slash : bslash;
    base = base ? base + 1 : buf;

    upper_ascii(base);
    if (fa_w01_open_file(w, buf) == 0) return 0;
    lower_ascii(base);
    if (fa_w01_open_file(w, buf) == 0) return 0;
    return -1;
}

/* Read a whole file into a malloc'd buffer. Returns bytes, or -1. */
static long slurp(const char *path, char **out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);
    char *b = (char *)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return -1; }
    size_t got = fread(b, 1, (size_t)sz, f);
    fclose(f);
    b[got] = 0;
    *out = b;
    return (long)got;
}

int fa_cs_sheet_open(fa_cs_sheet *s, const char *w01_path,
                     const char *sidecar_path, int base_facing)
{
    if (!s || !w01_path) return -1;
    memset(s, 0, sizeof *s);
    s->base_facing = base_facing < 0 ? -1 : 1;

    if (open_case_tolerant(&s->w01, w01_path) != 0) return -1;
    s->owns_w01 = 1;

    if (sidecar_path && *sidecar_path) {
        char *txt = NULL;
        long len = slurp(sidecar_path, &txt);
        if (len >= 0) {
            int n = fa_cs_parse_sidecar(s->clips, FA_CS_MAX_CLIPS, txt,
                                        (size_t)len);
            if (n > 0) s->clip_count = n;
            free(txt);
        }
    }
    return 0;
}

void fa_cs_sheet_close(fa_cs_sheet *s)
{
    if (!s) return;
    if (s->owns_w01) fa_w01_close(&s->w01);
    memset(s, 0, sizeof *s);
}

int fa_cs_sheet_add_clip(fa_cs_sheet *s, const char *code,
                         int first, int last, int loop_first, int loop_last)
{
    if (!s || !code || !*code || first < 0 || last < first) return -1;
    if (strlen(code) >= FA_CS_CODE_MAX) return -1;
    if (loop_first < 0 || loop_last < 0) { loop_first = first; loop_last = last; }
    if (loop_last < loop_first) return -1;

    int idx = fa_cs_sheet_find(s, code);
    if (idx < 0) {
        if (s->clip_count >= FA_CS_MAX_CLIPS) return -1;
        idx = s->clip_count++;
    }
    fa_cs_clip *cl = &s->clips[idx];
    snprintf(cl->code, sizeof cl->code, "%s", code);
    cl->first = first; cl->last = last;
    cl->loop_first = loop_first; cl->loop_last = loop_last;
    cl->loop = 1;
    return idx;
}

int fa_cs_sheet_find(const fa_cs_sheet *s, const char *code)
{
    if (!s || !code) return -1;
    for (int i = 0; i < s->clip_count; i++)
        if (strcmp(s->clips[i].code, code) == 0) return i;
    return -1;
}

int fa_cs_sheet_frame_count(const fa_cs_sheet *s)
{
    return s ? fa_w01_count(&s->w01) : 0;
}

/* --- animation instance -------------------------------------------- */

void fa_cs_anim_init(fa_cs_anim *a, const fa_cs_sheet *s)
{
    if (!a) return;
    memset(a, 0, sizeof *a);
    a->sheet = s;
    a->frame = -1;
    a->period = 1;
    a->facing = s ? s->base_facing : 1;
    a->cached_frame = -1;
}

void fa_cs_anim_free(fa_cs_anim *a)
{
    if (!a) return;
    free(a->cache);
    a->cache = NULL;
    a->cached_frame = -1;
}

int fa_cs_anim_bind_frames(fa_cs_anim *a, fa_cs_pose pose,
                           int current, int loop_first, int last, int loop)
{
    if (!a || pose < 0 || pose >= FA_CS_POSE_COUNT) return -1;
    if (current < 0 || last < current) return -1;
    if (loop_first < current || loop_first > last) loop_first = current;

    fa_cs_clip *cl = &a->pose_clip[pose];
    cl->code[0] = 0;
    cl->first = current;
    cl->last = last;
    cl->loop_first = loop_first;
    cl->loop_last = last;
    cl->loop = loop ? 1 : 0;
    a->pose_set[pose] = 1;
    return 0;
}

int fa_cs_anim_bind(fa_cs_anim *a, fa_cs_pose pose, const char *code)
{
    if (!a || !a->sheet || pose < 0 || pose >= FA_CS_POSE_COUNT) return -1;
    if (!code || !*code) return -1;
    int idx = fa_cs_sheet_find(a->sheet, code);
    if (idx < 0) return -1;
    const fa_cs_clip *src = &a->sheet->clips[idx];
    a->pose_clip[pose] = *src;
    a->pose_set[pose] = 1;
    return 0;
}

int fa_cs_anim_bind_all(fa_cs_anim *a, const char *codes[FA_CS_POSE_COUNT])
{
    if (!a || !codes) return 0;
    int n = 0;
    for (int i = 0; i < FA_CS_POSE_COUNT; i++)
        if (fa_cs_anim_bind(a, (fa_cs_pose)i, codes[i]) == 0) n++;
    return n;
}

void fa_cs_anim_set_period(fa_cs_anim *a, unsigned ticks)
{
    if (a) a->period = ticks;
}

/* Pick the clip for a pose, with fallbacks. Returns NULL only when nothing is
 * bound at all. */
static const fa_cs_clip *resolve_clip(const fa_cs_anim *a, fa_cs_pose pose)
{
    static const fa_cs_pose fallback[] = {
        FA_CS_STAND, FA_CS_WALK, FA_CS_JUMP_FALL
    };
    if (a->pose_set[pose]) return &a->pose_clip[pose];
    for (unsigned i = 0; i < sizeof fallback / sizeof *fallback; i++)
        if (a->pose_set[fallback[i]]) return &a->pose_clip[fallback[i]];
    for (int i = 0; i < FA_CS_POSE_COUNT; i++)
        if (a->pose_set[i]) return &a->pose_clip[i];
    return NULL;
}

void fa_cs_anim_set(fa_cs_anim *a, fa_cs_pose pose, int facing)
{
    if (!a || !a->sheet) return;
    facing = facing < 0 ? -1 : 1;
    const fa_cs_clip *cl = resolve_clip(a, pose);
    if (!cl) { a->has_active = 0; a->frame = -1; return; }

    int same = a->has_active && pose == a->pose && facing == a->facing &&
               a->active.first == cl->first && a->active.last == cl->last;
    a->pose = pose;
    a->facing = facing;
    if (same) return;

    a->active = *cl;
    a->has_active = 1;
    a->sub = 0;
    a->cycles = 0;
    a->frame = cl->first;
}

void fa_cs_anim_tick(fa_cs_anim *a)
{
    if (!a || !a->has_active) return;
    const fa_cs_clip *cl = &a->active;

    if (a->frame < cl->first || a->frame > cl->last) a->frame = cl->first;
    if (a->period == 0) return;

    if (++a->sub < a->period) return;
    a->sub = 0;

    /* Engine updater 0x40AE10: current++, and when it passes the inclusive
     * end, either wrap to loop_first (repeat) or hold (stop). */
    if (a->frame < cl->last) {
        a->frame++;
    } else if (cl->loop) {
        a->frame = cl->loop_first;
        a->cycles++;
    }
}

int fa_cs_anim_frame(const fa_cs_anim *a)
{
    return a && a->has_active ? a->frame : -1;
}

/* --- draw ---------------------------------------------------------- */

/* Decode the active frame into a->cache (host-order RGB565, w*h). Returns 0
 * or -1. */
static int ensure_cache(fa_cs_anim *a)
{
    if (!a->has_active || a->frame < 0) return -1;
    if (a->frame == a->cached_frame && a->cache) return 0;

    int w = 0, h = 0;
    if (fa_w01_frame_size(&a->sheet->w01, a->frame, &w, &h) != 0 ||
        w <= 0 || h <= 0)
        return -1;

    size_t need = (size_t)w * (size_t)h;
    if (!a->cache || (size_t)a->cache_w * a->cache_h < need) {
        uint16_t *nb = (uint16_t *)realloc(a->cache, need * sizeof(uint16_t));
        if (!nb) return -1;
        a->cache = nb;
    }
    if (fa_w01_decode(&a->sheet->w01, a->frame, a->cache) != 0) return -1;
    a->cache_w = w;
    a->cache_h = h;
    a->cached_frame = a->frame;
    return 0;
}

long fa_cs_anim_draw(fa_cs_anim *a, const struct fa_surface *dst,
                     int anchor_x, int anchor_y, const struct fa_rect *clip)
{
    if (!a || !a->sheet || !dst) return -1;
    if (ensure_cache(a) != 0) return -1;

    int w = a->cache_w, h = a->cache_h;
    int ox = 0, oy = 0;
    fa_w01_frame_origin(&a->sheet->w01, a->frame, &ox, &oy);
    /* table A stores the null-point offset as a signed 16-bit value */
    int sox = (int)(int16_t)(uint16_t)ox;
    int soy = (int)(int16_t)(uint16_t)oy;

    int flip = a->facing != a->sheet->base_facing;

    /* build a source surface: a straight or a column-mirrored copy */
    uint16_t *pix = (uint16_t *)malloc((size_t)w * h * sizeof(uint16_t));
    if (!pix) return -1;
    if (!flip) {
        memcpy(pix, a->cache, (size_t)w * h * sizeof(uint16_t));
    } else {
        for (int y = 0; y < h; y++) {
            const uint16_t *sr = a->cache + (size_t)y * w;
            uint16_t *dr = pix + (size_t)y * w;
            for (int x = 0; x < w; x++) dr[x] = sr[w - 1 - x];
        }
    }

    fa_surface src;
    fa_surface_wrap(&src, pix, w, h, 0);

    int dx = flip ? anchor_x - sox - w : anchor_x + sox;
    int dy = anchor_y + soy;

    long n = fa_blit_keyed(dst, dx, dy, &src, NULL, clip, FA_COLORKEY);
    free(pix);
    return n;
}
