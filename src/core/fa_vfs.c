/*
 * fa_vfs.c - storage abstraction: write beside the game, never inside GData.
 *            See include/fa/fa_vfs.h. Zero external dependencies.
 */
#include "fa/fa_vfs.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#  include <direct.h>
#  define FA_MKDIR(p)  _mkdir(p)
#else
#  define FA_MKDIR(p)  mkdir((p), 0777)
#endif

#ifndef S_ISDIR
#  define S_ISDIR(m)  (((m) & S_IFDIR) != 0)
#endif

/* ------------------------------------------------------------------ utils */

static int copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n + 1 > cap) return -1;
    memcpy(dst, src, n + 1);
    return 0;
}

/* Strip a single trailing '/' or '\\' (but never turn "/" or "C:\\" empty). */
static void trim_trailing_sep(char *s)
{
    size_t n = strlen(s);
    while (n > 1 && (s[n - 1] == '/' || s[n - 1] == '\\')) {
        if (n == 3 && s[1] == ':') break;     /* keep "C:\" */
        s[--n] = '\0';
    }
}

int fa_vfs_normalize(const char *rel, char *out, size_t out_sz)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)rel; *p; p++) {
        const char *sub = NULL;
        unsigned char c = *p;
        switch (c) {
            case 0xC4: case 0xE4: sub = "ae"; break;
            case 0xD6: case 0xF6: sub = "oe"; break;
            case 0xDC: case 0xFC: sub = "ue"; break;
            case 0xDF:            sub = "ss"; break;
            default: break;
        }
        if (sub) {
            if (o + 2 >= out_sz) return -1;
            out[o++] = sub[0];
            out[o++] = sub[1];
            continue;
        }
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (c == '\\') c = '/';
        if (o + 1 >= out_sz) return -1;
        out[o++] = (char)c;
    }
    out[o] = '\0';
    return 0;
}

/* Reject a relative part that could escape its root. */
static int rel_is_safe(const char *rel)
{
    if (rel[0] == '/' || rel[0] == '\\') return 0;          /* absolute      */
    if (rel[0] && rel[1] == ':') return 0;                  /* drive letter  */
    for (const char *p = rel; *p; ) {
        const char *seg = p;
        while (*p && *p != '/' && *p != '\\') p++;
        size_t len = (size_t)(p - seg);
        if (len == 2 && seg[0] == '.' && seg[1] == '.') return 0;
        if (*p) p++;
    }
    return 1;
}

