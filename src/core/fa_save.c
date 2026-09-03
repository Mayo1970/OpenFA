/*
 * fa_save.c - Option.ini / Highscore1-4.dat text formats. See fa_save.h.
 */
#include "fa/fa_save.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Line reader: copies the line at *pos (excluding CR/LF) into dst, advances
 * *pos past the terminator. Returns the line length, or -1 at end of input. */
static int read_line(const unsigned char *buf, size_t len, size_t *pos,
                     char *dst, size_t dstcap)
{
    size_t start, i, n;
    if (*pos >= len) return -1;
    start = i = *pos;
    while (i < len && buf[i] != '\r' && buf[i] != '\n') i++;
    n = i - start;
    if (dst && dstcap) {
        size_t c = n < dstcap - 1 ? n : dstcap - 1;
        memcpy(dst, buf + start, c);
        dst[c] = 0;
    }
    if (i < len && buf[i] == '\r') i++;
    if (i < len && buf[i] == '\n') i++;
    *pos = i;
    return (int)n;
}

/* ---- Option.ini ---------------------------------------------------- */

int fa_opt_parse(const unsigned char *buf, size_t len, fa_opt_file *out)
{
    size_t pos = 0;
    char line[32];
    for (int i = 0; i < FA_OPT_LINES; i++) {
        if (read_line(buf, len, &pos, line, sizeof line) < 0) return -1;
        out->line[i] = (int)strtol(line, NULL, 10);
    }
    return 0;
}

int fa_opt_write(const fa_opt_file *in, unsigned char *out, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < FA_OPT_LINES; i++) {
        char tmp[16];
        int k = sprintf(tmp, "%d", in->line[i]);
        if (i) {
            if (n + 2 > cap) return -1;
            out[n++] = '\r'; out[n++] = '\n';
        }
        if (n + (size_t)k > cap) return -1;
        memcpy(out + n, tmp, (size_t)k);
        n += (size_t)k;
    }
    return (int)n;
}

void fa_opt_default(fa_opt_file *out)
{
    static const int def[FA_OPT_LINES] = {
        203, 205, 200, 208, 30, 31, 33, 32,   /* arrows + A S F D */
        -1, -1, -1, -1, -1, -1, -1, -1,       /* controller slots, no game-pad */
        -1, -1, -1, -1,                       /* aux slots */
        75, 100                               /* music, sound */
    };
    memcpy(out->line, def, sizeof def);
}

/* ---- Highscore1-4.dat -------------------------------------------- */

int fa_hs_parse(const unsigned char *buf, size_t len, fa_hs_file *out)
{
    size_t pos = 0;
    for (int i = 0; i < FA_HS_ENTRIES; i++) {
        char sc[16];
        if (read_line(buf, len, &pos, out->entry[i].name,
                      sizeof out->entry[i].name) < 0)
            return -1;
        if (read_line(buf, len, &pos, sc, sizeof sc) < 0) return -1;
        out->entry[i].score = strtoul(sc, NULL, 10);
    }
    return 0;
}

int fa_hs_write(const fa_hs_file *in, unsigned char *out, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < FA_HS_ENTRIES; i++) {
        char tmp[24];
        size_t nl = strlen(in->entry[i].name);
        if (i) {
            if (n + 2 > cap) return -1;
            out[n++] = '\r'; out[n++] = '\n';
        }
        if (n + nl + 2 > cap) return -1;
        memcpy(out + n, in->entry[i].name, nl); n += nl;
        out[n++] = '\r'; out[n++] = '\n';
        int k = sprintf(tmp, "%lu", in->entry[i].score);
        if (n + (size_t)k > cap) return -1;
        memcpy(out + n, tmp, (size_t)k); n += (size_t)k;
    }
    return (int)n;
}

void fa_hs_default(fa_hs_file *out)
{
    for (int i = 0; i < FA_HS_ENTRIES; i++) {
        sprintf(out->entry[i].name, "Player %d", i + 1);
        out->entry[i].score = (unsigned long)(10000 - i * 1000);
    }
}

int fa_hs_insert(fa_hs_file *hs, const char *name, unsigned long score)
{
    int rank = -1;
    for (int i = 0; i < FA_HS_ENTRIES; i++)
        if (score > hs->entry[i].score) { rank = i; break; }
    if (rank < 0) return -1;

    for (int i = FA_HS_ENTRIES - 1; i > rank; i--)
        hs->entry[i] = hs->entry[i - 1];

    memset(hs->entry[rank].name, 0, sizeof hs->entry[rank].name);
    strncpy(hs->entry[rank].name, name ? name : "", FA_HS_NAME_MAX);
    hs->entry[rank].score = score;
    return rank;
}
