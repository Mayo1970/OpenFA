/*
 * fa_platform.c - backend selection (RRR-41)
 * Select the native backend for the target. The Switch build must initialize
 * libnx's real display path; desktop builds retain SDL2 -> null fallback.
 */
#include "fa/fa_platform.h"

int fa_platform_create(fa_platform *p, const fa_platform_cfg *cfg)
{
#if defined(__SWITCH__)
    return fa_backend_switch_create(p, cfg);
#else
    if (fa_backend_sdl2_available() && fa_backend_sdl2_create(p, cfg) == 0)
        return 0;
    return fa_backend_null_create(p, cfg);
#endif
}
