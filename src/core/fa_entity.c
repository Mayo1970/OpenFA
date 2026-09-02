/*
 * fa_entity.c - level object / sprite layer. See fa_entity.h.
 */
#include "fa/fa_entity.h"
#include "fa/fa_map.h"
#include "fa/fa_aom.h"
#include "fa/fa_w01.h"
#include "fa/fa_surface.h"
#include "fa/fa_render.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

/* --- little-endian record reads ------------------------------------- */

static int rd_i16(const uint8_t *p)
{
    return (int)(int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static int rd_u16(const uint8_t *p)
{
    return (int)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* --- .W01 sheet cache --------------------------------------------- */

#define FA_ENT_MAX_SHEETS 96

typedef struct {
    char   name[64];        /* basename, as parsed from FileName */
    fa_w01 w01;
    int    ok;
} ent_sheet;

/* --- store ------------------------------------------------------- */

struct fa_entity_store {
    fa_entity_rec *rec;
    int            rec_count;

    fa_aom_def    *def;      /* indexed by obj_nr, def_count entries */
    int           *def_ok;   /* 1 if def[i] was parsed */
    int            def_count;

    ent_sheet      sheet[FA_ENT_MAX_SHEETS];
    int            sheet_count;

    int           *bucket[FA_ENTITY_PLANES][FA_ENTITY_BANDS];
    int            bcount[FA_ENTITY_PLANES][FA_ENTITY_BANDS];

    char           anim_dir[600];

    /* RRR-50 runtime */
    int            base_delay;       /* level[0x1735], 0 on load = 1 fr/tick */
    int            have_start;
    int            start_x, start_y;

    struct { int obj_nr; fa_entity_behaviour fn; void *ctx; } beh[64];
    int            beh_count;

    fa_entity_solid_fn terrain;
    void              *terrain_ctx;
};

/* generic faller physics (stop-gap until per-ObjNr AI, RRR-51) */
#define FA_ENT_GRAVITY   72     /* 1/256 px per tick^2  (~0.28 px/tick^2) */
#define FA_ENT_TERM_VY   (16 * 256)   /* terminal fall = 16 px/tick */
/* RRR-57: the pickup handlers set rec[0x10] = 2 in their state-0 init
 * (collect_paradiso 0x40EE84, collect_energy 0x40EFB9 - both `mov [esi+0x10],2`),
 * so a bob frame lasts base_delay(0) + 2 + 1 = 3 ticks. Was 5 (feel guess). */
#define FA_ENT_PICKUP_DELAY  2  /* rec[0x10] for DetailGroup 1/2 pickups (exe) */

/* --- directory listing (Scripts\*.jrs) --------------------------- */

typedef void (*dir_cb)(const char *name, void *user);

static int list_dir(const char *dir, dir_cb fn, void *user)
{
#if defined(_WIN32)
    char pat[600];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do { fn(fd.cFileName, user); } while (FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
#else
    /* POSIX dirent - guarded so the file still builds where it is absent */
    (void)dir; (void)fn; (void)user;
    return -1;
#endif
}

/* --- AOM definition registry ----------------------------------- */

static int has_ext(const char *name, const char *ext)
{
    size_t n = strlen(name), e = strlen(ext);
    if (n < e) return 0;
    for (size_t i = 0; i < e; i++)
        if (tolower((unsigned char)name[n - e + i]) != tolower((unsigned char)ext[i]))
            return 0;
    return 1;
}

typedef struct { const char *dir; fa_aom_def *tmp; int max_obj; } scan1;

static void scan_max(const char *name, void *u)
{
    scan1 *s = (scan1 *)u;
    if (!has_ext(name, ".jrs")) return;
    char path[700];
    snprintf(path, sizeof path, "%s/%s", s->dir, name);
    if (fa_aom_parse_file(s->tmp, path, NULL, 0) != 0) return;
    if (s->tmp->obj_nr >= s->max_obj) s->max_obj = s->tmp->obj_nr + 1;
}

typedef struct { const char *dir; fa_entity_store *st; } scan2;

static void scan_fill(const char *name, void *u)
{
    scan2 *s = (scan2 *)u;
    if (!has_ext(name, ".jrs")) return;
    char path[700];
    snprintf(path, sizeof path, "%s/%s", s->dir, name);
    fa_aom_def d;
    if (fa_aom_parse_file(&d, path, NULL, 0) != 0) return;
    int n = d.obj_nr;
    if (n < 0 || n >= s->st->def_count) return;
    s->st->def[n] = d;
    s->st->def_ok[n] = 1;
}

static int build_defs(fa_entity_store *st, const char *scripts_dir)
{
    fa_aom_def tmp;
    scan1 a = { scripts_dir, &tmp, 1 };
    if (list_dir(scripts_dir, scan_max, &a) != 0) {
        st->def_count = 0;   /* no Scripts dir: the tail still parses */
        return 0;
    }

    st->def_count = a.max_obj;
    if (st->def_count <= 0) { st->def_count = 0; return 0; }
    st->def = (fa_aom_def *)calloc((size_t)st->def_count, sizeof(fa_aom_def));
    st->def_ok = (int *)calloc((size_t)st->def_count, sizeof(int));
    if (!st->def || !st->def_ok) return -1;

    scan2 b = { scripts_dir, st };
    list_dir(scripts_dir, scan_fill, &b);
    return 0;
}

/* --- sheet lookup --------------------------------------------- */

static const fa_w01 *sheet_for(fa_entity_store *st, const char *basename)
{
    for (int i = 0; i < st->sheet_count; i++)
        if (strcmp(st->sheet[i].name, basename) == 0)
            return st->sheet[i].ok ? &st->sheet[i].w01 : NULL;
    if (st->sheet_count >= FA_ENT_MAX_SHEETS) return NULL;

    ent_sheet *sh = &st->sheet[st->sheet_count++];
    snprintf(sh->name, sizeof sh->name, "%s", basename);
    sh->ok = 0;

    char p[700];
    snprintf(p, sizeof p, "%s/%s", st->anim_dir, basename);
    if (fa_w01_open_file(&sh->w01, p) == 0) { sh->ok = 1; return &sh->w01; }

    /* shipped case mix: try all-upper / all-lower basename */
    char up[64], lo[64];
    snprintf(up, sizeof up, "%s", basename);
    snprintf(lo, sizeof lo, "%s", basename);
    for (char *c = up; *c; c++) *c = (char)toupper((unsigned char)*c);
    for (char *c = lo; *c; c++) *c = (char)tolower((unsigned char)*c);
    snprintf(p, sizeof p, "%s/%s", st->anim_dir, up);
    if (fa_w01_open_file(&sh->w01, p) == 0) { sh->ok = 1; return &sh->w01; }
    snprintf(p, sizeof p, "%s/%s", st->anim_dir, lo);
    if (fa_w01_open_file(&sh->w01, p) == 0) { sh->ok = 1; return &sh->w01; }
    return NULL;
}

/* --- load ---------------------------------------------------- */

static void entity_init_runtime(fa_entity_store *st);
static int ent_frame_box(fa_entity_store *s, const fa_entity_rec *e,
                         int *x0, int *y0, int *x1, int *y1);

fa_entity_store *fa_entity_load(const struct fa_map *map, const char *gdata)
{
    if (!map || !map->tail || map->tail_size < 26 || !gdata) return NULL;

    const uint8_t *tail = map->tail;
    uint32_t rec_bytes = rd_u32(tail);
    int count = rd_u16(tail + 4);
    if (count <= 0) return NULL;

    uint32_t need = 26u + (uint32_t)count * FA_ENTITY_REC_BYTES;
    if (map->tail_size < need) {
        /* tolerate a short tail: parse as many whole records as fit */
        count = (int)((map->tail_size - 26u) / FA_ENTITY_REC_BYTES);
        if (count <= 0) return NULL;
    }
    (void)rec_bytes;

    fa_entity_store *st = (fa_entity_store *)calloc(1, sizeof *st);
    if (!st) return NULL;
    snprintf(st->anim_dir, sizeof st->anim_dir, "%s/Animation", gdata);

    char scripts[600];
    snprintf(scripts, sizeof scripts, "%s/Scripts", gdata);
    if (build_defs(st, scripts) != 0) { fa_entity_free(st); return NULL; }

    st->rec = (fa_entity_rec *)calloc((size_t)count, sizeof(fa_entity_rec));
    if (!st->rec) { fa_entity_free(st); return NULL; }
    st->rec_count = count;

    for (int i = 0; i < count; i++) {
        const uint8_t *r = tail + 26 + (size_t)i * FA_ENTITY_REC_BYTES;
        fa_entity_rec *e = &st->rec[i];
        memcpy(e->raw, r, FA_ENTITY_REC_BYTES);
        e->obj_nr = rd_i16(r + 0x00);
        e->x      = rd_i16(r + 0x02);
        e->y      = rd_i16(r + 0x04);
        e->active = rd_u16(r + 0x06);
        e->frame  = rd_i16(r + 0x0E);
        e->band   = r[0x17];
        e->detail_group = r[0x18];
        e->plane  = r[0x19];
        e->flip_x = r[0x2B] != 0;
        e->hidden = r[0xB9] != 0;
        e->force_offscreen = r[0xB8] != 0;

        /* RRR-50 runtime fields (PL-128) */
        e->active_reset           = rd_u16(r + 0x08);
        e->anim_first             = rd_i16(r + 0x0A);
        e->anim_last              = rd_i16(r + 0x0C);
        e->anim_extra_delay       = rd_i16(r + 0x10);
        e->anim_timer             = rd_i16(r + 0x12);
        e->anim_mode              = r[0x16];
        e->collision_enabled      = r[0x1A] != 0;
        e->automove               = r[0x1C] != 0;
        e->move_step              = r[0x1D];
        e->min_x                  = rd_i16(r + 0x1F);
        e->min_y                  = rd_i16(r + 0x21);
        e->max_x                  = rd_i16(r + 0x23);
        e->max_y                  = rd_i16(r + 0x25);
        e->move_dir               = r[0x27];
        e->wait_reset             = r[0x28];
        e->wait                   = r[0x29];
        e->collision_bottom_adjust = r[0x2A];
        e->flip_at_endpoint       = r[0x2C] != 0;
    }

    /* buckets: (plane, band), file order (matches fcn.00432B50) */
    for (int p = 0; p < FA_ENTITY_PLANES; p++)
        for (int b = 0; b < FA_ENTITY_BANDS; b++) {
            for (int i = 0; i < count; i++) {
                fa_entity_rec *e = &st->rec[i];
                if (e->obj_nr < 0 || e->x == -1 || e->y == -1) continue;
                if (e->plane != p || e->band != b) continue;
                st->bcount[p][b]++;
            }
            if (st->bcount[p][b] == 0) continue;
            st->bucket[p][b] =
                (int *)malloc(sizeof(int) * (size_t)st->bcount[p][b]);
            if (!st->bucket[p][b]) { fa_entity_free(st); return NULL; }
            int k = 0;
            for (int i = 0; i < count; i++) {
                fa_entity_rec *e = &st->rec[i];
                if (e->obj_nr < 0 || e->x == -1 || e->y == -1) continue;
                if (e->plane != p || e->band != b) continue;
                st->bucket[p][b][k++] = i;
            }
        }

    entity_init_runtime(st);
    return st;
}

void fa_entity_free(fa_entity_store *s)
{
    if (!s) return;
    for (int i = 0; i < s->sheet_count; i++)
        if (s->sheet[i].ok) fa_w01_close(&s->sheet[i].w01);
    for (int p = 0; p < FA_ENTITY_PLANES; p++)
        for (int b = 0; b < FA_ENTITY_BANDS; b++)
            free(s->bucket[p][b]);
    free(s->rec);
    free(s->def);
    free(s->def_ok);
    free(s);
}

int fa_entity_count(const fa_entity_store *s) { return s ? s->rec_count : 0; }
int fa_entity_def_count(const fa_entity_store *s) { return s ? s->def_count : 0; }

const fa_entity_rec *fa_entity_at(const fa_entity_store *s, int i)
{
    if (!s || i < 0 || i >= s->rec_count) return NULL;
    return &s->rec[i];
}

fa_entity_rec *fa_entity_at_mut(fa_entity_store *s, int i)
{
    if (!s || i < 0 || i >= s->rec_count) return NULL;
    return &s->rec[i];
}

const struct fa_aom_def *fa_entity_def(const fa_entity_store *s, int obj_nr)
{
    if (!s || obj_nr < 0 || obj_nr >= s->def_count || !s->def_ok[obj_nr])
        return NULL;
    return &s->def[obj_nr];
}

int fa_entity_frame_box(const fa_entity_store *s, int i,
                        int *x0, int *y0, int *x1, int *y1)
{
    if (!s || i < 0 || i >= s->rec_count) return -1;
    return ent_frame_box((fa_entity_store *)s, &s->rec[i], x0, y0, x1, y1);
}

/* --- resolve a record to a decoded frame ----------------------- */

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The .W01 sheet an ObjNr draws from (for enemy projectiles, which the exe
 * renders from the throwing enemy's own sheet - 0x40AF80). NULL if unknown. */
const struct fa_w01 *fa_entity_obj_sheet(const fa_entity_store *s, int obj_nr)
{
    if (!s || obj_nr < 0 || obj_nr >= s->def_count || !s->def_ok[obj_nr])
        return NULL;
    char base[FA_AOM_FILENAME_MAX];
    if (fa_aom_def_basename(&s->def[obj_nr], base, sizeof base) < 0) return NULL;
    return sheet_for((fa_entity_store *)s, base);
}

/* Returns the .W01 + frame index for a record, or NULL. Fills *frame_out. */
static const fa_w01 *resolve(fa_entity_store *s, const fa_entity_rec *e,
                             int *frame_out)
{
    if (e->obj_nr < 0 || e->obj_nr >= s->def_count || !s->def_ok[e->obj_nr])
        return NULL;
    const fa_aom_def *d = &s->def[e->obj_nr];

    char base[FA_AOM_FILENAME_MAX];
    if (fa_aom_def_basename(d, base, sizeof base) < 0) return NULL;

    const fa_w01 *w = sheet_for(s, base);
    if (!w) return NULL;

    int lo = d->file_anim_start, hi = d->file_anim_end;
    if (hi < lo) hi = lo;
    int f = clampi(e->frame, lo, hi) - lo;
    int n = fa_w01_count(w);
    if (f < 0 || f >= n) f = 0;
    *frame_out = f;
    return w;
}

int fa_entity_drawable(const fa_entity_store *s)
{
    if (!s) return 0;
    int n = 0;
    fa_entity_store *m = (fa_entity_store *)s;   /* resolve() populates caches */
    for (int i = 0; i < s->rec_count; i++) {
        const fa_entity_rec *e = &s->rec[i];
        if (!e->active || e->hidden || e->obj_nr < 0) continue;
        int f;
        if (resolve(m, e, &f)) n++;
    }
    return n;
}

/* --- draw one bucket ----------------------------------------- */

int fa_entity_draw_band(const struct fa_surface *dst, const fa_entity_store *s,
                        const struct fa_camera *cam, int plane, int band)
{
    if (!dst || !s || !cam) return -1;
    if (plane < 0 || plane >= FA_ENTITY_PLANES ||
        band < 0 || band >= FA_ENTITY_BANDS)
        return 0;

    fa_entity_store *m = (fa_entity_store *)s;
    const int *bk = s->bucket[plane][band];
    int bn = s->bcount[plane][band];
    int drawn = 0;

    for (int j = 0; j < bn; j++) {
        const fa_entity_rec *e = &s->rec[bk[j]];
        if (!e->active || e->hidden) continue;

        int frame = 0;
        const fa_w01 *w = resolve(m, e, &frame);
        if (!w) continue;

        int fw = 0, fh = 0;
        if (fa_w01_frame_size(w, frame, &fw, &fh) != 0 || fw <= 0 || fh <= 0)
            continue;

        /* render position == the hitbox (ent_frame_box), so a sprite and its
         * collision box always coincide (RRR-51). */
        int wx0, wy0, wx1, wy1;
        if (ent_frame_box(m, e, &wx0, &wy0, &wx1, &wy1) != 0) continue;
        int dx = wx0 - cam->x, dy = wy0 - cam->y;

        if (!e->force_offscreen &&
            (dx + fw < 0 || dx > cam->vw || dy + fh < 0 || dy > cam->vh))
            continue;

        uint16_t *px = (uint16_t *)malloc((size_t)fw * fh * sizeof(uint16_t));
        if (!px) continue;
        if (fa_w01_decode(w, frame, px) != 0) { free(px); continue; }

        if (e->flip_x) {
            for (int y = 0; y < fh; y++) {
                uint16_t *row = px + (size_t)y * fw;
                for (int x = 0; x < fw / 2; x++) {
                    uint16_t t = row[x];
                    row[x] = row[fw - 1 - x];
                    row[fw - 1 - x] = t;
                }
            }
        }
        fa_surface src;
        fa_surface_wrap(&src, px, fw, fh, 0);

        fa_blit_keyed(dst, dx, dy, &src, NULL, NULL, FA_COLORKEY);
        free(px);
        drawn++;
    }
    return drawn;
}

/* ================================================================
 * RRR-50 runtime  (fcn.004335A0 update, fcn.00430CF0/D90 patrol,
 * fcn.004118F5 player start, fcn.00412900 lift carry)
 * ================================================================ */

/* Collision box of a pushable block: the sprite frame inset ~22% each side,
 * so it matches the visible base (the totem head is wider than the base;
 * PL-133 / push-disasm.md) and does not falsely jam against nearby scenery.
 * -1 if it has no sprite. */
static int block_box(fa_entity_store *s, const fa_entity_rec *e,
                     int *x0, int *y0, int *x1, int *y1);


/*
 * Where a record's current frame is drawn, in world pixels. Traced to the
 * placed-AOM renderer 0x4338A0 + the geometry helper 0x431300 and the
 * load-time origin fix-up 0x443EA0 (RRR-51, RRR-51/enemy-render-and-hit-
 * disasm.md Q1).
 *
 * Split the .W01 table-A dword into signed words a_x(f)/a_y(f). With
 * CorrectAlignToNull = 1 (the AOM default; every shipped asset sets it) the
 * loader translates the whole origin track so sheet frame 0 lands at (0,0),
 * i.e. the on-screen offset is a(f) - a(0). This absorbs Parrot02.w01's
 * authored (+189, +41) with NO special case, and leaves the static scenery
 * (frame 0 already (0,0)) exactly where it was.
 *
 *   rec[+0x2B] == 0 :  left = X + o_x(f)
 *   rec[+0x2B] != 0 :  left = X + ReferenceSprWidth - w(f) - o_x(f)
 *   top = Y + o_y(f)
 *
 * ReferenceSprWidth only defines the mirror span; it never shifts an
 * unmirrored blit. It defaults to 0 when the script omits it.
 */
static int ent_frame_box(fa_entity_store *s, const fa_entity_rec *e,
                         int *x0, int *y0, int *x1, int *y1)
{
    int f = 0;
    const fa_w01 *w = resolve(s, e, &f);
    if (!w) return -1;
    int fw = 0, fh = 0;
    if (fa_w01_frame_size(w, f, &fw, &fh) != 0 || fw <= 0 || fh <= 0) return -1;

    int ox = 0, oy = 0;
    fa_w01_frame_origin(w, f, &ox, &oy);
    int sox = (int)(int16_t)(uint16_t)ox;
    int soy = (int)(int16_t)(uint16_t)oy;

    int refw = 0, align = 1;
    if (e->obj_nr >= 0 && e->obj_nr < s->def_count && s->def_ok[e->obj_nr]) {
        refw  = s->def[e->obj_nr].reference_spr_width;
        align = s->def[e->obj_nr].correct_align_to_null;
    }
    if (align) {
        int o0x = 0, o0y = 0;
        fa_w01_frame_origin(w, 0, &o0x, &o0y);
        sox -= (int)(int16_t)(uint16_t)o0x;
        soy -= (int)(int16_t)(uint16_t)o0y;
    }

    int left = e->flip_x ? (e->x + refw - fw - sox) : (e->x + sox);
    int top  = e->y + soy;
    *x0 = left; *y0 = top; *x1 = left + fw; *y1 = top + fh;
    return 0;
}

static int block_box(fa_entity_store *s, const fa_entity_rec *e,
                     int *x0, int *y0, int *x1, int *y1)
{
    /* PL-135 / PL-136: the player-contact box of a pushable block is the full
     * sprite AABB - [X, X+fw) x [Y + rec[0x2A], Y+fh). No horizontal inset
     * (the earlier 22% inset did not match the exe). */
    if (ent_frame_box(s, e, x0, y0, x1, y1) != 0) return -1;
    *y0 += e->collision_bottom_adjust;      /* rec[0x2A], 0 for 76/78        */
    if (*y1 <= *y0) *y1 = *y0 + 1;
    return 0;
}

static void entity_init_runtime(fa_entity_store *st)
{
    st->base_delay = 0;      /* level[0x1735] = 0 on load (0x43267C) */
    st->have_start = 0;
    st->beh_count  = 0;

    for (int i = 0; i < st->rec_count; i++) {
        fa_entity_rec *e = &st->rec[i];

        /* the ObjNr 1000 (misc_start.jrs) marker carries the spawn X/Y; the
         * exe reads rec[+2]/rec[+4], places the player, then clears rec[+6]
         * so it never renders (0x4118F5, PL-127). */
        if (e->obj_nr == 1000) {
            st->have_start = 1;
            st->start_x = e->x;
            st->start_y = e->y;
            e->active = 0;
            continue;
        }
        if (e->active == 0 || e->obj_nr < 0) continue;

        /* generic default animation state = AOM state 0 (StartAnimLeft/
         * EndAnimLeft), else the FileAnim range. Per-ObjNr callbacks switch
         * states later (RRR-51). */
        if (e->obj_nr < st->def_count && st->def_ok[e->obj_nr]) {
            const fa_aom_def *d = &st->def[e->obj_nr];
            if (d->move[FA_DIR_LEFT].set) {
                e->anim_first = d->move[FA_DIR_LEFT].start;
                e->anim_last  = d->move[FA_DIR_LEFT].end;
            } else {
                e->anim_first = d->file_anim_start;
                e->anim_last  = d->file_anim_end;
            }
        }
        if (e->anim_last < e->anim_first) e->anim_last = e->anim_first;
        e->frame       = (e->anim_mode == 2) ? e->anim_last : e->anim_first;
        e->anim_timer  = 0;
        e->anim_cycles = 0;
        e->dx = e->dy  = 0;

        /* pickups bob / rotate much slower than the 1-frame-per-tick generic
         * rate (owner playtest). The per-ObjNr AI would set rec[+0x10]
         * (RRR-51); as a stop-gap give DetailGroup 1/2 a slow default when
         * the disk value is 0. */
        if ((e->detail_group == 1 || e->detail_group == 2) &&
            e->anim_extra_delay == 0)
            e->anim_extra_delay = FA_ENT_PICKUP_DELAY;

        /* initial patrol direction: head toward the farther bound
         * (fcn.00430CF0). Sentinel -1 on the min field = that axis is free. */
        e->move_dir = 0;
        if (e->automove) {
            if (e->min_x != -1 && e->max_x != -1) {
                int from_min = e->x - e->min_x, to_max = e->max_x - e->x;
                e->move_dir |= (from_min <= to_max) ? 2 : 1;
            }
            if (e->min_y != -1 && e->max_y != -1) {
                int from_min = e->y - e->min_y, to_max = e->max_y - e->y;
                e->move_dir |= (from_min <= to_max) ? 8 : 4;
            }
        }

        /* classification. DetailGroup 0 = solid movable objects: a raft
         * moves or has patrol bounds (fcn.00411922 lift list); a block
         * (ObjNr 76 dschungel / 78 eis, DetailGroup 0, no motion) is
         * pushable (PL-103). Everything else DetailGroup 3 (enemy) with
         * collision on and no vertical patrol falls under generic gravity
         * until RRR-51 gives it real per-type movement. */
        e->is_lift = e->is_block = e->gravity = 0;
        e->vy_acc = e->deck_off = 0;
        if (e->obj_nr == 83) {
            /* RRR-61: the FABBRICA boss-arena button. is_block so the player
             * push probe (fa_beh_push) can register a shove; beh_button
             * watches for it and never lets the button slide. */
            e->is_block = 1;
        } else if (e->detail_group == 0) {
            if (e->obj_nr == 76 || e->obj_nr == 78 ||
                e->obj_nr == 86 || e->obj_nr == 87) {
                /* pushable blocks (PL-135): ObjNr 76/78/86/87 all run
                 * fcn.00414E50. Full-box solid; the real float physics
                 * (friction / gravity / swept collision / fall counter) is
                 * fa_beh's beh_block - the e->gravity stop-gap below only
                 * runs when no behaviour layer is bound (headless tests). */
                e->is_block = 1;
                e->gravity  = 1;
            } else {
                /* a stand-on platform / raft: wide-and-flat DetailGroup-0.
                 * A tall DetailGroup-0 sprite (a lever misc_schalter, a
                 * barrel misc_fass) is not a platform - leave it as scenery
                 * until RRR-51. The stand-on surface is
                 * `sprite_top + collision_bottom_adjust` (record +0x2A) -
                 * the exact top of the collision box the exe builds at
                 * 0x412A89 (box y0 = rec.Y + frame_origin_y + rec[+0x2A]);
                 * general, per-object, no sprite heuristic (PL-136). */
                int x0, y0, x1, y1;
                if (ent_frame_box(st, e, &x0, &y0, &x1, &y1) == 0 &&
                    (x1 - x0) > (y1 - y0)) {
                    e->is_lift  = 1;
                    e->deck_off = e->collision_bottom_adjust;
                }
            }
        } else if (e->detail_group == 3 && e->collision_enabled &&
                   e->min_y == -1) {
            e->gravity = 1;
        }
    }
}

/* One patrol axis: move `step` px toward the bound, then bounce / wait at it
 * (fcn.00430D90). X bounce flips the sprite when flip_at_endpoint; Y never
 * does (0x431178 has no facing XOR). */
static void patrol_axis(int *pos, int *dir, int lo_bit, int hi_bit,
                        int lo, int hi, int step, int *wait, int wait_reset,
                        int *flip_x, int flip_at_endpoint)
{
    if (*dir & lo_bit) {                         /* toward `lo` */
        if (*pos > lo) { *pos -= step; if (*pos < lo) *pos = lo; }
        if (*pos <= lo) {
            if (*wait > 0) { (*wait)--; return; }
            *wait = wait_reset;
            *dir  = (*dir & ~lo_bit) | hi_bit;
            if (flip_x && flip_at_endpoint && lo_bit == 1) *flip_x ^= 1;
        }
    } else if (*dir & hi_bit) {                  /* toward `hi` */
        if (*pos < hi) { *pos += step; if (*pos > hi) *pos = hi; }
        if (*pos >= hi) {
            if (*wait > 0) { (*wait)--; return; }
            *wait = wait_reset;
            *dir  = (*dir & ~hi_bit) | lo_bit;
            if (flip_x && flip_at_endpoint && hi_bit == 2) *flip_x ^= 1;
        }
    }
}

static int ent_on_screen(fa_entity_store *s, const fa_entity_rec *e,
                         int cx, int cy, int vw, int vh)
{
    int x0, y0, x1, y1;
    if (ent_frame_box(s, e, &x0, &y0, &x1, &y1) != 0) {
        x0 = x1 = e->x; y0 = y1 = e->y;
    }
    const int M = 384;   /* the exe keeps ~350px of slack (0x4336xx) */
    return !(x1 < cx - M || x0 > cx + vw + M ||
             y1 < cy - M || y0 > cy + vh + M);
}

int fa_entity_tick(fa_entity_store *s, int cx, int cy, int vw, int vh)
{
    if (!s) return -1;
    int ticked = 0;

    for (int i = 0; i < s->rec_count; i++) {
        fa_entity_rec *e = &s->rec[i];
        e->dx = e->dy = 0;
        if (e->active == 0 || e->obj_nr < 0) continue;

        /* RRR-60: a collected respawning pickup (DetailGroup 1/2, ammo/energy)
         * counts down hidden, then reappears - exe rec[+0x74] = 0x4B0 (1200 t). */
        if ((e->detail_group == FA_AOM_DG_BONUS ||
             e->detail_group == FA_AOM_DG_POWERUP) &&
            e->hidden && e->bs[0] > 0) {
            if (--e->bs[0] == 0) { e->hidden = 0; e->force_offscreen = 0; }
            continue;
        }

        /* a shoved block keeps falling off-screen until it lands (or is lost
         * in a pit); everything else obeys the on-screen update gate */
        if (!e->is_block && !e->force_offscreen &&
            !ent_on_screen(s, e, cx, cy, vw, vh)) continue;

        int ox = e->x, oy = e->y;

        /* --- animation advance (fcn.004335A0 @0x4337A1) --- */
        int wrapped = 0;
        if (e->anim_last > e->anim_first) {
            if (e->anim_timer > 0) {
                e->anim_timer--;
            } else {
                e->anim_timer = s->base_delay + e->anim_extra_delay;
                if (e->anim_mode == 2) {
                    if (e->frame > e->anim_first) e->frame--;
                    else { e->frame = e->anim_last; wrapped = 1; }
                } else {
                    if (e->frame < e->anim_last) e->frame++;
                    else { e->frame = e->anim_first; wrapped = 1; }
                }
            }
            if (wrapped) {
                e->anim_cycles++;
                if (e->active != 0xFF && e->active > 0) {
                    e->active--;               /* finite lifetime (0x433828) */
                    if (e->active == 0) { ticked++; continue; }
                }
            }
        }

        /* --- behaviour seam then generic gravity + patrol --- */
        int run_patrol = 1, has_beh = 0;
        for (int b = 0; b < s->beh_count; b++)
            if (s->beh[b].obj_nr == e->obj_nr && s->beh[b].fn) {
                has_beh = 1;
                run_patrol = s->beh[b].fn(e, wrapped, s->beh[b].ctx) != 0;
                break;
            }

        /* generic physics (stop-gap; RRR-51 replaces it with the per-type
         * AI). The ground probe uses the frame's bottom edge (align-to-null
         * feet for enemies, sprite bottom for blocks); e->y is the no-sprite
         * fallback. */
        if (e->gravity && !has_beh && s->terrain) {
            int bx0, by0, bx1, by1, foot = e->y + 1, cbx = e->x;
            if (e->is_block) {
                if (block_box(s, e, &bx0, &by0, &bx1, &by1) == 0) {
                    foot = by1; cbx = (bx0 + bx1) / 2;
                }
            } else if (ent_frame_box(s, e, &bx0, &by0, &bx1, &by1) == 0) {
                foot = by1;
            }

            if (e->is_block) {
                /* blocks: continuous gravity (fcn.00414E50) - shoved off a
                 * ledge they fall to the surface below. A block that falls
                 * >1024 px with no landing is lost in a pit and removed
                 * (matches record[+0x74] >= 1000 at 0x414FB5). */
                int on_ground = s->terrain(bx0, foot, s->terrain_ctx) != 0 ||
                                s->terrain(bx1, foot, s->terrain_ctx) != 0 ||
                                s->terrain(cbx, foot, s->terrain_ctx) != 0;
                if (!on_ground) {
                    e->vy_acc += FA_ENT_GRAVITY;
                    if (e->vy_acc > FA_ENT_TERM_VY) e->vy_acc = FA_ENT_TERM_VY;
                    int fall = e->vy_acc / 256;
                    for (int k = 0; k < fall; k++) {
                        if (s->terrain(bx0, foot, s->terrain_ctx) != 0 ||
                            s->terrain(bx1, foot, s->terrain_ctx) != 0 ||
                            s->terrain(cbx, foot, s->terrain_ctx) != 0)
                            break;
                        e->y++; foot++; e->fall_px++;
                    }
                    if (e->fall_px > 1024) { e->active = 0; ticked++; continue; }
                } else {
                    e->vy_acc = e->fall_px = 0;
                }
            } else if (!e->snapped) {
                /* enemies: a ONE-TIME spawn ground-snap, not continuous
                 * gravity - so an enemy perched at a platform edge is not
                 * dragged off it and lost. Only commit the snap when ground
                 * is actually within reach (else it is placed over a gap on
                 * purpose - leave it for RRR-51's per-type AI). */
                e->snapped = 1;
                int drop = 0;
                while (drop < 224 &&
                       s->terrain(e->x, foot + drop, s->terrain_ctx) == 0)
                    drop++;
                if (drop < 224) e->y += drop;
            }
        }
        if (run_patrol && e->automove && e->move_dir) {
            if (e->min_x != -1 && e->max_x != -1)
                patrol_axis(&e->x, &e->move_dir, 1, 2, e->min_x, e->max_x,
                            e->move_step, &e->wait, e->wait_reset,
                            &e->flip_x, e->flip_at_endpoint);
            if (e->min_y != -1 && e->max_y != -1)
                patrol_axis(&e->y, &e->move_dir, 4, 8, e->min_y, e->max_y,
                            e->move_step, &e->wait, e->wait_reset,
                            NULL, 0);
        }

        e->dx = e->x - ox;
        e->dy = e->y - oy;
        ticked++;
    }
    return ticked;
}

int fa_entity_player_start(const fa_entity_store *s, int *x, int *y)
{
    if (!s || !s->have_start) return 0;
    if (x) *x = s->start_x;
    if (y) *y = s->start_y;
    return 1;
}

int fa_entity_set_behaviour(fa_entity_store *s, int obj_nr,
                            fa_entity_behaviour fn, void *ctx)
{
    if (!s) return -1;
    for (int i = 0; i < s->beh_count; i++)
        if (s->beh[i].obj_nr == obj_nr) {
            s->beh[i].fn = fn; s->beh[i].ctx = ctx;
            return 0;
        }
    if (s->beh_count >= (int)(sizeof s->beh / sizeof s->beh[0])) return -1;
    s->beh[s->beh_count].obj_nr = obj_nr;
    s->beh[s->beh_count].fn = fn;
    s->beh[s->beh_count].ctx = ctx;
    s->beh_count++;
    return 0;
}

int fa_entity_ride(const fa_entity_store *s, int px, int feet_y, int half_w,
                   int *lift_top, int *carry_dx, int *carry_dy)
{
    if (!s) return 0;
    fa_entity_store *m = (fa_entity_store *)s;
    for (int i = 0; i < s->rec_count; i++) {
        const fa_entity_rec *e = &s->rec[i];
        if (e->active == 0 || !e->is_lift) continue;
        int x0, y0, x1, y1;
        if (ent_frame_box(m, e, &x0, &y0, &x1, &y1) != 0) continue;
        if (px + half_w < x0 || px - half_w > x1) continue;
        int top = y0 + e->deck_off;         /* per-sprite stand-on surface */
        if (feet_y < top - FA_ENTITY_RIDE_SLOP ||
            feet_y > top + FA_ENTITY_RIDE_SLOP)
            continue;
        if (lift_top)  *lift_top  = top;
        if (carry_dx)  *carry_dx  = e->dx;
        if (carry_dy)  *carry_dy  = e->dy;
        return 1;
    }
    return 0;
}

int fa_entity_collect(fa_entity_store *s, int px, int py,
                      int half_w, int half_h,
                      fa_entity_pickup_cb cb, void *ctx)
{
    if (!s) return -1;
    int n = 0;
    int pl = px - half_w, pr = px + half_w, pt = py - half_h, pb = py + half_h;
    for (int i = 0; i < s->rec_count; i++) {
        fa_entity_rec *e = &s->rec[i];
        if (e->active == 0 || e->hidden) continue;
        if (e->detail_group != FA_AOM_DG_BONUS &&
            e->detail_group != FA_AOM_DG_POWERUP)
            continue;
        int x0, y0, x1, y1;
        if (ent_frame_box(s, e, &x0, &y0, &x1, &y1) != 0) continue;
        if (pr < x0 || pl > x1 || pb < y0 || pt > y1) continue;
        int respawn = cb ? cb(e->obj_nr, e->detail_group, ctx) : 0;
        n++;
        if (respawn > 0) {              /* exe: hide, count rec[+0x74], reappear */
            e->hidden = 1;
            e->force_offscreen = 1;     /* keep it ticking while hidden          */
            e->bs[0] = respawn;
        } else {
            e->active = 0;              /* rec[+6] = 0 (the exe pickup path)     */
        }
    }
    return n;
}

int fa_entity_hazard(const fa_entity_store *s, int px, int py,
                     int half_w, int half_h, int *idx)
{
    if (!s) return 0;
    fa_entity_store *m = (fa_entity_store *)s;
    int pl = px - half_w, pr = px + half_w, pt = py - half_h, pb = py + half_h;
    for (int i = 0; i < s->rec_count; i++) {
        const fa_entity_rec *e = &s->rec[i];
        if (e->active == 0 || e->detail_group != FA_AOM_DG_ENEMY) continue;
        if (!e->collision_enabled) continue;
        int x0, y0, x1, y1;
        if (ent_frame_box(m, e, &x0, &y0, &x1, &y1) != 0) continue;
        if (pr < x0 || pl > x1 || pb < y0 || pt > y1) continue;
        if (idx) *idx = i;
        return 1;
    }
    return 0;
}

void fa_entity_set_terrain(fa_entity_store *s, fa_entity_solid_fn fn, void *ctx)
{
    if (!s) return;
    s->terrain = fn;
    s->terrain_ctx = ctx;
}

int fa_entity_solid_at(const fa_entity_store *s, int px, int py)
{
    if (!s) return 0;
    fa_entity_store *m = (fa_entity_store *)s;
    for (int i = 0; i < s->rec_count; i++) {
        const fa_entity_rec *e = &s->rec[i];
        if (e->active == 0 || (!e->is_lift && !e->is_block)) continue;
        int x0, y0, x1, y1;
        int r = e->is_block ? block_box(m, e, &x0, &y0, &x1, &y1)
                            : ent_frame_box(m, e, &x0, &y0, &x1, &y1);
        if (r != 0) continue;
        if (px < x0 || px > x1) continue;
        if (e->is_block) {
            if (py >= y0 && py <= y1) return 1;      /* inset box, hard solid */
        } else {
            /* lift / raft: a THIN one-way strip just below the per-sprite
             * deck line - fa_collide grounds the kid when his feet sit at
             * the deck and fn(feet) reads NONE while fn(feet+1) reads
             * ONEWAY (fa_collide.c floor_blocks). Passable from below so a
             * jump reaches the top. The fa_slice pre-tick snap keeps the
             * feet exactly on the deck while the platform moves. */
            int deck = y0 + e->deck_off;
            if (py >= deck + 1 && py <= deck + 5) return 2;
        }
    }
    return 0;
}

int fa_entity_pushable_at(const fa_entity_store *s, int px, int py)
{
    if (!s) return 0;
    fa_entity_store *m = (fa_entity_store *)s;
    for (int i = 0; i < s->rec_count; i++) {
        const fa_entity_rec *e = &s->rec[i];
        if (e->active == 0 || !e->is_block) continue;
        int x0, y0, x1, y1;
        if (block_box(m, e, &x0, &y0, &x1, &y1) != 0) continue;
        /* the caller probes a short reach past the player's own front edge,
         * so this is an exact half-open box test (PL-135). */
        if (px >= x0 && px < x1 && py >= y0 && py < y1) return 1;
    }
    return 0;
}

int fa_entity_shove(fa_entity_store *s, int px, int feet_y, int facing,
                    int reach, int step)
{
    if (!s || (facing != 1 && facing != -1)) return 0;
    int probe_x = px + facing * reach;
    int probe_y = feet_y - 16;

    for (int i = 0; i < s->rec_count; i++) {
        fa_entity_rec *e = &s->rec[i];
        if (e->active == 0 || !e->is_block) continue;
        int x0, y0, x1, y1;
        if (block_box(s, e, &x0, &y0, &x1, &y1) != 0) continue;
        if (probe_x < x0 - 8 || probe_x > x1 + 8) continue;
        if (probe_y < y0 || probe_y > y1) continue;

        /* shove a block while ANY part of its base still rests on ground -
         * so it can be pushed off the edge; a fully-airborne block just
         * falls (fcn.00414E50 gravity). */
        int grounded = !s->terrain ||
            s->terrain(x0, y1 + 1, s->terrain_ctx) != 0 ||
            s->terrain(x1, y1 + 1, s->terrain_ctx) != 0 ||
            s->terrain((x0 + x1) / 2, y1 + 1, s->terrain_ctx) != 0;
        if (!grounded) return 0;

        /* move a pixel at a time; only a solid WALL across the block's mid
         * height stops it - a ledge does NOT (it slides off, then falls) */
        int moved = 0;
        for (int k = 0; k < step; k++) {
            int lead_x = (facing > 0) ? (x1 + 1) : (x0 - 1);
            int blocked = 0;
            if (s->terrain)
                for (int y = y0 + 8; y < y1 - 6 && !blocked; y += 4)
                    if (s->terrain(lead_x, y, s->terrain_ctx) == 1) blocked = 1;
            if (blocked) break;
            e->x += facing;
            x0 += facing; x1 += facing;
            moved += facing;
        }
        e->dx += moved;
        return moved;
    }
    return 0;
}
