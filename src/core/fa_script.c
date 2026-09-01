/*
 * fa_script.c - Lua 4.0 assignment-subset evaluator (RRR-36)
 * See include/fa/fa_script.h for the model, the RRR-11 basis, and the
 * decision record (purpose-built, not a vendored interpreter).
 */
#include "fa/fa_script.h"

#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH     32
#define MAX_STR       (1u << 20)
#define MAX_ENTRIES   8192
#define MAX_NAME      63

/* --- values -------------------------------------------------------- */

typedef struct sv {
    fa_script_type t;
    double         num;
    char          *str;     /* STRING: owned, NUL-terminated, slen bytes */
    size_t         slen;
    struct tab    *tab;     /* TABLE: owned */
} sv;

typedef struct tent { sv key; sv val; } tent;

typedef struct tab {
    tent  *e;
    size_t n, cap;
    long   narr;            /* running positional index for a constructor */
} tab;

static sv sv_nil(void)
{
    sv v; v.t = FA_SCRIPT_NIL; v.num = 0; v.str = NULL; v.slen = 0; v.tab = NULL;
    return v;
}
static sv sv_num(double d)
{
    sv v = sv_nil(); v.t = FA_SCRIPT_NUMBER; v.num = d; return v;
}

static void  tab_free(tab *t);
static tab  *tab_dup(const tab *t);

static void sv_free(sv *v)
{
    if (v->t == FA_SCRIPT_STRING) free(v->str);
    else if (v->t == FA_SCRIPT_TABLE) tab_free(v->tab);
    *v = sv_nil();
}

static sv sv_strn(const char *s, size_t n)
{
    sv v = sv_nil();
    v.t = FA_SCRIPT_STRING;
    v.str = (char *)malloc(n + 1);
    if (!v.str) { fputs("fa_script: out of memory\n", stderr); abort(); }
    memcpy(v.str, s, n);
    v.str[n] = '\0';
    v.slen = n;
    return v;
}

static sv sv_dup(const sv *src)
{
    switch (src->t) {
    case FA_SCRIPT_NIL:    return sv_nil();
    case FA_SCRIPT_NUMBER: return sv_num(src->num);
    case FA_SCRIPT_STRING: return sv_strn(src->str, src->slen);
    case FA_SCRIPT_TABLE:  { sv v = sv_nil(); v.t = FA_SCRIPT_TABLE;
                             v.tab = tab_dup(src->tab); return v; }
    }
    return sv_nil();
}

/* --- table ------------------------------------------------------- */

static tab *tab_new(void)
{
    tab *t = (tab *)calloc(1, sizeof(*t));
    if (!t) { fputs("fa_script: out of memory\n", stderr); abort(); }
    return t;
}

static void tab_free(tab *t)
{
    size_t i;
    if (!t) return;
    for (i = 0; i < t->n; i++) { sv_free(&t->e[i].key); sv_free(&t->e[i].val); }
    free(t->e);
    free(t);
}

static int key_eq(const sv *a, const sv *b)
{
    if (a->t != b->t) return 0;
    if (a->t == FA_SCRIPT_NUMBER) return a->num == b->num;
    if (a->t == FA_SCRIPT_STRING)
        return a->slen == b->slen && memcmp(a->str, b->str, a->slen) == 0;
    return 0;
}

static sv *tab_get(const tab *t, const sv *key)
{
    size_t i;
    for (i = 0; i < t->n; i++)
        if (key_eq(&t->e[i].key, key)) return &t->e[i].val;
    return NULL;
}

/* Store key -> val, taking ownership of both. A nil val removes the entry.
 * Returns 0, or -1 if the table is already at MAX_ENTRIES. */
