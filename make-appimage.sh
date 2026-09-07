#!/usr/bin/env bash
# Build a self-contained OpenFA-x86_64.AppImage: the OpenFA binary plus its
# whole shared-library closure (SDL2, X11, ALSA, PulseAudio, Wayland, ...),
# minus the parts that must come from the target (glibc core, the GPU stack).
#
#   ./make-appimage.sh [ICON]
#
# ICON is an .ico or .png for the launcher (default: src/icon/icon.png, else a
# plain placeholder). Needs: a C compiler,
# pkg-config, SDL2 dev files, wget, and ImageMagick ("convert").
#
# SDL2 is linked statically by default (needs libSDL2.a on the build host - see
# make-linux-build.sh). Set FA_SDL2_STATIC=0 to bundle libSDL2.so instead; the
# ldd closure below picks it up either way.
#
# RUN THIS ON THE OLDEST GLIBC YOU WANT TO SUPPORT. The AppImage runs on that
# glibc and every newer one, never older. Ubuntu 20.04 (glibc 2.31) covers
# essentially every desktop still in use.
set -euo pipefail
cd "$(dirname "$0")"

ICON_SRC="${1:-src/icon/icon.png}"
OUT=dist
APPDIR=AppDir
NAME=OpenFA-x86_64.AppImage
L=/usr/lib/x86_64-linux-gnu

command -v convert >/dev/null || { echo "need ImageMagick (convert)"; exit 1; }
command -v wget    >/dev/null || { echo "need wget"; exit 1; }

echo "== build OpenFA =="
FA_SDL2_STATIC="${FA_SDL2_STATIC:-1}" FA_BUNDLE_SDL=0 ./make-linux-build.sh >/dev/null
command -v strip >/dev/null && strip "$OUT/OpenFA" || true

echo "== assemble $APPDIR =="
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib"
cp "$OUT/OpenFA" "$APPDIR/usr/bin/OpenFA"

# Full ldd closure. Never ship: the loader + core glibc (must match the
# target kernel) and the GPU stack (must be the target's driver).
never='^(ld-linux|libc|libm|libdl|libpthread|librt|libGL|libGLX|libGLdispatch|libEGL|libOpenGL|libGLESv2|libdrm|libgbm|libglapi)\.so'
copy_closure() {
    ldd "$1" 2>/dev/null | awk '/=> \//{print $3}' | while read -r lib; do
        bn=$(basename "$lib")
        echo "$bn" | grep -Eq "$never" && continue
        [ -e "$APPDIR/usr/lib/$bn" ] && continue
        cp -L "$lib" "$APPDIR/usr/lib/$bn"
        copy_closure "$APPDIR/usr/lib/$bn"
    done
}
copy_closure "$APPDIR/usr/bin/OpenFA"

# SDL2 dlopen's its audio and video backends, so they are not in NEEDED. Add
# the safe ones (never the GPU libs) and their closures. A static SDL2 also
# dlopen's X11 / Xext / ..., which a shared libSDL2 would have pulled as NEEDED.
for extra in libasound.so.2 libpulse.so.0 libpulse-simple.so.0 libjack.so.0 \
             libpipewire-0.3.so.0 libsndio.so.7 libwayland-client.so.0 \
             libwayland-cursor.so.0 libwayland-egl.so.1 libxkbcommon.so.0 \
             libdecor-0.so.0 libX11.so.6 libXext.so.6 libXcursor.so.1 \
             libXi.so.6 libXrandr.so.2 libXfixes.so.3 libXrender.so.1 \
             libXss.so.1 libXinerama.so.1 libXxf86vm.so.1; do
    [ -e "$L/$extra" ] || continue
    [ -e "$APPDIR/usr/lib/$extra" ] || cp -L "$L/$extra" "$APPDIR/usr/lib/$extra"
    copy_closure "$APPDIR/usr/lib/$extra"
done
echo "   bundled $(ls "$APPDIR/usr/lib" | wc -l) libraries"

cat > "$APPDIR/AppRun" <<'R'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="${HERE}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
exec "${HERE}/usr/bin/OpenFA" "$@"
R
chmod +x "$APPDIR/AppRun"

cat > "$APPDIR/openfa.desktop" <<'D'
[Desktop Entry]
Type=Application
Name=OpenFA
Comment=Kinder & Ferrero - Fresh Adventures (OpenFA engine)
Exec=OpenFA
Icon=openfa
Categories=Game;
Terminal=false
D

# Prefer the pre-rendered hicolor set from src/icon; else derive from ICON_SRC;
# else a placeholder. appimagetool reads usr/share/icons/hicolor and the
# top-level openfa.png (used as .DirIcon).
if [ -f src/icon/icon_256.png ]; then
    for s in 16 24 32 48 64 128 256 512; do
        [ -f "src/icon/icon_${s}.png" ] || continue
        install -Dm644 "src/icon/icon_${s}.png" \
            "$APPDIR/usr/share/icons/hicolor/${s}x${s}/apps/openfa.png"
    done
    cp src/icon/icon_256.png "$APPDIR/openfa.png"
elif [ -f "$ICON_SRC" ]; then
    for s in 16 24 32 48 64 128 256; do
        d="$APPDIR/usr/share/icons/hicolor/${s}x${s}/apps"
        mkdir -p "$d"
        convert "${ICON_SRC}[0]" -resize ${s}x${s} -background none \
            -gravity center -extent ${s}x${s} "$d/openfa.png"
    done
    cp "$APPDIR/usr/share/icons/hicolor/256x256/apps/openfa.png" "$APPDIR/openfa.png"
else
    echo "   (no icon at $ICON_SRC - using a placeholder)"
    convert -size 256x256 xc:'#c8102e' -gravity center -pointsize 40 \
        -fill white -annotate 0 'FA' "$APPDIR/openfa.png"
fi

echo "== fetch appimagetool =="
if [ ! -x appimagetool-x86_64.AppImage ]; then
    wget -q https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x appimagetool-x86_64.AppImage
fi

echo "== pack =="
export APPIMAGE_EXTRACT_AND_RUN=1
ARCH=x86_64 ./appimagetool-x86_64.AppImage "$APPDIR" "$OUT/$NAME"
chmod +x "$OUT/$NAME"

echo
echo "built $OUT/$NAME"
ls -la "$OUT/$NAME"
sha256sum "$OUT/$NAME"
echo
echo "Ship $OUT/$NAME. The player drops GData beside it and runs it."
