#!/bin/sh
# Run raum headless (no window, no host desktop needed) with a script and keep
# the screenshots it takes. Its own config dir, so the real rooms stay as
# they are.  tests/run.sh [script]   (default: tests/tour.script)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPT="$(realpath "${1:-$ROOT/tests/tour.script}")"
OUT="$ROOT/build/shots"
CFG="$(mktemp -d)"
mkdir -p "$OUT"
cd "$OUT"
RAUM_CONFIG_DIR="$CFG" WLR_BACKENDS=headless timeout 120 "$ROOT/build/raum" --size 1920x1080 \
    --script "$SCRIPT" > "$OUT/run.log" 2>&1 || { echo "raum ist nicht sauber beendet (siehe $OUT/run.log)"; exit 1; }
rm -rf "$CFG"
grep -c "screenshot" "$OUT/run.log" | xargs printf "%s Bildschirmfotos in $OUT\n"
