#!/usr/bin/env bash
# Install a built OpenFA into a prefix: the binary, a .desktop entry and the
# full set of hicolor icon sizes. Run make-linux-build.sh first.
#
#   ./make-linux-install.sh [PREFIX]
#
# PREFIX defaults to ~/.local (a user install, no root needed). Use /usr or
# /usr/local for a system install (run with sudo).
#
# Uninstall: delete
#   $PREFIX/bin/OpenFA
#   $PREFIX/share/applications/openfa.desktop
#   $PREFIX/share/icons/hicolor/*/apps/openfa.png
set -euo pipefail
cd "$(dirname "$0")"

PREFIX="${1:-$HOME/.local}"
BIN="dist/OpenFA"

[ -x "$BIN" ] || { echo "no $BIN - run ./make-linux-build.sh first"; exit 1; }

echo "== install into $PREFIX =="
install -Dm755 "$BIN" "$PREFIX/bin/OpenFA"

install -Dm644 /dev/stdin "$PREFIX/share/applications/openfa.desktop" <<'D'
[Desktop Entry]
Type=Application
Name=OpenFA
Comment=Kinder & Ferrero - Fresh Adventures (OpenFA engine)
Exec=OpenFA
Icon=openfa
Categories=Game;
Terminal=false
D

for s in 16 24 32 48 64 128 256 512; do
  src="src/icon/icon_${s}.png"
  [ -f "$src" ] || continue
  install -Dm644 "$src" \
    "$PREFIX/share/icons/hicolor/${s}x${s}/apps/openfa.png"
done

# Refresh the icon cache so the launcher picks the new icon up now.
if command -v gtk-update-icon-cache >/dev/null; then
  gtk-update-icon-cache -f -t "$PREFIX/share/icons/hicolor" 2>/dev/null || true
fi
if command -v update-desktop-database >/dev/null; then
  update-desktop-database "$PREFIX/share/applications" 2>/dev/null || true
fi

echo
echo "installed:"
echo "  $PREFIX/bin/OpenFA"
echo "  $PREFIX/share/applications/openfa.desktop"
echo "  $PREFIX/share/icons/hicolor/*/apps/openfa.png"
echo
echo "If $PREFIX/bin is not on PATH, add it. GData still loads from the"
echo "working dir or --gdata; the install does not bundle game data."
