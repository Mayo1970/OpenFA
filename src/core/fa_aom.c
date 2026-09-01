/*
 * fa_aom.c - animated-object (AOM) contract + runtime (RRR-37)
 * See include/fa/fa_aom.h for the model and the RRR-21 / RRR-9 basis.
 */
#include "fa/fa_aom.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fa_aom_advance(fa_aom_obj *o);

/* --- small helpers ------------------------------------------------- */

static const char *const DIR_NAME[FA_DIR_COUNT] = {
    "Left", "Right", "Up", "Do", "LeftUp", "LeftDo", "RightUp", "RightDo"
};

const char *fa_dir_name(fa_dir d)
{
    return (d >= 0 && d < FA_DIR_COUNT) ? DIR_NAME[d] : "?";
}

const char *fa_aom_detail_group_name(fa_aom_detail_group g)
{
    switch (g) {
    case FA_AOM_DG_LIFT:    return "LIFT/PLATTFORM";
    case FA_AOM_DG_BONUS:   return "BONUS";
    case FA_AOM_DG_POWERUP: return "POWERUP";
    case FA_AOM_DG_ENEMY:   return "ENEMY";
    case FA_AOM_DG_DETAIL:  return "DETAIL";
    default:               return "?";
    }
}

/* Append a diagnostic line. Safe with buf == NULL. */
static void diag_add(char *buf, size_t cap, const char *fmt, ...)
{
    if (!buf || cap == 0)
        return;
    size_t used = strlen(buf);
    if (used + 1 >= cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + used, cap - used, fmt, ap);
    va_end(ap);
    used = strlen(buf);
    if (used + 1 < cap) {
        buf[used] = '\n';
        buf[used + 1] = '\0';
    }
}

/* The whole string must be one optionally-signed decimal integer - matching
 * the binary's 16-bit decimal value reader (fcn.00445043). Returns 0 / -1. */
static int parse_int(const char *s, long *out)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0')
        return -1;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s)
        return -1;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end != '\0')
        return -1;
    *out = v;
    return 0;
}

/* --- key dispatch ------------------------------------------------- */

typedef enum {
    K_UNKNOWN = 0, K_TYP, K_OBJNR, K_FILENAME,
    K_FILE_START, K_FILE_END, K_ALIGN, K_REFW, K_VIDMEM, K_DGROUP,
    K_MOVE_START, K_MOVE_END, K_IDLE_START, K_IDLE_END,
    K_SP_START, K_SP_END
} kkind;

/* Classify a key. For range keys, *idx is the direction (0..7) or the
 * special index (0 attack, 1 freeze, 2 ko). */
static kkind classify(const char *key, int *idx)
{
    *idx = 0;
    if (!strcmp(key, "SCRIPT_TYP"))         return K_TYP;
    if (!strcmp(key, "ObjNr"))              return K_OBJNR;
    if (!strcmp(key, "FileName"))           return K_FILENAME;
    if (!strcmp(key, "FileAnimStart"))      return K_FILE_START;
    if (!strcmp(key, "FileAnimEnd"))        return K_FILE_END;
    if (!strcmp(key, "CorrectAlignToNull")) return K_ALIGN;
    if (!strcmp(key, "ReferenceSprWidth"))  return K_REFW;
    if (!strcmp(key, "UseVideoMem"))        return K_VIDMEM;
    if (!strcmp(key, "DetailGroup"))        return K_DGROUP;

    const char *rest;
    int is_start;
    if (!strncmp(key, "StartAnim", 9))      { rest = key + 9; is_start = 1; }
    else if (!strncmp(key, "EndAnim", 7))   { rest = key + 7; is_start = 0; }
    else                                    return K_UNKNOWN;

    if (!strcmp(rest, "Attack")) { *idx = 0; return is_start ? K_SP_START : K_SP_END; }
    if (!strcmp(rest, "Freeze")) { *idx = 1; return is_start ? K_SP_START : K_SP_END; }
    if (!strcmp(rest, "KO"))     { *idx = 2; return is_start ? K_SP_START : K_SP_END; }

    int idle = 0;
    if (!strncmp(rest, "Idle", 4)) { idle = 1; rest += 4; }
    for (int d = 0; d < FA_DIR_COUNT; d++) {
        if (!strcmp(rest, DIR_NAME[d])) {
            *idx = d;
            if (idle) return is_start ? K_IDLE_START : K_IDLE_END;
            return is_start ? K_MOVE_START : K_MOVE_END;
        }
    }
    return K_UNKNOWN;
}

/* --- the parser ------------------------------------------------- */

struct seen {
    int gotS, gotE;
};

