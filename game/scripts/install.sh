#!/bin/bash
# Installs the Spread Shot wallpaper game and wires it into Omarchy:
#   - the binary into $BINDIR (default ~/.local/bin)
#   - theme-set and post-boot hooks that start it while the spread-shot theme is active
#   - a Super+Ctrl+G keybinding that toggles game mode
# Everything it adds is removed again by uninstall.sh.

set -euo pipefail

BIN_SRC=${1:?usage: install.sh <path-to-spread-shot-binary>}
BINDIR=${BINDIR:-$HOME/.local/bin}
HERE=$(cd "$(dirname "$0")" && pwd)
BINDINGS=$HOME/.config/hypr/bindings.lua
BIND_KEY="SUPER + CTRL + G"
BIND_MARK_BEGIN="-- >>> spread-shot"
BIND_MARK_END="-- <<< spread-shot"

if ! command -v omarchy >/dev/null; then
  echo "spread-shot: Omarchy was not found. The binary still runs on any wlr-layer-shell compositor;" >&2
  echo "install it by hand with: install -Dm755 $BIN_SRC \"$BINDIR/spread-shot\"" >&2
  exit 1
fi

install -Dm755 "$BIN_SRC" "$BINDIR/spread-shot"
echo "Installed $BINDIR/spread-shot"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sed "s|@BINDIR@|$BINDIR|" "$HERE/spread-shot-wallpaper" >"$tmp/spread-shot-wallpaper"
omarchy hook install theme-set "$tmp/spread-shot-wallpaper"
omarchy hook install post-boot "$tmp/spread-shot-wallpaper"

# The keybinding goes between markers so uninstall.sh can remove exactly what was added.
if grep -qF -e "$BIND_MARK_BEGIN" "$BINDINGS" 2>/dev/null; then
  echo "Keybinding already present in $BINDINGS"
elif hyprctl binds -j 2>/dev/null | jq -e '.[] | select(.modmask == 68 and (.key | ascii_downcase) == "g")' >/dev/null; then
  echo "Warning: $BIND_KEY is already bound, so no keybinding was added." >&2
  echo "Bind another key to: pkill -USR1 -x spread-shot" >&2
else
  cp "$BINDINGS" "$BINDINGS.bak.$(date +%s)"
  cat >>"$BINDINGS" <<EOF

$BIND_MARK_BEGIN
-- Spread Shot wallpaper game: toggle game mode (arrow keys play, Esc exits).
o.bind("$BIND_KEY", "Spread Shot game mode", "pkill -USR1 -x spread-shot")
$BIND_MARK_END
EOF
  hyprctl reload >/dev/null
  echo "Added $BIND_KEY to $BINDINGS (backup saved alongside)"
fi

# Start right away if the theme is already applied.
bash "$tmp/spread-shot-wallpaper"

echo
echo "Done. Apply the theme with:  omarchy theme set \"Spread Shot\""
echo "Then press $BIND_KEY to play."
