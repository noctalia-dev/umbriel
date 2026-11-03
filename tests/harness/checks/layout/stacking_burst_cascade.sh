#!/usr/bin/env bash
# A burst of maps keeps cascading after the first diagonal reaches the output boundary.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[layout]
mode = "stacking"
[appearance.title_bar]
mode = "forced"
height = 28
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
for i in $(seq 20); do
  "$UMBRIEL_FRACTIONAL_CLIENT" "burst-$i" 480 300 >"$UMBRIEL_RUNTIME_DIR/burst-$i.log" 2>&1 &
done
for _ in $(seq 160); do
  count=$("$UMBRIEL" windows --json | jq '[.[] | select(.title | startswith("burst-"))] | length')
  [[ $count == 20 ]] && break
  sleep 0.025
done
[[ $count == 20 ]]
"$UMBRIEL" settle >/dev/null
windows=$("$UMBRIEL" windows --json)
unique=$(jq '[.[] | select(.title | startswith("burst-")) | [.x, .y]] | unique | length' <<<"$windows")
[[ $unique == 20 ]] || {
  echo "20 rapid maps produced only $unique distinct origins: $windows"
  exit 1
}
jq -e 'all(.[]; .x >= 0 and .y >= 28 and .x + .w <= 1280 and .y + .h <= 720)' <<<"$windows" >/dev/null
echo "20 simultaneous floating maps cascade to distinct reachable origins"