int fa_aom_parse(fa_aom_def *def, const char *src, size_t len,
                 char *diag, size_t diag_cap)
{
    if (diag && diag_cap)
        diag[0] = '\0';
    if (!def || (!src && len))
        return -1;

    memset(def, 0, sizeof(*def));
    def->correct_align_to_null = 1;   /* DoScript default 1 when absent */
    def->use_video_mem = 1;           /* DoScript default 1 when absent */

    struct seen sm[FA_DIR_COUNT] = {{0}}, si[FA_DIR_COUNT] = {{0}}, sp[3] = {{0}};
    int got_typ = 0, typ_ok = 0, got_objnr = 0, got_filename = 0;
    int hard_error = 0;

    size_t i = 0;
    int lineno = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && src[j] != '\n') j++;
        size_t e = j;
        if (e > i && src[e - 1] == '\r') e--;
        const char *ls = src + i, *le = src + e;
        i = (j < len) ? j + 1 : len;
        lineno++;

        /* blank? */
        const char *p = ls;
        while (p < le && isspace((unsigned char)*p)) p++;
        if (p == le)
            continue;

        /* split on '=' */
        const char *eq = memchr(p, '=', (size_t)(le - p));
        if (!eq) {
            diag_add(diag, diag_cap, "warning: line %d: no '=', ignored", lineno);
            continue;
        }
        /* key */
        const char *ks = p, *kend = eq;
        while (kend > ks && isspace((unsigned char)kend[-1])) kend--;
        size_t klen = (size_t)(kend - ks);
        char key[64];
        if (klen == 0 || klen >= sizeof(key)) {
            diag_add(diag, diag_cap, "warning: line %d: bad key, ignored", lineno);
            continue;
        }
        memcpy(key, ks, klen);
        key[klen] = '\0';
        int bad = !(isalpha((unsigned char)key[0]) || key[0] == '_');
        for (size_t k = 1; !bad && k < klen; k++)
            bad = !(isalnum((unsigned char)key[k]) || key[k] == '_');
        if (bad) {
            diag_add(diag, diag_cap, "warning: line %d: bad key %s, ignored",
                     lineno, key);
            continue;
        }

        /* value: strip leading ws, "//" comment, trailing ws, one ';', quotes */
        const char *vs = eq + 1, *ve = le;
        while (vs < ve && isspace((unsigned char)*vs)) vs++;
        for (const char *c = vs; c + 1 < ve; c++)
            if (c[0] == '/' && c[1] == '/') { ve = c; break; }
        while (ve > vs && isspace((unsigned char)ve[-1])) ve--;
        if (ve > vs && ve[-1] == ';') ve--;
        while (ve > vs && isspace((unsigned char)ve[-1])) ve--;
        if (ve - vs >= 2 && vs[0] == '"' && ve[-1] == '"') { vs++; ve--; }
        size_t vlen = (size_t)(ve - vs);
        char val[512];
        if (vlen >= sizeof(val))
            vlen = sizeof(val) - 1;
        memcpy(val, vs, vlen);
        val[vlen] = '\0';

        int idx = 0;
        kkind kk = classify(key, &idx);
        long n = 0;

        switch (kk) {
        case K_UNKNOWN:
            diag_add(diag, diag_cap, "line %d: unknown key %s", lineno, key);
            hard_error = 1;
            break;
        case K_TYP:
            got_typ = 1;
            typ_ok = (strlen(val) == 3 &&
                      (val[0] == 'A' || val[0] == 'a') &&
                      (val[1] == 'O' || val[1] == 'o') &&
                      (val[2] == 'M' || val[2] == 'm'));
            break;
        case K_FILENAME:
            got_filename = 1;
            if (val[0] == '\0') {
                diag_add(diag, diag_cap, "line %d: empty FileName", lineno);
                hard_error = 1;
            } else {
                snprintf(def->file_name, sizeof(def->file_name), "%s", val);
            }
            break;
        case K_OBJNR:
        case K_FILE_START: case K_FILE_END: case K_ALIGN:
        case K_REFW: case K_VIDMEM: case K_DGROUP:
        case K_MOVE_START: case K_MOVE_END:
        case K_IDLE_START: case K_IDLE_END:
        case K_SP_START: case K_SP_END:
            if (parse_int(val, &n) != 0) {
                diag_add(diag, diag_cap, "line %d: key %s not an integer: %s",
                         lineno, key, val);
                hard_error = 1;
                break;
            }
            switch (kk) {
            case K_OBJNR:      def->obj_nr = (int)n; got_objnr = 1; break;
            case K_FILE_START: def->file_anim_start = (int)n; break;
            case K_FILE_END:   def->file_anim_end = (int)n; break;
            case K_ALIGN:      def->correct_align_to_null = (int)n; break;
            case K_REFW:       def->reference_spr_width = (int)n; break;
            case K_VIDMEM:     def->use_video_mem = (int)n; break;
            case K_DGROUP:
                def->detail_group_raw = (int)n;
                if (n < 0 || n > 4)
                    diag_add(diag, diag_cap,
                             "warning: line %d: DetailGroup %ld outside 0..4",
                             lineno, n);
                break;
            case K_MOVE_START: def->move[idx].start = (int)n;
                               def->move[idx].set = 1; sm[idx].gotS = 1; break;
            case K_MOVE_END:   def->move[idx].end = (int)n;
                               def->move[idx].set = 1; sm[idx].gotE = 1; break;
            case K_IDLE_START: def->idle[idx].start = (int)n;
                               def->idle[idx].set = 1; si[idx].gotS = 1; break;
            case K_IDLE_END:   def->idle[idx].end = (int)n;
                               def->idle[idx].set = 1; si[idx].gotE = 1; break;
            case K_SP_START: {
                fa_aom_range *r = idx == 0 ? &def->attack
                               : idx == 1 ? &def->freeze : &def->ko;
                r->start = (int)n; r->set = 1; sp[idx].gotS = 1;
                break;
            }
            case K_SP_END: {
                fa_aom_range *r = idx == 0 ? &def->attack
                               : idx == 1 ? &def->freeze : &def->ko;
                r->end = (int)n; r->set = 1; sp[idx].gotE = 1;
                break;
            }
            default: break;
            }
            break;
        }
    }

    /* SCRIPT_TYP gate (RRR-21 is_aom) */
    if (!got_typ || !typ_ok) {
        diag_add(diag, diag_cap, "not an AOM script (SCRIPT_TYP missing or != AOM)");
        return -1;
    }
    /* required keys - the original raises severity-9 and drops the script */
    if (!got_objnr) {
        diag_add(diag, diag_cap, "missing required key ObjNr (Token:ObjNr)");
        hard_error = 1;
    }
    if (!got_filename || def->file_name[0] == '\0') {
        diag_add(diag, diag_cap, "missing required key FileName (Token:FileName)");
        hard_error = 1;
    }

    /* range order, only when both keys were given (RRR-21) */
    for (int d = 0; d < FA_DIR_COUNT; d++) {
        if (sm[d].gotS && sm[d].gotE && def->move[d].end < def->move[d].start) {
            diag_add(diag, diag_cap, "range StartAnim%s(%d) > EndAnim%s(%d)",
                     DIR_NAME[d], def->move[d].start, DIR_NAME[d], def->move[d].end);
            hard_error = 1;
        }
        if (si[d].gotS && si[d].gotE && def->idle[d].end < def->idle[d].start) {
            diag_add(diag, diag_cap, "range StartAnimIdle%s(%d) > EndAnimIdle%s(%d)",
                     DIR_NAME[d], def->idle[d].start, DIR_NAME[d], def->idle[d].end);
            hard_error = 1;
        }
    }
    {
        const char *nm[3] = { "Attack", "Freeze", "KO" };
        fa_aom_range *rr[3] = { &def->attack, &def->freeze, &def->ko };
        for (int k = 0; k < 3; k++)
            if (sp[k].gotS && sp[k].gotE && rr[k]->end < rr[k]->start) {
                diag_add(diag, diag_cap, "range StartAnim%s(%d) > EndAnim%s(%d)",
                         nm[k], rr[k]->start, nm[k], rr[k]->end);
                hard_error = 1;
            }
    }
    return hard_error ? -1 : 0;
}

