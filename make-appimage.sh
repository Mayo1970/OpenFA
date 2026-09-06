#!/usr/bin/env bash
# Build a self-contained OpenFA-x86_64.AppImage: fa_slice plus its
# whole shared-library closure (SDL2, X11, ALSA, PulseAudio, Wayland, ...),
# minus the parts that must come from the target (glibc core, the GPU stack).
#
#   ./make-appimage.sh [ICON]
#
# ICON is an .ico or .png for the launcher (default: ../master/gesamt.ico, the
# original game icon, else a plain placeholder). Needs: a C compiler,
# pkg-config, SDL2 dev files, wget, and ImageMagick ("convert").
#
# RUN THIS ON THE OLDEST GLIBC YOU WANT TO SUPPORT. The AppImage runs on that
# glibc and every newer one, never older. Ubuntu 20.04 (glibc 2.31) covers
# essentially every desktop still in use.
set -euo pipefail
cd "$(dirname "$0")"

ICON_SRC="${1:-../master/gesamt.ico}"
OUT=dist
APPDIR=AppDir
NAME=OpenFA-x86_64.AppImage
L=/usr/lib/x86_64-linux-gnu

command -v convert >/dev/null || { echo "need ImageMagick (convert)"; exit 1; }
command -v wget    >/dev/null || { echo "need wget"; exit 1; }

echo "== build fa_slice =="
FA_BUNDLE_SDL=0 ./make-linux-build.sh >/dev/null
command -v strip >/dev/null && strip "$OUT/fa_slice" || true

echo "== assemble $APPDIR =="
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib"
cp "$OUT/fa_slice" "$APPDIR/usr/bin/fa_slice"

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
copy_closure "$APPDIR/usr/bin/fa_slice"

# SDL2 dlopen's some audio/video backends that are not in NEEDED. Add the
# safe ones (never the GPU libs) and their closures.
for extra in libasound.so.2 libpulse.so.0 libpulse-simple.so.0 libjack.so.0 \
             libpipewire-0.3.so.0 libsndio.so.7 libwayland-client.so.0 \
             libwayland-cursor.so.0 libwayland-egl.so.1 libxkbcommon.so.0 \
             libdecor-0.so.0; do
    [ -e "$L/$extra" ] || continue
    [ -e "$APPDIR/usr/lib/$extra" ] || cp -L "$L/$extra" "$APPDIR/usr/lib/$extra"
    copy_closure "$APPDIR/usr/lib/$extra"
done
echo "   bundled $(ls "$APPDIR/usr/lib" | wc -l) libraries"

cat > "$APPDIR/AppRun" <<'R'
#!/bin/sh
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="${HERE}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
exec "${HERE}/usr/bin/fa_slice" "$@"
R
chmod +x "$APPDIR/AppRun"

cat > "$APPDIR/openfa.desktop" <<'D'
[Desktop Entry]
Type=Application
Name=OpenFA
Comment=Kinder & Ferrero - Fresh Adventures (OpenFA engine)
Exec=fa_slice
Icon=openfa
Categories=Game;
Terminal=false
D

if [ -f "$ICON_SRC" ]; then
    convert "${ICON_SRC}[0]" -resize 256x256 -background none -gravity center \
        -extent 256x256 "$APPDIR/openfa.png"
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
