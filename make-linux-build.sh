#!/usr/bin/env bash
# Build a standalone Linux fa_slice with the SDL2 desktop backend and stage it
# in dist/ with a README. Run from this folder.
#
#   ./make-linux-build.sh
#
# Needs SDL2 development files. Debian/Ubuntu:  sudo apt install libsdl2-dev
# Fedora:  sudo dnf install SDL2-devel     Arch:  sudo pacman -S sdl2
#
# FA_BUNDLE_SDL=1 ./make-linux-build.sh
#   also copies libSDL2-2.0.so.0 into dist/ and points the binary at it
#   ($ORIGIN rpath), so the target needs no SDL2 package - just the usual
#   X11 / ALSA / Wayland runtime libs that SDL2 itself pulls in.
#
# The binary (and a bundled libSDL2) only run on a glibc AT LEAST as new as
# the build host's. Build on the OLDEST distro you need to support.
set -euo pipefail
cd "$(dirname "$0")"

CC="${CC:-cc}"
OUT=dist
BUNDLE="${FA_BUNDLE_SDL:-0}"

# SDL2 compile/link flags from pkg-config, else sdl2-config.
if pkg-config --exists sdl2 2>/dev/null; then
  SDL_CFLAGS=$(pkg-config --cflags sdl2)
  SDL_LIBS=$(pkg-config --libs sdl2)
elif command -v sdl2-config >/dev/null 2>&1; then
  SDL_CFLAGS=$(sdl2-config --cflags)
  SDL_LIBS=$(sdl2-config --libs)
else
  echo "SDL2 development files not found (pkg-config sdl2 / sdl2-config)." >&2
  echo "  Debian/Ubuntu: sudo apt install libsdl2-dev" >&2
  echo "  Fedora:        sudo dnf install SDL2-devel" >&2
  echo "  Arch:          sudo pacman -S sdl2" >&2
  exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT"

CFLAGS="-std=c11 -O2 -Wall -Wextra -Iinclude -D_POSIX_C_SOURCE=200809L \
-DFA_HAVE_SDL2=1 -DSDL_MAIN_HANDLED $SDL_CFLAGS"

SRC="
tools/fa_slice.c
src/core/fa_loop.c src/core/fa_fs.c src/core/fa_surface.c src/core/fa_script.c
src/core/fa_aom.c src/core/fa_res.c src/core/fa_vfs.c src/core/fa_input.c
src/core/fa_w01.c src/core/fa_w02.c src/core/fa_map.c src/core/fa_render.c
src/core/fa_entity.c src/core/fa_bmp.c src/core/fa_menu.c
src/core/fa_hiscore.c src/core/fa_options.c src/core/fa_save.c
src/core/fa_wav.c src/core/fa_audio.c src/core/fa_rng.c
src/game/fa_player.c src/game/fa_charspr.c src/game/fa_collide.c src/game/fa_beh.c
src/game/fa_hud.c src/game/fa_death.c src/game/fa_credits.c
src/platform/fa_platform.c src/platform/fa_backend_null.c
src/platform/fa_backend_sdl2.c src/app/fa_app.c
src/platform/fa_time_posix.c src/platform/fa_paths_posix.c
"

LDEXTRA=""
[ "$BUNDLE" = "1" ] && LDEXTRA="-Wl,-rpath,\$ORIGIN"

echo "== compiling + linking fa_slice (SDL2 $(pkg-config --modversion sdl2 2>/dev/null || echo '?')) =="
# shellcheck disable=SC2086
$CC $CFLAGS $SRC $SDL_LIBS -lm $LDEXTRA -o "$OUT/fa_slice"
command -v strip >/dev/null && strip "$OUT/fa_slice" || true

if [ "$BUNDLE" = "1" ]; then
  so=$("$CC" -print-file-name=libSDL2-2.0.so.0 2>/dev/null)
  [ -f "$so" ] || so=$(ldd "$OUT/fa_slice" | sed -n 's/.*=> \(.*libSDL2-2\.0\.so\.0\) .*/\1/p' | head -1)
  if [ -f "$so" ]; then
    cp -L "$so" "$OUT/libSDL2-2.0.so.0"
    command -v strip >/dev/null && strip "$OUT/libSDL2-2.0.so.0" || true
    echo "   bundled $(basename "$so") ($(du -h "$OUT/libSDL2-2.0.so.0" | cut -f1))"
  else
    echo "   WARNING: could not locate libSDL2-2.0.so.0 to bundle" >&2
  fi
fi

cat > "$OUT/README.txt" <<'EOF'
OpenFA - Linux build
====================

You must own Kinder & Ferrero - Fresh Adventures to play it. This ships only
the engine; it reads your GData tree at run time.

RUN
---
Put your GData folder next to the fa_slice binary:

    fa_slice
    GData/Pics/StartBG.bmp
    GData/Maps/...
    GData/Animation/...

Then:  ./fa_slice
Or point at it:  ./fa_slice --gdata /path/to/GData

GData lookup order: --gdata DIR, then <binary dir>/GData, then ./GData.

On a case-sensitive filesystem the engine folds filename case itself, so a
GData copied straight off the disc works. Copy the files as-is - do not
transcode the filenames.

Saves, the high-score tables and Option.ini are written to
$XDG_DATA_HOME/FreshAdventures (default ~/.local/share/FreshAdventures),
never inside GData.

CONTROLS
--------
    Menu    arrows / D-pad select a world, Enter / A confirms; mouse works too
    Level   arrows walk   A jump   S throw   D switch character   Esc quits
    Enter / START  join local co-op (2nd character)

    ./fa_slice --world N     boot straight into world N (1..4)
    ./fa_slice --scale N     open the window at N x 800x600
    ./fa_slice --fullscreen
    ./fa_slice --frames N    run headless, print stats
EOF

if [ "$BUNDLE" = "1" ] && [ -f "$OUT/libSDL2-2.0.so.0" ]; then
  cat >> "$OUT/README.txt" <<'EOF'

BUNDLED SDL2
------------
libSDL2-2.0.so.0 ships in this folder and is loaded from here ($ORIGIN
rpath), so no SDL2 package is needed on the target. SDL2 still uses the
system X11 / Wayland / ALSA / PulseAudio libraries at run time (present on
any desktop). SDL2 is (c) Sam Lantinga, zlib license -
https://www.libsdl.org/license.php
EOF
fi

echo "== headless smoke test (no display here -> null backend) =="
"./$OUT/fa_slice" --frames 3 || true

echo
echo "staged in $OUT/ :"
ls -la "$OUT"
echo
echo "Give the whole $OUT/ folder to the player. They drop GData beside"
echo "fa_slice and run it."