static int tab_set(tab *t, sv key, sv val)
{
    size_t i;
    for (i = 0; i < t->n; i++) {
        if (key_eq(&t->e[i].key, &key)) {
            sv_free(&t->e[i].val);
            sv_free(&key);                 /* keep the stored key */
            if (val.t == FA_SCRIPT_NIL) {
                sv_free(&t->e[i].key);
                t->e[i] = t->e[t->n - 1];
                t->n--;
            } else {
                t->e[i].val = val;
            }
            return 0;
        }
    }
    if (val.t == FA_SCRIPT_NIL) { sv_free(&key); return 0; }
    if (t->n == t->cap) {
        size_t nc = t->cap ? t->cap * 2 : 8;
        tent *ne;
        if (nc > MAX_ENTRIES) nc = MAX_ENTRIES;
        if (t->n >= nc) { sv_free(&key); sv_free(&val); return -1; }
        ne = (tent *)realloc(t->e, nc * sizeof(*ne));
        if (!ne) { fputs("fa_script: out of memory\n", stderr); abort(); }
        t->e = ne; t->cap = nc;
    }
    t->e[t->n].key = key;
    t->e[t->n].val = val;
    t->n++;
    return 0;
}

static tab *tab_dup(const tab *t)
{
    tab *nt = tab_new();
    size_t i;
    nt->narr = t->narr;
    for (i = 0; i < t->n; i++)
        tab_set(nt, sv_dup(&t->e[i].key), sv_dup(&t->e[i].val));
    return nt;
}

/* --- evaluator state ------------------------------------------- */

struct fa_script {
    tab *G;
    char err[256];
};

/* --- lexer ---------------------------------------------------- */

enum {
    TK_EOF, TK_NAME, TK_NUMBER, TK_STRING,
    TK_ASSIGN, TK_LBRACE, TK_RBRACE, TK_LBRK, TK_RBRK,
    TK_LPAREN, TK_RPAREN, TK_COMMA, TK_SEMI, TK_MINUS, TK_PLUS
};

typedef struct {
    int    kind;
    int    line;
    double num;
    char  *str;             /* STRING / NAME: owned */
    size_t slen;
} token;

typedef struct {
    fa_script  *S;
    const char *p, *end;
    const char *chunk;
    int         line;
    token       cur;
    token       ahead;
    int         have_ahead;
    jmp_buf    *jb;
} lexer;

static const char *const RESERVED[] = {
    "and", "break", "do", "else", "elseif", "end", "for", "function",
    "global", "if", "in", "local", "not", "or", "repeat", "return",
    "then", "until", "while"
};

static int is_reserved(const char *s)
{
    size_t i;
    for (i = 0; i < sizeof(RESERVED) / sizeof(RESERVED[0]); i++)
        if (strcmp(s, RESERVED[i]) == 0) return 1;
    return 0;
}

static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_alpha(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static int is_alnum(int c) { return is_alpha(c) || is_digit(c); }

static void lx_errv(lexer *lx, int line, const char *fmt, va_list ap)
{
    int k = snprintf(lx->S->err, sizeof(lx->S->err), "%s:%d: ",
                     lx->chunk ? lx->chunk : "?", line);
    if (k < 0) k = 0;
    if ((size_t)k < sizeof(lx->S->err))
        vsnprintf(lx->S->err + k, sizeof(lx->S->err) - (size_t)k, fmt, ap);
    longjmp(*lx->jb, 1);
}

static void lx_err(lexer *lx, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); lx_errv(lx, lx->line, fmt, ap); va_end(ap);
}

static void tok_free(token *t)
{
    if (t->kind == TK_STRING || t->kind == TK_NAME) free(t->str);
    t->str = NULL;
}

