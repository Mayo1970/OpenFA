#!/usr/bin/env bash
# Build a standalone Windows OpenFA.exe with the SDL2 desktop backend and
# stage it in dist/. Run from this folder.
#
# Set FA_SIGN_PFX (and FA_SIGN_PW) to an Authenticode cert to sign the exe.
# An unsigned, no-reputation binary is the main reason AV engines flag it.
#
#   ./make-windows-build.sh [SDL2_ROOT] [SDL2_LIBDIR]
#
# SDL2_ROOT   holds include/SDL.h                 (default: the ioq3 thirdparty copy)
# SDL2_LIBDIR holds the SDL2 link libs + SDL2.dll (default: <root>/../libs/win64)
#
# Static link (no SDL2.dll shipped): if SDL_LIBDIR holds a real static lib
# (SDL2-static.lib or libSDL2.a), it is linked and no DLL is staged. Build one
# once from SDL2 source: cmake -B build -DSDL_STATIC=ON -DSDL_SHARED=OFF && cmake --build build
# then copy build/SDL2-static.lib (or libSDL2.a) into SDL_LIBDIR.
# Otherwise it falls back to the import lib + SDL2.dll beside the exe.
set -euo pipefail
cd "$(dirname "$0")"

SDL_ROOT="${1:-/e/Users/Matteo/Desktop/quake3/ioq3/code/thirdparty/SDL2-2.32.8}"
SDL_LIB="${2:-$SDL_ROOT/../libs/win64}"
CC="${CC:-clang}"
OUT=dist

[ -f "$SDL_ROOT/include/SDL.h" ] || { echo "no SDL.h under $SDL_ROOT/include"; exit 1; }

# Pick the SDL2 link mode. Static wins if a real static archive is present.
SDL_STATIC=0
if   [ -f "$SDL_LIB/SDL2-static.lib" ]; then SDL_LINK="$SDL_LIB/SDL2-static.lib"; SDL_STATIC=1
elif [ -f "$SDL_LIB/libSDL2.a" ];       then SDL_LINK="$SDL_LIB/libSDL2.a";       SDL_STATIC=1
elif [ -f "$SDL_LIB/SDL2.lib" ];        then SDL_LINK="$SDL_LIB/SDL2.lib"
else echo "no SDL2 link lib under $SDL_LIB"; exit 1
fi
if [ "$SDL_STATIC" -eq 0 ]; then
  [ -f "$SDL_LIB/SDL2.dll" ] || { echo "shared link needs SDL2.dll under $SDL_LIB"; exit 1; }
fi

rm -rf "$OUT"
mkdir -p "$OUT"

CFLAGS="-std=c11 -O2 -Wall -Wextra -Iinclude -I$SDL_ROOT/include \
-D_CRT_SECURE_NO_WARNINGS -DFA_HAVE_SDL2=1 -DSDL_MAIN_HANDLED"

SRC="
tools/fa_slice.c
src/core/fa_loop.c src/core/fa_fs.c src/core/fa_surface.c src/core/fa_script.c src/core/fa_aom.c
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

echo "== compiling the Windows icon resource =="
RCOBJ="$OUT/fa_win32.res"
# llvm-rc ships with clang; fall back to windres (devkitPro / MinGW).
# llvm-rc parses an MSYS "/abs/path" as an option, so run it from the .rc
# folder with bare names and move the result.
if command -v llvm-rc >/dev/null; then
  ( cd src/platform && llvm-rc fa_win32.rc )
  mv src/platform/fa_win32.res "$RCOBJ"
else
  windres src/platform/fa_win32.rc -O coff -o "$RCOBJ"
fi

# Static SDL2 pulls in its Win32 backends directly, so the exe must link the
# system libs SDL2.dll would otherwise carry. Harmless in the shared case.
SDL_SYSLIBS=""
if [ "$SDL_STATIC" -eq 1 ]; then
  SDL_SYSLIBS="-luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion \
-luuid -ladvapi32 -lsetupapi -lshell32 -ldinput8"
fi

if [ "$SDL_STATIC" -eq 1 ]; then
  MODE="static, no DLL"; SDL_CRTFIX="-Wl,/nodefaultlib:libcmt"
else
  MODE="shared, SDL2.dll"; SDL_CRTFIX=""
fi
echo "== compiling + linking OpenFA.exe (SDL2 $(basename "$SDL_ROOT"), $MODE) =="
# shellcheck disable=SC2086
# /subsystem:windows keeps Windows from opening a console ("prompts") window
# next to the game. main() stays the entry point via mainCRTStartup.
# nodefaultlib:libcmt: static SDL2 is built against the dynamic CRT (MSVCRT);
# drop the static-CRT default so both sides share one CRT heap.
$CC $CFLAGS $SRC "$RCOBJ" "$SDL_LINK" -lwinmm $SDL_SYSLIBS \
  -Wl,/subsystem:windows -Wl,/entry:mainCRTStartup $SDL_CRTFIX -o "$OUT/OpenFA.exe"

# Authenticode sign if a cert is configured. signtool ships with the Windows SDK;
# osslsigncode is the cross-platform fallback. TS is the RFC3161 timestamp URL.
TS=http://timestamp.digicert.com
if [ -n "${FA_SIGN_PFX:-}" ]; then
  echo "== signing OpenFA.exe =="
  if command -v signtool >/dev/null; then
    pw=(); [ -n "${FA_SIGN_PW:-}" ] && pw=(//p "$FA_SIGN_PW")
    signtool sign //f "$FA_SIGN_PFX" "${pw[@]}" //fd sha256 //tr "$TS" //td sha256 "$OUT/OpenFA.exe"
  elif command -v osslsigncode >/dev/null; then
    pw=(); [ -n "${FA_SIGN_PW:-}" ] && pw=(-pass "$FA_SIGN_PW")
    osslsigncode sign -pkcs12 "$FA_SIGN_PFX" "${pw[@]}" -h sha256 -ts "$TS" \
      -in "$OUT/OpenFA.exe" -out "$OUT/OpenFA-signed.exe"
    mv "$OUT/OpenFA-signed.exe" "$OUT/OpenFA.exe"
  else
    echo "   FA_SIGN_PFX set but no signtool / osslsigncode found - shipping unsigned" >&2
  fi
fi

[ "$SDL_STATIC" -eq 1 ] || cp "$SDL_LIB/SDL2.dll" "$OUT/"
cp dist-README.txt "$OUT/README.txt" 2>/dev/null || true

echo "== headless smoke test (no display here -> null backend) =="
"./$OUT/OpenFA.exe" --frames 3 || true

echo
echo "staged in $OUT/ :"
ls -la "$OUT"
echo
if command -v sha256sum >/dev/null; then sha256sum "$OUT/OpenFA.exe"
elif command -v certutil >/dev/null; then certutil -hashfile "$OUT/OpenFA.exe" SHA256
fi
echo
echo "Give the owner the whole $OUT/ folder. They drop GData beside OpenFA.exe"
echo "(or a Maps/ folder under GData/) and double-click it."
