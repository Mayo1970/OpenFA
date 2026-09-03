/*
 * fa_paths_posix.c - the per-user writable directory on POSIX.
 *
 * The port redirects Save/, Log/ and generated files
 * to a per-user path. On POSIX that is $XDG_DATA_HOME/<app>, else
 * $HOME/.local/share/<app> (XDG Base Directory spec). The directory tree is
 * created if missing.
 */
#include "fa/fa_vfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fa_user_dir(const char *app, char *out, size_t out_sz)
{
    if (!app || !app[0]) return -1;

    const char *xdg = getenv("XDG_DATA_HOME");
    int n;

    if (xdg && xdg[0] == '/') {
        n = snprintf(out, out_sz, "%s/%s", xdg, app);
    } else {
        const char *home = getenv("HOME");
        if (!home || !home[0]) return -1;
        n = snprintf(out, out_sz, "%s/.local/share/%s", home, app);
    }
    if (n < 0 || (size_t)n >= out_sz) return -1;

    if (fa_vfs_mkdirs(out) != 0) return -1;
    return 0;
}