static void scan_string(lexer *lx, token *out)
{
    int quote = *lx->p++;
    size_t cap = 32, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { fputs("fa_script: out of memory\n", stderr); abort(); }

    for (;;) {
        int c;
        if (lx->p >= lx->end) { free(buf); lx_err(lx, "unterminated string"); }
        c = (unsigned char)*lx->p;
        if (c == quote) { lx->p++; break; }
        if (c == '\n') { free(buf); lx_err(lx, "unterminated string"); }

        if (c == '\\') {
            lx->p++;
            if (lx->p >= lx->end) {
                free(buf); lx_err(lx, "unterminated string");
            }
            {
                int e = (unsigned char)*lx->p;
                int out_c = -1;
                switch (e) {
                case 'a': out_c = 7;  lx->p++; break;
                case 'b': out_c = 8;  lx->p++; break;
                case 'f': out_c = 12; lx->p++; break;
                case 'n': out_c = 10; lx->p++; break;
                case 'r': out_c = 13; lx->p++; break;
                case 't': out_c = 9;  lx->p++; break;
                case 'v': out_c = 11; lx->p++; break;
                case '\n': out_c = 10; lx->p++; lx->line++; break;
                default:
                    if (is_digit(e)) {
                        int v = 0, i = 0;
                        while (i < 3 && lx->p < lx->end &&
                               is_digit((unsigned char)*lx->p)) {
                            v = v * 10 + (*lx->p - '0');
                            lx->p++; i++;
                        }
                        if (v > 255) {
                            free(buf);
                            lx_err(lx, "decimal escape \\%d is out of range", v);
                        }
                        out_c = v;
                    } else {
                        out_c = e;      /* \\ \" \' and any other char verbatim */
                        lx->p++;
                    }
                    break;
                }
                c = out_c;
            }
        } else {
            lx->p++;
        }

        if (len + 1 > MAX_STR) { free(buf); lx_err(lx, "string literal too long"); }
        if (len == cap) {
            char *nb = (char *)realloc(buf, cap *= 2);
            if (!nb) { fputs("fa_script: out of memory\n", stderr); abort(); }
            buf = nb;
        }
        buf[len++] = (char)c;
    }

    if (len == cap) {
        char *nb = (char *)realloc(buf, cap + 1);
        if (!nb) { fputs("fa_script: out of memory\n", stderr); abort(); }
        buf = nb;
    }
    buf[len] = '\0';
    out->kind = TK_STRING;
    out->str = buf;
    out->slen = len;
}

static void scan_number(lexer *lx, token *out)
{
    const char *s = lx->p;
    char tmp[64];
    size_t k;
    char *endp;

    while (lx->p < lx->end && is_digit((unsigned char)*lx->p)) lx->p++;
    if (lx->p < lx->end && *lx->p == '.') {
        lx->p++;
        while (lx->p < lx->end && is_digit((unsigned char)*lx->p)) lx->p++;
    }
    if (lx->p < lx->end && (*lx->p == 'e' || *lx->p == 'E')) {
        lx->p++;
        if (lx->p < lx->end && (*lx->p == '+' || *lx->p == '-')) lx->p++;
        if (lx->p >= lx->end || !is_digit((unsigned char)*lx->p))
            lx_err(lx, "malformed number (bad exponent)");
        while (lx->p < lx->end && is_digit((unsigned char)*lx->p)) lx->p++;
    }
    /* a lone '.' is not a number and is handled by the caller */

    k = (size_t)(lx->p - s);
    if (k == 0 || k >= sizeof(tmp)) lx_err(lx, "malformed number");
    memcpy(tmp, s, k); tmp[k] = '\0';
    out->kind = TK_NUMBER;
    out->num = strtod(tmp, &endp);
    if (endp != tmp + k) lx_err(lx, "malformed number '%s'", tmp);
}

