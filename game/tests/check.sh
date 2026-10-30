#!/bin/bash
# Headless gameplay checks (no display needed):
#   - the AI demo clears all four stages and bosses, the ending plays, and loop 2 begins
#   - a run with nobody at the keys loses every life and reaches game over
#   - snapshots render

set -euo pipefail

BIN=${1:-./spread-shot}
fail() {
  echo "FAIL: $*" >&2
  exit 1
}

out=$("$BIN" --simulate 24000 --verbose 2>&1)
for n in 1 2 3 4; do
  grep -q "stage $n: boss defeated" <<<"$out" || fail "stage $n boss was never defeated"
done
grep -q "mission complete" <<<"$out" || fail "the ending never played"
[[ $(grep -c "stage 1: JUNGLE" <<<"$out") -ge 2 ]] || fail "the game did not loop back to stage 1"
echo "ok - all stages cleared, ending played, loop 2 started"

out=$("$BIN" --simulate 9000 --idle-player --verbose 2>&1)
grep -q "game over" <<<"$out" || fail "an idle player never reached game over"
echo "ok - idle player reaches game over"

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
"$BIN" --snapshot "$tmp" --stage 2 --boss --ticks 120
[[ $(head -c 2 "$tmp") == P6 ]] || fail "snapshot is not a PPM image"
echo "ok - snapshot renders"
