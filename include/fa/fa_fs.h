/*
 * fa_fs.h - case-tolerant read access for the mixed-case GData asset tree.
 *
 * GData was authored for NTFS, so loaders open assets by any spelling. On a
 * case-sensitive filesystem this resolver matches each path segment ignoring
 * ASCII case, and treats a Latin-1 byte as equal to its UTF-8 encoding so
 * non-ASCII names (e.g. "Bär.W01") match a disc copied onto Linux.
 * Windows is a pass-through. Reads only; writes go through fa_vfs.
 */
#ifndef FA_FS_H
#define FA_FS_H

#include <stdio.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FA_FS_PATH_MAX 1024

/* Resolve `in` to an existing on-disk path, tolerating ASCII-case differences
 * per segment; writes the NUL-terminated result to `out`. Returns 0, or -1 if
 * `out` is too small or a segment has no case-insensitive match. */
int fa_fs_resolve(const char *in, char *out, size_t out_sz);

/* fopen, but a failing read mode is retried against the resolved path.
 * Write and append modes pass straight through with no fallback. */
FILE *fa_fs_fopen(const char *path, const char *mode);

#ifdef __cplusplus
}
#endif

#endif /* FA_FS_H */
