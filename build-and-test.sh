#!/usr/bin/env bash
# Build the engine core and run the RRR-34 loop tests. No external deps.
set -euo pipefail
cd "$(dirname "$0")"

CC=${CC:-clang}
CFLAGS="-std=c11 -Wall -Wextra -Wpedantic -O2 -Iinclude -D_CRT_SECURE_NO_WARNINGS"
OUT=_build
mkdir -p "$OUT"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*|Windows_NT)
    PLAT=src/platform/fa_time_win32.c;  PATHS=src/platform/fa_paths_win32.c
    TIMELIB="-lwinmm";  MATHLIB="" ;;
  *)
    PLAT=src/platform/fa_time_posix.c;  PATHS=src/platform/fa_paths_posix.c
    TIMELIB="";  MATHLIB="-lm" ;;
esac

CORE="src/core/fa_loop.c $PLAT"
STORAGE="src/core/fa_vfs.c $PATHS"
AUDIO="src/core/fa_audio.c src/core/fa_wav.c"

echo "== stage 1: capture the golden hash =="
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_loop.c $CORE $TIMELIB -o "$OUT/test_loop_probe"
GOLD=$("$OUT/test_loop_probe" | sed -n 's/.*golden hash = \(0x[0-9a-f]*\).*/\1/p')
echo "   golden hash = $GOLD"

echo "== stage 2: build tests with the pinned hash =="
# shellcheck disable=SC2086
$CC $CFLAGS -DGOLDEN_HASH="${GOLD}u" tests/test_loop.c $CORE $TIMELIB -o "$OUT/test_loop"

echo "== build tools =="
# shellcheck disable=SC2086
$CC $CFLAGS tools/sim_replay.c $CORE $TIMELIB -o "$OUT/sim_replay"

echo "== stage 3: blitter + script + aom + resource tests (RRR-35..38) =="
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_surface.c src/core/fa_surface.c -o "$OUT/test_surface"
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_script.c  src/core/fa_script.c  -o "$OUT/test_script"
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_aom.c     src/core/fa_aom.c     -o "$OUT/test_aom"
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_res.c     src/core/fa_res.c     -o "$OUT/test_res"
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_rng.c     src/core/fa_rng.c     -o "$OUT/test_rng"

echo "== stage 4: storage + input tests (RRR-39, RRR-40) =="
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_vfs.c   $STORAGE -o "$OUT/test_vfs"
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_input.c src/core/fa_input.c $STORAGE -o "$OUT/test_input"

echo "== stage 5: platform backend + app loop (RRR-41) =="
MAPSRC="src/core/fa_w01.c src/core/fa_w02.c src/core/fa_map.c src/core/fa_render.c \
src/core/fa_entity.c src/core/fa_aom.c src/core/fa_bmp.c src/core/fa_menu.c \
src/core/fa_hiscore.c src/core/fa_options.c src/core/fa_save.c src/core/fa_rng.c \
src/game/fa_player.c src/game/fa_charspr.c src/game/fa_collide.c src/game/fa_beh.c \
src/game/fa_hud.c src/game/fa_death.c src/game/fa_credits.c"
PLATFORM_SRC="src/platform/fa_backend_null.c src/platform/fa_backend_sdl2.c \
src/platform/fa_platform.c src/app/fa_app.c src/core/fa_surface.c \
src/core/fa_input.c src/core/fa_vfs.c $AUDIO $MAPSRC $CORE $PATHS"
# no SDL2 in this build path -> the null backend; FA_HAVE_SDL2 stays undefined.
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_platform.c $PLATFORM_SRC $TIMELIB $MATHLIB -o "$OUT/test_platform"
# shellcheck disable=SC2086
$CC $CFLAGS tools/fa_slice.c $PLATFORM_SRC $TIMELIB $MATHLIB -o "$OUT/fa_slice"

