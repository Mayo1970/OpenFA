#!/usr/bin/env bash
# Build a standalone Windows fa_slice.exe with the SDL2 desktop backend and
# stage it in dist/ together with SDL2.dll and a README. Run from this folder.
#
#   ./make-windows-build.sh [SDL2_ROOT] [SDL2_LIBDIR]
#
# SDL2_ROOT   holds include/SDL.h        (default: the ioq3 thirdparty copy)
# SDL2_LIBDIR holds SDL2.lib + SDL2.dll  (default: <root>/../libs/win64)
set -euo pipefail
cd "$(dirname "$0")"

SDL_ROOT="${1:-/e/Users/Matteo/Desktop/quake3/ioq3/code/thirdparty/SDL2-2.32.8}"
SDL_LIB="${2:-$SDL_ROOT/../libs/win64}"
CC="${CC:-clang}"
OUT=dist

[ -f "$SDL_ROOT/include/SDL.h" ] || { echo "no SDL.h under $SDL_ROOT/include"; exit 1; }
[ -f "$SDL_LIB/SDL2.lib" ]       || { echo "no SDL2.lib under $SDL_LIB"; exit 1; }
[ -f "$SDL_LIB/SDL2.dll" ]       || { echo "no SDL2.dll under $SDL_LIB"; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT"

CFLAGS="-std=c11 -O2 -Wall -Wextra -Iinclude -I$SDL_ROOT/include \
-D_CRT_SECURE_NO_WARNINGS -DFA_HAVE_SDL2=1 -DSDL_MAIN_HANDLED"

SRC="
tools/fa_slice.c
src/core/fa_loop.c src/core/fa_surface.c src/core/fa_script.c src/core/fa_aom.c
src/core/fa_res.c src/core/fa_vfs.c src/core/fa_input.c
src/core/fa_w01.c src/core/fa_w02.c src/core/fa_map.c src/core/fa_render.c
src/core/fa_entity.c src/core/fa_bmp.c src/core/fa_menu.c
src/core/fa_hiscore.c src/core/fa_options.c src/core/fa_save.c
src/core/fa_wav.c src/core/fa_audio.c src/core/fa_rng.c
src/game/fa_player.c src/game/fa_charspr.c src/game/fa_collide.c src/game/fa_beh.c
src/game/fa_hud.c src/game/fa_death.c src/game/fa_credits.c
src/platform/fa_platform.c src/platform/fa_backend_null.c
src/platform/fa_backend_sdl2.c src/app/fa_app.c
src/platform/fa_time_win32.c src/platform/fa_paths_win32.c
"

echo "== compiling + linking fa_slice.exe (SDL2 $(basename "$SDL_ROOT")) =="
# shellcheck disable=SC2086
$CC $CFLAGS $SRC "$SDL_LIB/SDL2.lib" -lwinmm -o "$OUT/fa_slice.exe"

cp "$SDL_LIB/SDL2.dll" "$OUT/"
cp dist-README.txt "$OUT/README.txt" 2>/dev/null || true

echo "== headless smoke test (no display here -> null backend) =="
"./$OUT/fa_slice.exe" --frames 3 || true

echo
echo "staged in $OUT/ :"
ls -la "$OUT"
echo
echo "Give the owner the whole $OUT/ folder. They drop GData beside fa_slice.exe"
echo "(or a Maps/ folder under GData/) and double-click it."
