/*
 * fa_fs.c - case-tolerant asset path resolution. See fa/fa_fs.h.
 */
#include "fa/fa_fs.h"

#include <string.h>

#if defined(_WIN32)

/* NTFS folds case itself: nothing to do. */
int fa_fs_resolve(const char *in, char *out, size_t out_sz)
{
    if (!in || !out) return -1;
    size_t n = strlen(in);
    if (n + 1 > out_sz) return -1;
    memcpy(out, in, n + 1);
    return 0;
}

FILE *fa_fs_fopen(const char *path, const char *mode)
{
    return fopen(path, mode);
}

#else /* POSIX */

#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>

static int is_read_mode(const char *mode)
{
    return mode && (mode[0] == 'r');
}

/* Decode one unit from *pp and advance it: an ASCII byte, a 2-byte UTF-8
 * sequence, or a lone >=0x80 byte read as Latin-1. Returns the code point. */
static unsigned next_unit(const unsigned char **pp)
{
    const unsigned char *p = *pp;
    unsigned c = p[0];
    if (c < 0x80) { *pp = p + 1; return c; }
    if (c >= 0xC2 && c <= 0xDF && (p[1] & 0xC0) == 0x80) {
        *pp = p + 2;
        return ((c & 0x1Fu) << 6) | (p[1] & 0x3Fu);
    }
    *pp = p + 1;
    return c;                     /* lone byte: Latin-1 code point */
}

/* ASCII-case-insensitive segment equality. Also treats a Latin-1 byte as equal
 * to its UTF-8 encoding, so "Bär.W01" matches whether the disc stored the name
 * as Latin-1 (as the scripts spell it) or as UTF-8. */
static int seg_ci_equal(const char *a, const char *b)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (;;) {
        if (*pa == 0 || *pb == 0) return *pa == *pb;
        unsigned ca = next_unit(&pa), cb = next_unit(&pb);
        if (ca < 0x80) ca = (unsigned)tolower((int)ca);
        if (cb < 0x80) cb = (unsigned)tolower((int)cb);
        if (ca != cb) return 0;
    }
}

static int path_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

/* Append "/name" (or "name" when acc is empty) to acc[len]. Returns the new
 * length, or 0 on overflow. */
static size_t path_join(char *acc, size_t len, const char *name)
{
    size_t nn = strlen(name);
    int sep = (len > 0 && acc[len - 1] != '/');
    if (len + (size_t)sep + nn + 1 > FA_FS_PATH_MAX) return 0;
    if (sep) acc[len++] = '/';
    memcpy(acc + len, name, nn + 1);
    return len + nn;
}

/* Look for a directory entry in `dir` that equals `want` under ASCII-case
 * folding. On a hit, copy the real name into `want` (cap bytes) and return 1. */
static int find_ci(const char *dir, char *want, size_t cap)
{
    DIR *d = opendir(dir[0] ? dir : ".");
    if (!d) return 0;

    struct dirent *e;
    int hit = 0;
    while ((e = readdir(d)) != NULL) {
        if (!seg_ci_equal(e->d_name, want)) continue;
        if (strlen(e->d_name) + 1 > cap) break;   /* cannot store it */
        strcpy(want, e->d_name);
        hit = 1;
        break;
    }
    closedir(d);
    return hit;
}

int fa_fs_resolve(const char *in, char *out, size_t out_sz)
{
    if (!in || !in[0] || !out) return -1;

    if (path_exists(in)) {
        size_t n = strlen(in);
        if (n + 1 > out_sz) return -1;
        memcpy(out, in, n + 1);
        return 0;
    }

    char acc[FA_FS_PATH_MAX];
    size_t alen = 0;
    const char *p = in;

    if (*p == '/') { acc[alen++] = '/'; p++; }
    acc[alen] = '\0';

    while (*p) {
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t seglen = (size_t)(p - seg);
        while (*p == '/') p++;              /* collapse repeated slashes */
        if (seglen == 0) continue;

        char want[512];
        if (seglen + 1 > sizeof want) return -1;
        memcpy(want, seg, seglen);
        want[seglen] = '\0';

        int special = (strcmp(want, ".") == 0 || strcmp(want, "..") == 0);

        size_t probe = path_join(acc, alen, want);
        if (probe == 0) return -1;

        if (!special && !path_exists(acc)) {
            /* restore acc (path_join wrote `want` in place) and scan */
            acc[alen] = '\0';
            if (!find_ci(acc, want, sizeof want)) return -1;
            probe = path_join(acc, alen, want);
            if (probe == 0) return -1;
        }
        alen = probe;
    }

    if (alen + 1 > out_sz) return -1;
    memcpy(out, acc, alen + 1);
    return 0;
}

FILE *fa_fs_fopen(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode);
    if (f || !is_read_mode(mode) || !path) return f;

    char real[FA_FS_PATH_MAX];
    if (fa_fs_resolve(path, real, sizeof real) != 0) return NULL;
    if (strcmp(real, path) == 0) return NULL;    /* nothing new to try */
    return fopen(real, mode);
}

#endif /* POSIX */
