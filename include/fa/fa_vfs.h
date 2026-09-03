/*
 * fa_vfs.h - storage abstraction: write beside the game, never inside GData
 *
 * The original writes three things and nothing else: Highscore1-4.dat,
 * Option.ini and tut.ini. It has no registry use, no per-level save and no
 * progression state. On Windows every one of those files lives inside the
 * game data, in GData\Save\.
 *
 * The port must not write into the asset source directory: on most targets
 * that tree is read-only (a mounted disc image, a system content partition,
 * a shared install), and it is the user's own legally-owned copy, which the
 * engine has no business modifying. Instead the writable files sit BESIDE
 * GData, in the install directory - the player sees the
 * executable, the GData/ folder, and the loose writable files next to each
 * other. So this module gives the engine two roots:
 *
 *   asset:   read-only. Points at the GData tree from a lawful copy of the
 *            game. Opening a path under this root for writing is refused.
 *   user:    read-write. By default the directory that holds the GData tree
 *            and the original executable - so a player sees the exe, the
 *            GData/ folder, and the writable files (Option.ini, the
 *            Highscore tables, tut.ini) plus Log/ side by side. If that
 *            directory is read-only (a locked Program Files install, a
 *            mounted image, a console content partition) it falls back to a
 *            per-user path from fa_user_dir: %APPDATA%\<app> on Windows,
 *            $XDG_DATA_HOME/<app> or ~/.local/share/<app> on POSIX, the
 *            save-data mount on a console.
 *
 * A virtual path is "<root>:<relative>", for example "asset:Scripts/EngineInit.jrs"
 * or "user:Option.ini". The relative part uses forward slashes. ".."
 * components, absolute components and drive letters are rejected, so a
 * virtual path can never escape its root.
 *
 * fa_vfs_classify() maps an original game-relative path - written exactly as
 * the .exe used it, with backslashes - to the right virtual path: the three
 * persistent files, and anything the original kept under Save\ or Log\, go to
 * user: (the Save\ prefix is dropped, Log\ is kept); everything else goes to
 * asset:.
 *
 * Path normalisation: resolution lower-cases the relative path and
 * transliterates the German digraphs (ae/oe/ue/ss), so a case-sensitive
 * filesystem resolves the same references Windows did. The asset loader layers
 * the dead-reference handling on top; this module only does the case / digraph
 * fold that every lookup needs.
 */
#ifndef FA_VFS_H
#define FA_VFS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FA_VFS_PATH_MAX  1024

typedef enum {
    FA_VFS_READ = 0,
    FA_VFS_WRITE,
    FA_VFS_APPEND
} fa_vfs_mode;

typedef struct fa_vfs {
    char asset_root[FA_VFS_PATH_MAX];  /* read-only  */
    char user_root[FA_VFS_PATH_MAX];   /* read-write */
} fa_vfs;

/* --- platform: the per-user writable directory (fallback only) ---------- */

/*
 * Write the per-user writable directory for `app` into `out`. The directory
 * is created if it does not exist. `app` is a short name such as
 * "FreshAdventures". Returns 0, or -1 if no writable location could be found
 * or `out` is too small.
 *
 * This is the FALLBACK for fa_vfs_init_default when the install directory is
 * read-only. On a normal desktop install the writable root is the install
 * directory itself, not this path.
 *
 * Backends: fa_paths_win32.c (%APPDATA%\<app>, then %USERPROFILE%\<app>),
 * fa_paths_posix.c ($XDG_DATA_HOME/<app>, then $HOME/.local/share/<app>).
 * A console backend provides its own over the save-data mount.
 */
int fa_user_dir(const char *app, char *out, size_t out_sz);

/* --- lifecycle --------------------------------------------------------- */

/*
 * Initialise with the two real directory paths. `asset_root` must exist and
 * be a directory; `user_root` is created if missing. Either may be NULL to
 * leave that root unset - an unset root rejects every access. Returns 0, or
 * -1 on a bad argument or an unusable asset_root.
 */
int fa_vfs_init(fa_vfs *v, const char *asset_root, const char *user_root);

/*
 * Convenience. asset_root = `gdata_dir`. user_root = the directory that
 * contains `gdata_dir` (the install directory, beside the executable) when
 * that directory is writable; otherwise fa_user_dir(app). Returns 0, or -1
 * (gdata_dir missing/unusable, or no writable location at all).
 */
int fa_vfs_init_default(fa_vfs *v, const char *gdata_dir, const char *app);

/* --- path handling --------------------------------------------------- */

