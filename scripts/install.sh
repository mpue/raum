#!/bin/sh
# Build raum and install it for this user: ~/.local/bin/raum, the icon, and a
# menu entry that starts it nested, fullscreen, on the running desktop (its
# right-click action: in a window).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
cmake -S "$ROOT" -B "$ROOT/build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$ROOT/build" --target raum
install -Dm755 "$ROOT/build/raum" "$PREFIX/bin/raum"
install -Dm755 "$ROOT/scripts/raum-session" "$PREFIX/bin/raum-session"
install -Dm644 "$ROOT/data/raum.svg" "$PREFIX/share/icons/hicolor/scalable/apps/raum.svg"
mkdir -p "$PREFIX/share/applications"
cat > "$PREFIX/share/applications/raum.desktop" <<DESKTOP
[Desktop Entry]
Name=raum
GenericName=3D-Desktop
Comment=Fenster im Raum, Projekte als Räume -- in einem Fenster auf diesem Desktop
Exec=$PREFIX/bin/raum --fullscreen
Icon=raum
Terminal=false
Type=Application
Categories=System;Utility;
StartupWMClass=raum
Actions=fenster;

[Desktop Action fenster]
Name=Im Fenster starten
Exec=$PREFIX/bin/raum
DESKTOP
gtk-update-icon-cache -q -t "$PREFIX/share/icons/hicolor" 2>/dev/null || true
echo "raum ist installiert: $PREFIX/bin/raum (und im Programmmenü)."
