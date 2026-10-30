#!/bin/bash
# Renders a gameplay trailer straight from the game (no screen recording):
#   <out>.mp4  ~45 s at 1368x768, H.264, for social posts
#   <gif>      a short loop for the README
# Usage: render-trailer.sh <spread-shot-binary> <out.mp4> <out.gif>
# Requires ffmpeg.

set -euo pipefail

BIN=${1:?usage: render-trailer.sh <binary> <out.mp4> <out.gif>}
MP4=${2:?usage: render-trailer.sh <binary> <out.mp4> <out.gif>}
GIF=${3:?usage: render-trailer.sh <binary> <out.mp4> <out.gif>}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# segment <name> <frames> <spread-shot args...>
segment() {
  local name=$1 frames=$2
  shift 2
  "$BIN" "$@" --record "$frames" --scale 4 |
    ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s 1368x768 -r 30 -i - \
      -c:v libx264 -preset slow -crf 16 -pix_fmt yuv420p "$tmp/$name.mp4"
  echo "$tmp/$name.mp4" >>"$tmp/list"
}

: >"$tmp/list"
segment 1-run 210 --stage 1 --ticks 200            # title logo, running and gunning
segment 2-warning 150 --stage 1 --boss --ticks 20  # WARNING! and the wall rolls in
segment 3-wall-down 120 --stage 1 --boss --ticks 690
segment 4-tank 150 --stage 2 --boss --ticks 140
segment 5-reactor 150 --stage 3 --boss --ticks 120
segment 6-heart 150 --stage 4 --boss --ticks 100
segment 7-ending 400 --stage 4 --boss --ticks 1100 # final boss falls, MISSION COMPLETE

sed 's/^/file /' "$tmp/list" >"$tmp/concat.txt"
ffmpeg -loglevel error -y -f concat -safe 0 -i "$tmp/concat.txt" -c copy -movflags +faststart "$MP4"

"$BIN" --stage 1 --ticks 200 --record 180 --scale 2 |
  ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s 684x384 -r 30 -i - \
    -vf "fps=15,split[a][b];[a]palettegen=max_colors=64[p];[b][p]paletteuse=dither=none" "$GIF"

ls -la "$MP4" "$GIF"
