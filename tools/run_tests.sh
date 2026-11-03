#!/bin/zsh
# Runs each test ROM in tests/ headless and saves a PNG screenshot in build/test-output/.
# Usage: tools/run_tests.sh [frames]
set -e
ROOT="${0:A:h:h}"
APP="$ROOT/build/LaLuneDoree.app/Contents/MacOS/LaLuneDoree"
OUT="$ROOT/build/test-output"
FRAMES="${1:-120}"
mkdir -p "$OUT"

for rom in "$ROOT"/tests/*.gba; do
  name="${rom:t:r}"
  "$APP" "$rom" --headless "$FRAMES" --dump "$OUT/$name.bmp" > "$OUT/$name.log" 2>&1 || echo "$name: emulator exited with an error"
  sips -s format png "$OUT/$name.bmp" --out "$OUT/$name.png" > /dev/null 2>&1
  rm -f "$OUT/$name.bmp"
  echo "$name: $(tail -1 "$OUT/$name.log")"
done
