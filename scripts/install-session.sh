#!/bin/sh
# raum as a session of its own in GDM's session menu (the gear at the login).
# Needs root: the session list is system-wide. Experimental -- see README.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
[ -x "$ROOT/build/raum" ] || { echo "erst bauen: scripts/install.sh"; exit 1; }
sudo install -m 755 "$ROOT/build/raum" /usr/local/bin/raum
sudo install -m 755 "$ROOT/scripts/raum-session" /usr/local/bin/raum-session
sudo install -m 644 "$ROOT/data/raum.desktop" /usr/share/wayland-sessions/raum.desktop
# The user's half: the systemd targets the session binds to and ends with, and
# which portals a "raum" desktop uses (GTK's and the keyring; GNOME's need
# gnome-shell).
install -Dm644 "$ROOT/data/raum-session.target" "$HOME/.config/systemd/user/raum-session.target"
install -Dm644 "$ROOT/data/raum-session-shutdown.target" "$HOME/.config/systemd/user/raum-session-shutdown.target"
install -Dm644 "$ROOT/data/raum-portals.conf" "$HOME/.config/xdg-desktop-portal/raum-portals.conf"
systemctl --user daemon-reload
echo "Abmelden, im Anmeldebildschirm unten rechts das Zahnrad: raum."
