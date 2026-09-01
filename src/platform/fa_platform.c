/*
 * fa_platform.c - backend selection (RRR-41)
 * Try the SDL2 desktop backend; fall back to the headless null backend.
 */
#include "fa/fa_platform.h"

int fa_platform_create(fa_platform *p, const fa_platform_cfg *cfg)
{
    if (fa_backend_sdl2_available() && fa_backend_sdl2_create(p, cfg) == 0)
        return 0;
    return fa_backend_null_create(p, cfg);
}