/*
 * Resolve a virtual path ("asset:..." / "user:...") to a real OS path in
 * `out`. Applies the case / digraph fold to the relative part.
 * Returns 0, or -1 if the root prefix is missing or unset, the relative part
 * escapes the root (".."/absolute/drive), or `out` is too small.
 */
int fa_vfs_resolve(const fa_vfs *v, const char *vpath, char *out, size_t out_sz);

/*
 * Map an original game-relative path (as JR_FERRERO.exe used it, backslashes
 * and all) to a virtual path in `out`:
 *   anything under Save\      -> "user:..."  with the Save\ prefix DROPPED
 *                                (Save\Highscore1.dat -> user:Highscore1.dat)
 *   anything under Log\       -> "user:Log/..."  (the Log\ folder is kept)
 *   bare Option.ini / tut.ini / Highscore<1..4>.dat -> "user:..."
 *   everything else           -> "asset:..."
 * Returns 0, or -1 if `out` is too small.
 */
int fa_vfs_classify(const char *game_rel, char *out, size_t out_sz);

/* --- file access --------------------------------------------------- */

/*
 * Open a virtual path. FA_VFS_WRITE / FA_VFS_APPEND are refused with NULL
 * (and errno EACCES) unless the path is under the user root; parent
 * directories under the user root are created as needed. FA_VFS_READ works
 * on either root. The caller fcloses the result.
 */
FILE *fa_vfs_open(const fa_vfs *v, const char *vpath, fa_vfs_mode mode);

/* 1 if the virtual path names an existing file, else 0. */
int fa_vfs_exists(const fa_vfs *v, const char *vpath);

/*
 * Read a whole file. On success returns 0, sets *buf to a malloc'd buffer
 * with a NUL one past the end (not counted in *len) and *len to the byte
 * count. The caller frees *buf. Returns -1 on any failure.
 */
int fa_vfs_read_all(const fa_vfs *v, const char *vpath, void **buf, size_t *len);

/*
 * Write a whole file under the user root (atomically: a sibling temp file is
 * written then renamed over the target). Refused for asset paths. Returns 0
 * or -1.
 */
int fa_vfs_write_all(const fa_vfs *v, const char *vpath,
                     const void *buf, size_t len);

/* --- tut.ini: the 4-byte "tutorial seen" flag, one byte per world -----
 *
 * The exe file is 4 raw bytes at 0x45ED4C..0x45ED4F: byte i (i = 0..3) = "the
 * player has completed world i+1's tutorial". Load 0x40A0B0 zeroes the buffer,
 * fread()s 4 bytes ("rb"), fclose() - a missing or short file leaves every
 * byte 0 (every tutorial shows). Save 0x40A0F0 rewrites all 4 bytes ("wb").
 * The state-0 level loader (0x411682) picks WeltNt.w01/.w02 when the world's
 * byte is 0, else WeltN. The byte is set to 1 when the tutorial is cleared
 * (0x412702 / the Paradiso voice-end path 0x4159BD).
 */

/* Byte `world` (1..4) of tut.ini: 1 = the normal world layout, 0 = play the
 * tutorial (WeltNt). Absent / short file = 0. Never fails. */
int fa_vfs_tut_world_seen(const fa_vfs *v, int world);

/* Set byte `world` (1..4) to `seen` (0/1), preserving the other three bytes
 * (read-modify-write). Returns 0 or -1. */
int fa_vfs_set_tut_world_seen(const fa_vfs *v, int world, int seen);

/*
 * Returns 1 if tut.ini exists under the user root and ANY of its first 4 bytes
 * is non-zero, 0 if it is absent or all zero. Kept for callers that only need
 * "has the player finished any tutorial". Never fails.
 */
int fa_vfs_tut_seen(const fa_vfs *v);

/* Write tut.ini under the user root: 4 bytes, all zero for `seen` == 0, else
 * 0x01 0x00 0x00 0x00 (marks world 1 only). Prefer fa_vfs_set_tut_world_seen.
 * Returns 0 or -1. */
int fa_vfs_set_tut_seen(const fa_vfs *v, int seen);

/* --- helpers (exposed for tests) --------------------------------------- */

/* Lower-case ASCII and transliterate CP1252 German digraphs
 * (0xC4/0xE4 -> "ae", 0xD6/0xF6 -> "oe", 0xDC/0xFC -> "ue", 0xDF -> "ss").
 * Writes at most out_sz-1 bytes plus a NUL. Returns 0 or -1 (truncation). */
int fa_vfs_normalize(const char *rel, char *out, size_t out_sz);

/* Recursively create `real_dir` (and parents). Returns 0 or -1. */
int fa_vfs_mkdirs(const char *real_dir);

#ifdef __cplusplus
}
#endif

#endif /* FA_VFS_H */