int fa_aom_parse_file(fa_aom_def *def, const char *path,
                      char *diag, size_t diag_cap)
{
    if (diag && diag_cap)
        diag[0] = '\0';
    FILE *f = fopen(path, "rb");
    if (!f) {
        diag_add(diag, diag_cap, "cannot open %s", path ? path : "(null)");
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); diag_add(diag, diag_cap, "out of memory"); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    int rc = fa_aom_parse(def, buf, got, diag, diag_cap);
    free(buf);
    return rc;
}

fa_aom_detail_group fa_aom_def_group(const fa_aom_def *def)
{
    int g = def ? def->detail_group_raw : 4;
    if (g < 0 || g >= FA_AOM_DG_COUNT)
        return FA_AOM_DG_DETAIL;
    return (fa_aom_detail_group)g;
}

int fa_aom_def_basename(const fa_aom_def *def, char *buf, size_t cap)
{
    if (!def || !buf || cap == 0)
        return -1;
    const char *s = def->file_name;
    const char *base = s;
    for (const char *c = s; *c; c++)
        if (*c == '/' || *c == '\\')
            base = c + 1;
    int n = snprintf(buf, cap, "%s", base);
    return n;
}

/* --- registry ------------------------------------------------- */

void fa_aom_registry_init(fa_aom_registry *r)
{
    if (r)
        memset(r, 0, sizeof(*r));
}

