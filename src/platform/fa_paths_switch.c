/*
 * fa_paths_switch.c - writable fallback for the Nintendo Switch SD card.
 *
 * The normal install layout keeps saves beside GData. This path is used only
 * when that directory cannot be written, and keeps generated files under the
 * app's SD-card directory rather than attempting to write into the asset tree.
 */
#include "fa/fa_vfs.h"

#include <stdio.h>

int fa_user_dir(const char *app, char *out, size_t out_sz)
{
    (void)app;
    int n = snprintf(out, out_sz, "sdmc:/switch/freshadventures/save");
    if (n < 0 || (size_t)n >= out_sz) return -1;
    return fa_vfs_mkdirs(out);
}