static void lx_scan(lexer *lx, token *out)
{
    tok_free(out);
    out->num = 0; out->slen = 0;

    for (;;) {
        if (lx->p >= lx->end) { out->kind = TK_EOF; out->line = lx->line; return; }
        {
            int c = (unsigned char)*lx->p;
            if (c == '\n') { lx->line++; lx->p++; continue; }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                lx->p++; continue;
            }
            if (c == '-' && lx->p + 1 < lx->end && lx->p[1] == '-') {
                lx->p += 2;              /* line comment (Lua 4.0) */
                while (lx->p < lx->end && *lx->p != '\n') lx->p++;
                continue;
            }
            break;
        }
    }

    out->line = lx->line;
    {
        int c = (unsigned char)*lx->p;

        if (is_alpha(c)) {
            const char *s = lx->p;
            size_t k;
            while (lx->p < lx->end && is_alnum((unsigned char)*lx->p)) lx->p++;
            k = (size_t)(lx->p - s);
            if (k > MAX_NAME) lx_err(lx, "identifier too long");
            out->kind = TK_NAME;
            out->str = (char *)malloc(k + 1);
            if (!out->str) { fputs("fa_script: out of memory\n", stderr); abort(); }
            memcpy(out->str, s, k); out->str[k] = '\0';
            out->slen = k;
            return;
        }
        if (is_digit(c) || (c == '.' && lx->p + 1 < lx->end &&
                            is_digit((unsigned char)lx->p[1]))) {
            scan_number(lx, out);
            return;
        }
        if (c == '"' || c == '\'') { scan_string(lx, out); return; }

        lx->p++;
        switch (c) {
        case '=': out->kind = TK_ASSIGN; return;
        case '{': out->kind = TK_LBRACE; return;
        case '}': out->kind = TK_RBRACE; return;
        case '[': out->kind = TK_LBRK;   return;
        case ']': out->kind = TK_RBRK;   return;
        case '(': out->kind = TK_LPAREN; return;
        case ')': out->kind = TK_RPAREN; return;
        case ',': out->kind = TK_COMMA;  return;
        case ';': out->kind = TK_SEMI;   return;
        case '-': out->kind = TK_MINUS;  return;
        case '+': out->kind = TK_PLUS;   return;
        }
        if (c == '.')
            lx_err(lx, "unexpected '.' (the .jrs subset has no field access)");
        lx_err(lx, "unexpected character '%c' (0x%02x)", c, c);
    }
}

static void lx_init(lexer *lx, fa_script *S, const char *src, size_t len,
                    const char *chunk, jmp_buf *jb)
{
    memset(lx, 0, sizeof(*lx));
    lx->S = S;
    lx->p = src;
    lx->end = src + len;
    lx->chunk = chunk;
    lx->line = 1;
    lx->jb = jb;
    lx->cur.kind = TK_EOF;
    lx->ahead.kind = TK_EOF;
}

static void lx_free(lexer *lx)
{
    tok_free(&lx->cur);
    tok_free(&lx->ahead);
}

static void lx_next(lexer *lx)
{
    if (lx->have_ahead) {
        tok_free(&lx->cur);
        lx->cur = lx->ahead;
        lx->ahead.str = NULL;
        lx->ahead.kind = TK_EOF;
        lx->have_ahead = 0;
    } else {
        lx_scan(lx, &lx->cur);
    }
}

static token *lx_peek(lexer *lx)
{
    if (!lx->have_ahead) { lx_scan(lx, &lx->ahead); lx->have_ahead = 1; }
    return &lx->ahead;
}

/* --- parser / evaluator --------------------------------------- */

typedef struct {
    fa_script *S;
    lexer      lx;
    jmp_buf    jb;
    int        depth;
} parser;

static const char *type_name(fa_script_type t)
{
    switch (t) {
    case FA_SCRIPT_NIL:    return "nil";
    case FA_SCRIPT_NUMBER: return "number";
    case FA_SCRIPT_STRING: return "string";
    case FA_SCRIPT_TABLE:  return "table";
    }
    return "?";
}

static sv parse_exp(parser *P);

static sv global_ref(parser *P, const char *name)
{
    sv key = { FA_SCRIPT_STRING, 0, (char *)name, strlen(name), NULL };
    sv *g = tab_get(P->S->G, &key);
    return g ? sv_dup(g) : sv_nil();
}