int fa_vfs_mkdirs(const char *real_dir)
{
    char buf[FA_VFS_PATH_MAX];
    if (copy_str(buf, sizeof buf, real_dir) != 0) return -1;

    for (char *p = buf + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char sep = *p;
            *p = '\0';
            if (buf[0] && !(p - buf == 2 && buf[1] == ':')) {
                if (FA_MKDIR(buf) != 0 && errno != EEXIST) return -1;
            }
            *p = sep;
        }
    }
    if (FA_MKDIR(buf) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* ------------------------------------------------------------- lifecycle */

static int is_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

/* Directory that contains `path`. A path with no separator yields ".".
 * Keeps a bare root ("/" or "C:\"). */
static int parent_dir(const char *path, char *out, size_t out_sz)
{
    if (copy_str(out, out_sz, path) != 0) return -1;
    trim_trailing_sep(out);

    char *slash = NULL;
    for (char *p = out; *p; p++)
        if (*p == '/' || *p == '\\') slash = p;

    if (!slash) return copy_str(out, out_sz, ".");
    if (slash == out) { out[1] = '\0'; return 0; }              /* "/x" -> "/" */
    if (slash == out + 2 && out[1] == ':') { slash[1] = '\0'; return 0; } /* "C:\" */
    *slash = '\0';
    return 0;
}

/* Can we create a file in `dir`? Probe with a temp file and remove it. */
static int dir_is_writable(const char *dir)
{
    char probe[FA_VFS_PATH_MAX];
    if ((size_t)snprintf(probe, sizeof probe, "%s/.fa_write_probe", dir)
        >= sizeof probe)
        return 0;
    FILE *f = fopen(probe, "wb");
    if (!f) return 0;
    fclose(f);
    remove(probe);
    return 1;
}

int fa_vfs_init(fa_vfs *v, const char *asset_root, const char *user_root)
{
    if (!v) return -1;
    memset(v, 0, sizeof *v);

    if (asset_root && asset_root[0]) {
        if (copy_str(v->asset_root, sizeof v->asset_root, asset_root) != 0)
            return -1;
        trim_trailing_sep(v->asset_root);
        if (!is_dir(v->asset_root)) return -1;
    }
    if (user_root && user_root[0]) {
        if (copy_str(v->user_root, sizeof v->user_root, user_root) != 0)
            return -1;
        trim_trailing_sep(v->user_root);
        if (fa_vfs_mkdirs(v->user_root) != 0) return -1;
    }
    return 0;
}

int fa_vfs_init_default(fa_vfs *v, const char *gdata_dir, const char *app)
{
    if (!gdata_dir || !gdata_dir[0]) return -1;

    /* The writable root sits beside the game: the directory that holds the
     * GData tree and the original executable. Save/, Log/, Option.ini, the
     * high-score tables and tut.ini land there, next to GData, not inside
     * it. */
    char root[FA_VFS_PATH_MAX];
    if (parent_dir(gdata_dir, root, sizeof root) == 0 && dir_is_writable(root))
        return fa_vfs_init(v, gdata_dir, root);

    /* The install directory is read-only (a locked Program Files install, a
     * mounted image, a console content partition). Fall back to a per-user
     * writable directory. */
    char udir[FA_VFS_PATH_MAX];
    if (fa_user_dir(app ? app : "FreshAdventures", udir, sizeof udir) != 0)
        return -1;
    return fa_vfs_init(v, gdata_dir, udir);
}

/* ----------------------------------------------------------- resolution */

/* Split "root:rel" -> (which root, rel pointer). Returns the root dir, or
 * NULL if the prefix is bad or that root is unset. *is_user tells writes
 * apart. */
static const char *split_vpath(const fa_vfs *v, const char *vpath,
                               const char **rel_out, int *is_user)
{
    const char *root;
    if (strncmp(vpath, "asset:", 6) == 0) {
        *rel_out = vpath + 6;
        *is_user = 0;
        root = v->asset_root;
    } else if (strncmp(vpath, "user:", 5) == 0) {
        *rel_out = vpath + 5;
        *is_user = 1;
        root = v->user_root;
    } else {
        return NULL;
    }
    return root[0] ? root : NULL;
}

int fa_vfs_resolve(const fa_vfs *v, const char *vpath, char *out, size_t out_sz)
{
    const char *rel;
    int is_user;
    if (!v || !vpath) return -1;

    const char *root = split_vpath(v, vpath, &rel, &is_user);
    if (!root) return -1;
    if (!rel_is_safe(rel)) return -1;

    char norm[FA_VFS_PATH_MAX];
    if (fa_vfs_normalize(rel, norm, sizeof norm) != 0) return -1;

    size_t need = strlen(root) + 1 + strlen(norm) + 1;
    if (need > out_sz) return -1;

    strcpy(out, root);
    if (norm[0]) {
        size_t rn = strlen(out);
        if (rn && out[rn - 1] != '/' && out[rn - 1] != '\\') {
            out[rn] = '/';
            out[rn + 1] = '\0';
        }
        strcat(out, norm);
    }
    return 0;
}

/* Case-insensitive "does game_rel start with prefix\\" or equal a name. */
static int ci_has_prefix(const char *s, const char *prefix)
{
    size_t n = strlen(prefix);
    for (size_t i = 0; i < n; i++) {
        char a = s[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return s[n] == '/' || s[n] == '\\';
}

static int ci_equal(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* Case-insensitive prefix test, no trailing-separator requirement. */
static int ci_starts_with(const char *s, const char *prefix)
{
    for (size_t i = 0; prefix[i]; i++) {
        char a = s[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

/* "Highscore1.dat" .. "Highscore4.dat", any case. */
static int is_highscore_dat(const char *base)
{
    if (!ci_starts_with(base, "highscore")) return 0;
    if (strlen(base) != 14) return 0;
    if (base[9] < '1' || base[9] > '4') return 0;
    return ci_equal(base + 10, ".dat");
}

/* Base name after the last '/' or '\\'. */
static const char *base_name(const char *p)
{
    const char *b = p;
    for (const char *q = p; *q; q++)
        if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}

int fa_vfs_classify(const char *game_rel, char *out, size_t out_sz)
{
    /* skip a leading separator */
    while (*game_rel == '/' || *game_rel == '\\') game_rel++;

    const char *base = base_name(game_rel);
    int to_user = 0;

    if (ci_has_prefix(game_rel, "Save")) {
        /* Save\ only grouped the writable files inside GData. The port keeps
         * them loose beside the game, so the Save\ prefix is dropped:
         * Save\Highscore1.dat -> user:Highscore1.dat. */
        game_rel += 5;              /* "Save" + one separator */
        to_user = 1;
    } else if (ci_has_prefix(game_rel, "Log")) {
        to_user = 1;                /* Log\ stays a folder: user:Log/... */
    } else if (ci_equal(base, "Option.ini") || ci_equal(base, "tut.ini")) {
        to_user = 1;
    } else if (is_highscore_dat(base)) {
        to_user = 1;
    }

    const char *prefix = to_user ? "user:" : "asset:";
    size_t need = strlen(prefix) + strlen(game_rel) + 1;
    if (need > out_sz) return -1;

    strcpy(out, prefix);
    /* normalise backslashes to forward slashes in the stored relative part */
    char *w = out + strlen(prefix);
    for (const char *r = game_rel; *r; r++)
        *w++ = (*r == '\\') ? '/' : *r;
    *w = '\0';
    return 0;
}

/* ---------------------------------------------------------------- files */

FILE *fa_vfs_open(const fa_vfs *v, const char *vpath, fa_vfs_mode mode)
{
    const char *rel;
    int is_user;
    if (!v || !vpath) { errno = EINVAL; return NULL; }

    if (!split_vpath(v, vpath, &rel, &is_user)) { errno = ENOENT; return NULL; }

    if (mode != FA_VFS_READ && !is_user) {
        errno = EACCES;                 /* never write under asset: */
        return NULL;
    }

    char real[FA_VFS_PATH_MAX];
    if (fa_vfs_resolve(v, vpath, real, sizeof real) != 0) {
        errno = ENAMETOOLONG;
        return NULL;
    }

    if (mode != FA_VFS_READ) {
        /* make the parent directory tree */
        char dir[FA_VFS_PATH_MAX];
        if (copy_str(dir, sizeof dir, real) != 0) { errno = ENAMETOOLONG; return NULL; }
        char *slash = NULL;
        for (char *p = dir; *p; p++) if (*p == '/' || *p == '\\') slash = p;
        if (slash) { *slash = '\0'; if (fa_vfs_mkdirs(dir) != 0) return NULL; }
    }

    const char *m = (mode == FA_VFS_WRITE) ? "wb"
                  : (mode == FA_VFS_APPEND) ? "ab" : "rb";
    return fopen(real, m);
}

int fa_vfs_exists(const fa_vfs *v, const char *vpath)
{
    char real[FA_VFS_PATH_MAX];
    if (fa_vfs_resolve(v, vpath, real, sizeof real) != 0) return 0;
    FILE *f = fopen(real, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int fa_vfs_read_all(const fa_vfs *v, const char *vpath, void **buf, size_t *len)
{
    FILE *f = fa_vfs_open(v, vpath, FA_VFS_READ);
    if (!f) return -1;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return -1; }
    rewind(f);

    char *p = (char *)malloc((size_t)n + 1);
    if (!p) { fclose(f); return -1; }

    size_t got = fread(p, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(p); return -1; }

    p[n] = '\0';
    *buf = p;
    if (len) *len = (size_t)n;
    return 0;
}

int fa_vfs_write_all(const fa_vfs *v, const char *vpath,
                     const void *buf, size_t len)
{
    const char *rel;
    int is_user;
    if (!split_vpath(v, vpath, &rel, &is_user) || !is_user) {
        errno = EACCES;
        return -1;
    }

    char real[FA_VFS_PATH_MAX];
    if (fa_vfs_resolve(v, vpath, real, sizeof real) != 0) return -1;

    char tmp[FA_VFS_PATH_MAX];
    if (strlen(real) + 5 > sizeof tmp) return -1;
    strcpy(tmp, real);
    strcat(tmp, ".tmp");

    /* mkdirs for the parent */
    char vtmp[FA_VFS_PATH_MAX];
    if ((size_t)snprintf(vtmp, sizeof vtmp, "%s.tmp", vpath) >= sizeof vtmp)
        return -1;
    FILE *f = fa_vfs_open(v, vtmp, FA_VFS_WRITE);
    if (!f) return -1;

    int ok = (len == 0) || (fwrite(buf, 1, len, f) == len);
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(tmp); return -1; }

    remove(real);                       /* Windows rename won't overwrite */
    if (rename(tmp, real) != 0) { remove(tmp); return -1; }
    return 0;
}

/* -------------------------------------------------------------- tut.ini */

/* Read the 4-byte file into `out` (missing / short -> all zero). */
static void tut_read(const fa_vfs *v, unsigned char out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0;
    void *buf = NULL;
    size_t len = 0;
    if (fa_vfs_read_all(v, "user:tut.ini", &buf, &len) != 0) return;
    const unsigned char *b = (const unsigned char *)buf;
    for (size_t i = 0; i < 4 && i < len; i++) out[i] = b[i];
    free(buf);
}

int fa_vfs_tut_world_seen(const fa_vfs *v, int world)
{
    if (world < 1 || world > 4) return 0;
    unsigned char b[4];
    tut_read(v, b);
    return b[world - 1] != 0;
}

int fa_vfs_set_tut_world_seen(const fa_vfs *v, int world, int seen)
{
    if (world < 1 || world > 4) return -1;
    unsigned char b[4];
    tut_read(v, b);
    b[world - 1] = seen ? 1 : 0;
    return fa_vfs_write_all(v, "user:tut.ini", b, sizeof b);
}

int fa_vfs_tut_seen(const fa_vfs *v)
{
    unsigned char b[4];
    tut_read(v, b);
    return (b[0] | b[1] | b[2] | b[3]) != 0;
}

int fa_vfs_set_tut_seen(const fa_vfs *v, int seen)
{
    unsigned char b[4] = { 0, 0, 0, 0 };
    if (seen) b[0] = 1;
    return fa_vfs_write_all(v, "user:tut.ini", b, sizeof b);
}
