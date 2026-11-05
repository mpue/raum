#!/bin/sh
# raum as a session of its own in GDM's session menu (the gear at the login).
# Needs root: the session list is system-wide. Experimental -- see README.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ -x "$ROOT/build/raum" ] || { echo "erst bauen: scripts/install.sh"; exit 1; }
sudo install -m 755 "$ROOT/build/raum" /usr/local/bin/raum
sudo install -m 755 "$ROOT/scripts/raum-session" /usr/local/bin/raum-session
sudo install -m 644 "$ROOT/data/raum.desktop" /usr/share/wayland-sessions/raum.desktop
echo "Abmelden, im Anmeldebildschirm unten rechts das Zahnrad: raum."