static sv parse_table(parser *P)
{
    lexer *lx = &P->lx;
    sv tv = sv_nil();

    if (++P->depth > MAX_DEPTH) lx_err(lx, "table nesting too deep");
    tv.t = FA_SCRIPT_TABLE;
    tv.tab = tab_new();

    lx_next(lx);                             /* consume '{' */
    while (lx->cur.kind != TK_RBRACE) {
        if (lx->cur.kind == TK_EOF)
            lx_err(lx, "unterminated table constructor");

        if (lx->cur.kind == TK_LBRK) {
            sv k, val;
            lx_next(lx);
            k = parse_exp(P);
            if (lx->cur.kind != TK_RBRK) lx_err(lx, "expected ']' in table key");
            lx_next(lx);
            if (lx->cur.kind != TK_ASSIGN) lx_err(lx, "expected '=' after ']'");
            lx_next(lx);
            val = parse_exp(P);
            if (k.t == FA_SCRIPT_NIL) lx_err(lx, "table index is nil");
            if (tab_set(tv.tab, k, val) != 0)
                lx_err(lx, "table constructor has too many fields");
        } else if (lx->cur.kind == TK_NAME && !is_reserved(lx->cur.str) &&
                   lx_peek(lx)->kind == TK_ASSIGN) {
            sv k = sv_strn(lx->cur.str, lx->cur.slen);
            sv val;
            lx_next(lx);                     /* NAME */
            lx_next(lx);                     /* '='  */
            val = parse_exp(P);
            if (tab_set(tv.tab, k, val) != 0)
                lx_err(lx, "table constructor has too many fields");
        } else {
            sv val = parse_exp(P);
            long idx = ++tv.tab->narr;
            if (val.t == FA_SCRIPT_NIL) {
                sv_free(&val);              /* a hole; index is still spent */
            } else if (tab_set(tv.tab, sv_num((double)idx), val) != 0) {
                lx_err(lx, "table constructor has too many fields");
            }
        }

        if (lx->cur.kind == TK_COMMA || lx->cur.kind == TK_SEMI) {
            lx_next(lx);
            continue;
        }
        break;
    }
    if (lx->cur.kind != TK_RBRACE)
        lx_err(lx, "expected '}' to close the table constructor");
    lx_next(lx);
    P->depth--;
    return tv;
}

static sv parse_operand(parser *P)
{
    lexer *lx = &P->lx;

    switch (lx->cur.kind) {
    case TK_NUMBER: { sv v = sv_num(lx->cur.num); lx_next(lx); return v; }
    case TK_STRING: { sv v = sv_strn(lx->cur.str, lx->cur.slen);
                      lx_next(lx); return v; }
    case TK_LBRACE: return parse_table(P);
    case TK_LPAREN: {
        sv v;
        lx_next(lx);
        v = parse_exp(P);
        if (lx->cur.kind != TK_RPAREN) lx_err(lx, "expected ')'");
        lx_next(lx);
        return v;
    }
    case TK_NAME: {
        sv v;
        if (is_reserved(lx->cur.str))
            lx_err(lx, "'%s' is a Lua keyword; the .jrs subset is "
                       "NAME = value only", lx->cur.str);
        if (strcmp(lx->cur.str, "nil") == 0) { lx_next(lx); return sv_nil(); }
        v = global_ref(P, lx->cur.str);
        lx_next(lx);
        return v;
    }
    default:
        lx_err(lx, "expected a value (number, string, table, name or '(' )");
    }
    return sv_nil();                         /* unreachable */
}

static sv parse_exp(parser *P)
{
    lexer *lx = &P->lx;
    int neg = 0, sign_seen = 0;
    sv v;

    while (lx->cur.kind == TK_MINUS || lx->cur.kind == TK_PLUS) {
        if (lx->cur.kind == TK_MINUS) neg = !neg;
        sign_seen = 1;
        lx_next(lx);
    }
    v = parse_operand(P);
    if (sign_seen) {
        if (v.t != FA_SCRIPT_NUMBER) {
            fa_script_type had = v.t;
            sv_free(&v);
            lx_err(lx, "cannot apply unary '-' / '+' to a %s", type_name(had));
        }
        if (neg) v.num = -v.num;
    }
    return v;
}

static void parse_chunk(parser *P)
{
    lexer *lx = &P->lx;

    lx_next(lx);
    while (lx->cur.kind != TK_EOF) {
        char name[MAX_NAME + 1];
        sv value;

        if (lx->cur.kind == TK_SEMI) { lx_next(lx); continue; }
        if (lx->cur.kind != TK_NAME)
            lx_err(lx, "expected a name to start a statement "
                       "(the .jrs subset is one 'NAME = value' per line)");
        if (is_reserved(lx->cur.str))
            lx_err(lx, "'%s' is a Lua keyword; not in the .jrs subset",
                   lx->cur.str);
        if (strcmp(lx->cur.str, "nil") == 0)
            lx_err(lx, "cannot assign to 'nil'");

        memcpy(name, lx->cur.str, lx->cur.slen + 1);
        lx_next(lx);
        if (lx->cur.kind != TK_ASSIGN)
            lx_err(lx, "expected '=' after '%s'", name);
        lx_next(lx);

        value = parse_exp(P);
        if (tab_set(P->S->G, sv_strn(name, strlen(name)), value) != 0)
            lx_err(lx, "too many globals");

        if (lx->cur.kind == TK_SEMI) lx_next(lx);
    }
}

