#!/bin/bash
# Re-renders the theme artwork from the game itself:
#   backgrounds/1-4  one still per stage (used when the game isn't installed)
#   preview.png      theme picker preview (stage 1 boss fight)
#   unlock.png       the title logo on transparency
# Requires ImageMagick.

set -euo pipefail

BIN=${1:?usage: render-art.sh <spread-shot-binary> <theme-dir>}
THEME=${2:?usage: render-art.sh <spread-shot-binary> <theme-dir>}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$THEME/backgrounds"
names=(jungle snowfield enemy-base alien-hive)
for i in 1 2 3 4; do
  "$BIN" --snapshot "$tmp/bg$i.ppm" --stage "$i" --ticks 900 --scale 8 --no-hud
  magick "$tmp/bg$i.ppm" "$THEME/backgrounds/$i-${names[i - 1]}.png"
done

"$BIN" --snapshot "$tmp/preview.ppm" --stage 1 --boss --ticks 200 --scale 4
magick "$tmp/preview.ppm" "$THEME/preview.png"

"$BIN" --snapshot "$tmp/logo.ppm" --logo-only --scale 4
magick "$tmp/logo.ppm" -transparent black -trim +repage -background none -gravity center -extent 800x378 \
  "$THEME/unlock.png"

ls -la "$THEME/backgrounds" "$THEME/preview.png" "$THEME/unlock.png"