echo "== stage 6: map load + render + entities (RRR-42) =="
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_map.c $MAPSRC src/core/fa_surface.c -o "$OUT/test_map"
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_entity.c $MAPSRC src/core/fa_surface.c -o "$OUT/test_entity"
# shellcheck disable=SC2086
$CC $CFLAGS tools/map_probe.c src/core/fa_w02.c src/core/fa_map.c -o "$OUT/map_probe"

echo "== stage 7: player controller + collision (RRR-43 / RRR-44) =="
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_player.c src/game/fa_player.c src/game/fa_collide.c \
  -o "$OUT/test_player"
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_beh.c $MAPSRC src/core/fa_surface.c -o "$OUT/test_beh"
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_death.c src/game/fa_death.c -o "$OUT/test_death"
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_collide.c src/game/fa_collide.c -o "$OUT/test_collide"
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_charspr.c src/game/fa_charspr.c src/core/fa_w01.c \
  src/core/fa_surface.c -o "$OUT/test_charspr"

echo "== stage 8: menu + high-score + options + save (RRR-47) =="
MENUSRC="src/core/fa_menu.c src/core/fa_hiscore.c src/core/fa_options.c \
src/core/fa_save.c src/core/fa_bmp.c src/core/fa_w01.c src/core/fa_surface.c"
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_menu.c $MENUSRC -o "$OUT/test_menu"
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_hiscore.c $MENUSRC -o "$OUT/test_hiscore"
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_save.c src/core/fa_save.c src/core/fa_options.c \
  -o "$OUT/test_save"
# RRR-54: credits sequence
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_credits.c src/game/fa_credits.c src/core/fa_bmp.c \
  src/core/fa_w01.c src/core/fa_surface.c -o "$OUT/test_credits"

echo "== stage 9: audio mixer + WAV reader (RRR-46) =="
# shellcheck disable=SC2086
$CC $CFLAGS tests/test_audio.c $AUDIO $MATHLIB -o "$OUT/test_audio"

echo "== run tests =="
"$OUT/test_loop"
"$OUT/test_surface"
"$OUT/test_res"
"$OUT/test_rng"
"$OUT/test_vfs"
"$OUT/test_input"
"$OUT/test_platform"
"$OUT/test_player"
"$OUT/test_beh"
"$OUT/test_death"
"$OUT/test_collide"
"$OUT/test_charspr"
"$OUT/test_menu"
"$OUT/test_save"
"$OUT/test_credits"
"$OUT/test_audio"

# RRR-36 AC1 needs the real EngineInit.jrs / DetailGroup.jrs. Use FA_GDATA if
# set, else the frozen master copy next to this tree, else run semantics only.
GDATA="${FA_GDATA:-../master/GData}"
if [ -f "$GDATA/Scripts/EngineInit.jrs" ]; then
  "$OUT/test_script" "$GDATA"
  "$OUT/test_aom" "$GDATA"
  "$OUT/test_map" "$GDATA"
  "$OUT/test_entity" "$GDATA"
  "$OUT/test_charspr" "$GDATA"
  "$OUT/test_menu" "$GDATA"
  "$OUT/test_hiscore" "$GDATA"
  "$OUT/test_credits" "$GDATA"
  "$OUT/test_audio" "$GDATA"
else
  echo "   (no GData found at $GDATA - running semantics-only checks)"
  "$OUT/test_script"
  "$OUT/test_aom"
  "$OUT/test_map"
  "$OUT/test_entity"
  "$OUT/test_charspr"
  "$OUT/test_menu"
  "$OUT/test_hiscore"
  "$OUT/test_credits"
  "$OUT/test_audio"
fi

echo "== replay determinism check =="
"$OUT/sim_replay" record 0xA5A5 3000 > "$OUT/r.replay"
A=$("$OUT/sim_replay" run "$OUT/r.replay")
B=$("$OUT/sim_replay" run "$OUT/r.replay")
echo "   run A: $A"
echo "   run B: $B"
[ "$A" = "$B" ] || { echo "REPLAY MISMATCH"; exit 1; }

echo
echo "OK"