int fa_aom_registry_set(fa_aom_registry *r, fa_aom_detail_group g,
                        const fa_aom_hooks *h, void *ctx)
{
    if (!r || g < 0 || g >= FA_AOM_DG_COUNT)
        return -1;
    if (h)
        r->hooks[g] = *h;
    else
        memset(&r->hooks[g], 0, sizeof(r->hooks[g]));
    r->ctx[g] = ctx;
    return 0;
}

/* --- runtime ------------------------------------------------- */

int fa_aom_obj_init(fa_aom_obj *o, const fa_aom_def *def,
                    fa_aom_registry *reg, void *user)
{
    if (!o || !def)
        return -1;
    memset(o, 0, sizeof(*o));
    o->def = def;
    o->reg = reg;
    o->user = user;
    o->facing = FA_DIR_LEFT;
    o->moving = 0;
    o->special = FA_AOM_SP_NONE;
    o->frame_period = FA_AOM_TICKS_PER_FRAME;
    o->act = FA_AOM_ACT_NONE;
    /* resolve the opening state so fa_aom_frame is valid before the first tick,
     * without firing a hook or stepping the frame */
    fa_aom_advance(o);
    return 0;
}

static const fa_aom_hooks *hooks_for(const fa_aom_obj *o, fa_aom_detail_group *g_out)
{
    fa_aom_detail_group g = fa_aom_def_group(o->def);
    if (g_out)
        *g_out = g;
    if (!o->reg)
        return NULL;
    return &o->reg->hooks[g];
}

void fa_aom_obj_spawn(fa_aom_obj *o)
{
    if (!o)
        return;
    fa_aom_detail_group g;
    const fa_aom_hooks *h = hooks_for(o, &g);
    if (h && h->on_spawn)
        h->on_spawn(o, o->reg->ctx[g]);
}

void fa_aom_obj_player_touch(fa_aom_obj *o)
{
    if (!o)
        return;
    fa_aom_detail_group g;
    const fa_aom_hooks *h = hooks_for(o, &g);
    if (h && h->on_player_touch)
        h->on_player_touch(o, o->reg->ctx[g]);
}

void fa_aom_set_facing(fa_aom_obj *o, fa_dir d)
{
    if (o && d >= 0 && d < FA_DIR_COUNT)
        o->facing = d;
}
void fa_aom_set_moving(fa_aom_obj *o, int moving)
{
    if (o)
        o->moving = moving ? 1 : 0;
}
void fa_aom_set_special(fa_aom_obj *o, fa_aom_special sp)
{
    if (o && sp >= FA_AOM_SP_NONE && sp <= FA_AOM_SP_KO)
        o->special = sp;
}
void fa_aom_set_frame_period(fa_aom_obj *o, unsigned ticks)
{
    if (o)
        o->frame_period = ticks ? ticks : FA_AOM_TICKS_PER_FRAME;
}

/* Pick a cardinal component for a diagonal, preferring the horizontal one
 * (shipped content only ever sets Left/Right). Returns -1 if neither the
 * requested diagonal nor a usable cardinal is set. */
static int diagonal_fallback(const fa_aom_def *d, fa_dir dir)
{
    switch (dir) {
    case FA_DIR_LEFT_UP:
    case FA_DIR_LEFT_DOWN:
        if (d->move[FA_DIR_LEFT].set)  return FA_DIR_LEFT;
        if (d->move[dir == FA_DIR_LEFT_UP ? FA_DIR_UP : FA_DIR_DOWN].set)
            return dir == FA_DIR_LEFT_UP ? FA_DIR_UP : FA_DIR_DOWN;
        return -1;
    case FA_DIR_RIGHT_UP:
    case FA_DIR_RIGHT_DOWN:
        if (d->move[FA_DIR_RIGHT].set) return FA_DIR_RIGHT;
        if (d->move[dir == FA_DIR_RIGHT_UP ? FA_DIR_UP : FA_DIR_DOWN].set)
            return dir == FA_DIR_RIGHT_UP ? FA_DIR_UP : FA_DIR_DOWN;
        return -1;
    default:
        return -1;
    }
}

