/*
 * fa_paths_win32.c - the per-user writable directory on Windows (RRR-39).
 *
 * ENGINE-ARCH section 8: the port redirects Save/, Log/ and generated files
 * to a per-user path. On Windows that is %APPDATA%\<app> (roaming), with
 * %USERPROFILE%\<app> as a fallback. The directory is created if missing.
 */
#include "fa/fa_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fa_user_dir(const char *app, char *out, size_t out_sz)
{
    if (!app || !app[0]) return -1;

    const char *base = getenv("APPDATA");
    if (!base || !base[0]) base = getenv("USERPROFILE");
    if (!base || !base[0]) base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) return -1;

    int n = snprintf(out, out_sz, "%s\\%s", base, app);
    if (n < 0 || (size_t)n >= out_sz) return -1;

    if (fa_vfs_mkdirs(out) != 0) return -1;
    return 0;
}