/* --- public API ---------------------------------------------- */

fa_script *fa_script_new(void)
{
    fa_script *s = (fa_script *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->G = tab_new();
    return s;
}

void fa_script_free(fa_script *s)
{
    if (!s) return;
    tab_free(s->G);
    free(s);
}

int fa_script_do(fa_script *s, const char *src, size_t len,
                 const char *chunkname)
{
    parser P;
    if (!s || !src) return -1;
    s->err[0] = '\0';
    P.S = s;
    P.depth = 0;
    lx_init(&P.lx, s, src, len, chunkname ? chunkname : "?", &P.jb);

    if (setjmp(P.jb)) { lx_free(&P.lx); return -1; }
    parse_chunk(&P);
    lx_free(&P.lx);
    return 0;
}

int fa_script_do_file(fa_script *s, const char *path)
{
    FILE *f;
    long size;
    char *buf;
    size_t got;
    int rc;

    if (!s || !path) return -1;
    f = fopen(path, "rb");
    if (!f) {
        snprintf(s->err, sizeof(s->err), "%s: cannot open", path);
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        snprintf(s->err, sizeof(s->err), "%s: cannot size", path);
        fclose(f);
        return -1;
    }
    if (size > (1L << 24)) {
        snprintf(s->err, sizeof(s->err), "%s: script file too large", path);
        fclose(f);
        return -1;
    }
    buf = (char *)malloc((size_t)size + 1);
    if (!buf) { fclose(f); return -1; }
    got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';

    rc = fa_script_do(s, buf, got, path);
    free(buf);
    return rc;
}

const char *fa_script_last_error(const fa_script *s)
{
    return (s && s->err[0]) ? s->err : "";
}

static const sv *find_global(const fa_script *s, const char *name)
{
    sv key;
    if (!s || !name) return NULL;
    key.t = FA_SCRIPT_STRING; key.num = 0;
    key.str = (char *)name; key.slen = strlen(name); key.tab = NULL;
    return tab_get(s->G, &key);
}

fa_script_type fa_script_type_of(const fa_script *s, const char *name)
{
    const sv *v = find_global(s, name);
    return v ? v->t : FA_SCRIPT_NIL;
}

int fa_script_number(const fa_script *s, const char *name, double *out)
{
    const sv *v = find_global(s, name);
    if (!v || v->t != FA_SCRIPT_NUMBER) return -1;
    if (out) *out = v->num;
    return 0;
}

int fa_script_int(const fa_script *s, const char *name, long *out)
{
    const sv *v = find_global(s, name);
    double d;
    long l;
    if (!v || v->t != FA_SCRIPT_NUMBER) return -1;
    d = v->num;
    if (d != d) return -1;                              /* NaN */
    if (d < (double)LONG_MIN || d > (double)LONG_MAX) return -1;
    l = (long)d;
    if ((double)l != d) return -1;                      /* had a fraction */
    if (out) *out = l;
    return 0;
}

const char *fa_script_string(const fa_script *s, const char *name,
                             size_t *len_out)
{
    const sv *v = find_global(s, name);
    if (!v || v->t != FA_SCRIPT_STRING) return NULL;
    if (len_out) *len_out = v->slen;
    return v->str;
}

/* --- repr ---------------------------------------------------- */

typedef struct { char *b; size_t n, cap; } strbuf;

static void sb_putc(strbuf *sb, char c)
{
    if (sb->n + 1 >= sb->cap) {
        size_t nc = sb->cap ? sb->cap * 2 : 64;
        char *nb = (char *)realloc(sb->b, nc);
        if (!nb) { fputs("fa_script: out of memory\n", stderr); abort(); }
        sb->b = nb; sb->cap = nc;
    }
    sb->b[sb->n++] = c;
}
static void sb_puts(strbuf *sb, const char *s) { while (*s) sb_putc(sb, *s++); }

static void sb_putnum(strbuf *sb, double d)
{
    char t[32];
    if (d >= -9.007199254740992e15 && d <= 9.007199254740992e15 &&
        (double)(long long)d == d) {
        snprintf(t, sizeof(t), "%lld", (long long)d);
    } else {
        snprintf(t, sizeof(t), "%.14g", d);
    }
    sb_puts(sb, t);
}

static void sb_putstr(strbuf *sb, const char *s, size_t n)
{
    size_t i;
    sb_putc(sb, '"');
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  sb_puts(sb, "\\\""); break;
        case '\\': sb_puts(sb, "\\\\"); break;
        case '\n': sb_puts(sb, "\\n");  break;
        case '\r': sb_puts(sb, "\\r");  break;
        case '\t': sb_puts(sb, "\\t");  break;
        default:
            if (c < 0x20 || c == 0x7f) {
                char e[5];
                snprintf(e, sizeof(e), "\\%03u", c);
                sb_puts(sb, e);
            } else {
                sb_putc(sb, (char)c);
            }
        }
    }
    sb_putc(sb, '"');
}