/* Resolve the range that should be playing now. */
static void resolve(const fa_aom_obj *o, fa_aom_act *act, fa_aom_range *r)
{
    const fa_aom_def *d = o->def;

    if (o->special == FA_AOM_SP_ATTACK && d->attack.set) { *act = FA_AOM_ACT_ATTACK; *r = d->attack; return; }
    if (o->special == FA_AOM_SP_FREEZE && d->freeze.set) { *act = FA_AOM_ACT_FREEZE; *r = d->freeze; return; }
    if (o->special == FA_AOM_SP_KO     && d->ko.set)     { *act = FA_AOM_ACT_KO;     *r = d->ko;     return; }

    fa_dir dir = o->facing;

    if (o->moving) {
        if (d->move[dir].set) { *act = FA_AOM_ACT_MOVE; *r = d->move[dir]; return; }
        int fb = diagonal_fallback(d, dir);
        if (fb >= 0) { *act = FA_AOM_ACT_MOVE_FALLBACK; *r = d->move[fb]; return; }
        /* nothing to walk with - fall through to idle handling */
    }

    if (d->idle[dir].set) { *act = FA_AOM_ACT_IDLE; *r = d->idle[dir]; return; }

    if (d->move[dir].set) {
        *act = FA_AOM_ACT_IDLE_STAND;
        r->start = r->end = d->move[dir].start;
        r->set = 1;
        return;
    }
    {
        int fb = diagonal_fallback(d, dir);
        if (fb >= 0) {
            *act = FA_AOM_ACT_IDLE_STAND;
            r->start = r->end = d->move[fb].start;
            r->set = 1;
            return;
        }
    }
    if (d->file_anim_end >= d->file_anim_start &&
        (d->file_anim_start != 0 || d->file_anim_end != 0)) {
        *act = FA_AOM_ACT_FILE;
        r->start = d->file_anim_start;
        r->end = d->file_anim_end;
        r->set = 1;
        return;
    }
    *act = FA_AOM_ACT_FILE;
    r->start = r->end = 0;
    r->set = 1;
}

/* Resolve the active range; on a change reset to its first frame, otherwise
 * step the frame if the period elapsed. No hooks. */
static void fa_aom_advance(fa_aom_obj *o)
{
    fa_aom_act act = FA_AOM_ACT_NONE;
    fa_aom_range r = {0, 0, 0};
    resolve(o, &act, &r);

    if (act != o->act || r.start != o->active.start || r.end != o->active.end) {
        o->act = act;
        o->active = r;
        o->frame = r.start;
        o->sub = 0;
        o->cycles = 0;
    } else {
        if (++o->sub >= o->frame_period) {
            o->sub = 0;
            if (o->active.end > o->active.start) {
                o->frame++;
                if (o->frame > o->active.end) {
                    o->frame = o->active.start;
                    o->cycles++;
                }
            } else {
                /* one-frame range: it never leaves, but count the loops so a
                 * gameplay hook can still time off it */
                o->cycles++;
            }
        }
    }
}

void fa_aom_tick(fa_aom_obj *o)
{
    if (!o || !o->def)
        return;

    fa_aom_advance(o);

    fa_aom_detail_group g;
    const fa_aom_hooks *h = hooks_for(o, &g);
    if (h && h->on_tick)
        h->on_tick(o, o->reg->ctx[g]);
}

int      fa_aom_frame(const fa_aom_obj *o)  { return o ? o->frame : 0; }
uint64_t fa_aom_cycles(const fa_aom_obj *o) { return o ? o->cycles : 0; }
fa_aom_act fa_aom_active(const fa_aom_obj *o) { return o ? o->act : FA_AOM_ACT_NONE; }

fa_aom_detail_group fa_aom_group(const fa_aom_obj *o)
{
    return o ? fa_aom_def_group(o->def) : FA_AOM_DG_DETAIL;
}

const char *fa_aom_active_name(const fa_aom_obj *o)
{
    if (!o)
        return "none";
    switch (o->act) {
    case FA_AOM_ACT_NONE:          return "none";
    case FA_AOM_ACT_MOVE:          return "move";
    case FA_AOM_ACT_MOVE_FALLBACK: return "move-fallback";
    case FA_AOM_ACT_IDLE:          return "idle";
    case FA_AOM_ACT_IDLE_STAND:    return "idle-stand";
    case FA_AOM_ACT_FILE:          return "file";
    case FA_AOM_ACT_ATTACK:        return "attack";
    case FA_AOM_ACT_FREEZE:        return "freeze";
    case FA_AOM_ACT_KO:            return "ko";
    default:                       return "?";
    }
}
