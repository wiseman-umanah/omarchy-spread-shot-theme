#!/bin/bash
# Removes everything install.sh added. The saved high score is left in place.

set -euo pipefail

BINDIR=${BINDIR:-$HOME/.local/bin}
HOOKS=$HOME/.config/omarchy/hooks
BINDINGS=$HOME/.config/hypr/bindings.lua

pkill -x spread-shot 2>/dev/null || true

rm -fv "$BINDIR/spread-shot" "$HOOKS/theme-set.d/spread-shot-wallpaper" "$HOOKS/post-boot.d/spread-shot-wallpaper"

if grep -qF -- "-- >>> spread-shot" "$BINDINGS" 2>/dev/null; then
  cp "$BINDINGS" "$BINDINGS.bak.$(date +%s)"
  # Drop the marked block and the blank line install.sh put before it.
  awk '/^-- >>> spread-shot$/ { skip = 1; if (blank) blank = 0; next }
       skip && /^-- <<< spread-shot$/ { skip = 0; next }
       skip { next }
       { if (blank) print ""; blank = ($0 == ""); if (!blank) print }
       END { if (blank) print "" }' "$BINDINGS" >"$BINDINGS.tmp"
  mv "$BINDINGS.tmp" "$BINDINGS"
  hyprctl reload >/dev/null 2>&1 || true
  echo "Removed the Spread Shot keybinding from $BINDINGS"
fi

state=${XDG_STATE_HOME:-$HOME/.local/state}/spread-shot
[[ -d $state ]] && echo "Kept your high score in $state (delete it to reset)."
echo "Spread Shot game uninstalled."