static int ident_ok(const char *s, size_t n)
{
    size_t i;
    if (n == 0 || !is_alpha((unsigned char)s[0])) return 0;
    for (i = 1; i < n; i++) if (!is_alnum((unsigned char)s[i])) return 0;
    return !is_reserved(s);
}

static void sb_putval(strbuf *sb, const sv *v, int depth);

static void sb_puttable(strbuf *sb, const tab *t, int depth)
{
    size_t i;
    long idx = 0;
    int first = 1;

    sb_putc(sb, '{');
    /* contiguous positional part 1..k */
    for (;;) {
        sv key = sv_num((double)(idx + 1));
        sv *val = tab_get(t, &key);
        if (!val) break;
        if (!first) sb_puts(sb, ", ");
        first = 0;
        sb_putval(sb, val, depth + 1);
        idx++;
    }
    /* everything else, in insertion order */
    for (i = 0; i < t->n; i++) {
        const sv *k = &t->e[i].key;
        if (k->t == FA_SCRIPT_NUMBER && k->num >= 1.0 &&
            k->num <= (double)idx && (double)(long)k->num == k->num)
            continue;
        if (!first) sb_puts(sb, ", ");
        first = 0;
        if (k->t == FA_SCRIPT_STRING && ident_ok(k->str, k->slen)) {
            sb_puts(sb, k->str);
            sb_putc(sb, '=');
        } else {
            sb_putc(sb, '[');
            sb_putval(sb, k, depth + 1);
            sb_putc(sb, ']');
            sb_putc(sb, '=');
        }
        sb_putval(sb, &t->e[i].val, depth + 1);
    }
    sb_putc(sb, '}');
}

static void sb_putval(strbuf *sb, const sv *v, int depth)
{
    if (depth > MAX_DEPTH) { sb_puts(sb, "..."); return; }
    switch (v->t) {
    case FA_SCRIPT_NIL:    sb_puts(sb, "nil"); break;
    case FA_SCRIPT_NUMBER: sb_putnum(sb, v->num); break;
    case FA_SCRIPT_STRING: sb_putstr(sb, v->str, v->slen); break;
    case FA_SCRIPT_TABLE:  sb_puttable(sb, v->tab, depth); break;
    }
}

int fa_script_repr(const fa_script *s, const char *name, char *buf, size_t cap)
{
    const sv *v = find_global(s, name);
    strbuf sb = { NULL, 0, 0 };
    int ret;

    if (!v) return -1;
    sb_putval(&sb, v, 0);
    sb_putc(&sb, '\0');
    ret = (int)(sb.n - 1);
    if (buf && cap) {
        size_t copy = (sb.n < cap) ? sb.n : cap;
        memcpy(buf, sb.b, copy);
        buf[copy - 1] = '\0';
    }
    free(sb.b);
    return ret;
}
