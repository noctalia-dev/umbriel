#!/usr/bin/env bash
# The default_maximize_to_edges window rule must expand a matching window to the
# usable-area edges at map time. A window with the same layout width fraction but
# no matching rule stays at that fraction, so the widening is attributable to the
# rule rather than to any unrelated full-width fallback. A solid client on the
# black headless output makes the presented width measurable. Master and dwindle
# restore existing edge-maximized tiles when another tile joins.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly BASELINE="$UMBRIEL_RUNTIME_DIR/initial-maximize-to-edges-baseline.png"
readonly RULED="$UMBRIEL_RUNTIME_DIR/initial-maximize-to-edges-ruled.png"
readonly MAXIMIZED_LOG="$UMBRIEL_RUNTIME_DIR/initial-maximize-ruled.log"
readonly EDGES_LOG="$UMBRIEL_RUNTIME_DIR/initial-maximize-to-edges-ruled.log"

# Pixels of the client's #5577AA fill, within 2% per channel, across the output's middle row.
presented_width() {
  "$UMBRIEL_PIXEL_PROBE" "$1" count \
    'r > 0.313 && r < 0.353 && g > 0.447 && g < 0.487 && b > 0.647 && b < 0.687' 1280x1+0+360
}
assert_stable_initial_size() {
  local log=$1 label=$2 first last
  first=$(grep '^configured-size=' "$log" | sed -n '1p')
  last=$(grep '^configured-size=' "$log" | sed -n '$p')
  if [[ -z $first || $first != "$last" ]]; then
    echo "$label opened with a different size than its final configure: first=${first:-missing} last=${last:-missing}"
    return 1
  fi
}

spawn_client() {
  local title=$1 log=$2
  shift 2
  env LOG_CONFIGURES=1 "$@" "$CLIENT" "$title" 1280 720 > "$log" 2>&1 &
  CLIENT_PID=$!
  for _ in $(seq 60); do
    grep -q '^mapped$' "$log" && return 0
    sleep 0.05
  done
  echo "$title client never mapped: $(cat "$log")"
  return 1
}

stop_client() {
  kill -KILL "$CLIENT_PID" 2>/dev/null || true
  wait "$CLIENT_PID" 2>/dev/null || true
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1
curve = "linear"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[layout.scrolling]
default_extent_fraction = 0.5

[output.HEADLESS-1]
workspaces = 3

[[window_rule]]
match.title = "^maximize-ruled$"
default_maximize = true

[[window_rule]]
match.title = "^edges-ruled$"
default_maximize_to_edges = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

# The unmatched window keeps the half-width fraction: the measurement baseline.
spawn_client edges-baseline "$UMBRIEL_RUNTIME_DIR/initial-maximize-to-edges-baseline.log" || exit 1
"$UMBRIEL" settle
grim "$BASELINE"
baseline_width=$(presented_width "$BASELINE")
stop_client
if (( baseline_width > 900 )); then
  echo "baseline window was not laid out at the half-width fraction: width=$baseline_width"
  exit 1
fi
readonly MAXIMIZED_SCREENSHOT="$UMBRIEL_RUNTIME_DIR/initial-maximize-ruled.png"
spawn_client maximize-ruled "$MAXIMIZED_LOG" || exit 1
"$UMBRIEL" settle
grim "$MAXIMIZED_SCREENSHOT"
maximized_width=$(presented_width "$MAXIMIZED_SCREENSHOT")
stop_client
assert_stable_initial_size "$MAXIMIZED_LOG" "default_maximize"
if (( maximized_width < 1100 || maximized_width - baseline_width < 400 )); then
  echo "default_maximize did not expand the window to full column width: baseline=$baseline_width maximized=$maximized_width"
  exit 1
fi

# Same fraction, but the rule maximizes it to the edges at map.
spawn_client edges-ruled "$EDGES_LOG" || exit 1
"$UMBRIEL" settle
grim "$RULED"
ruled_width=$(presented_width "$RULED")
assert_stable_initial_size "$EDGES_LOG" "default_maximize_to_edges"

if (( ruled_width < 1100 || ruled_width - baseline_width < 400 )); then
  echo "default_maximize_to_edges did not expand the window to the edges at map: baseline=$baseline_width ruled=$ruled_width"
  exit 1
fi

echo "maximize rules used one initial size and expanded mapped windows: $baseline_width -> $maximized_width -> $ruled_width"

window_id() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | .id'
}

maximize_first() {
  "$UMBRIEL" msg window-focus "$first_id" > /dev/null
  "$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
  "$UMBRIEL" settle
  if ! "$UMBRIEL" windows --json | jq -e --arg title "$first" '
    any(.[]; .title == $title and .maximized_to_edges and .x == 0 and .y == 0 and .w == 1280 and .h == 720)
  ' > /dev/null; then
    echo "$mode first tile did not maximize to edges"
    return 1
  fi
}

assert_tiled_pair() {
  local windows image expected actual title predicate
  "$UMBRIEL" settle
  windows=$("$UMBRIEL" windows --json)
  if ! jq -e --arg first "$first" --arg second "$second" '
    [.[] | select(.title == $first or .title == $second)] as $pair
    | ($pair | length) == 2
      and all($pair[]; .floating == false and .maximized_to_edges == false and .maximized == false)
      and ($pair[0].x + $pair[0].w <= $pair[1].x
        or $pair[1].x + $pair[1].w <= $pair[0].x
        or $pair[0].y + $pair[0].h <= $pair[1].y
        or $pair[1].y + $pair[1].h <= $pair[0].y)
  ' <<< "$windows" > /dev/null; then
    echo "$mode did not restore nonoverlapping tiles after $arrival: $windows"
    return 1
  fi
  image="$UMBRIEL_RUNTIME_DIR/$mode-$arrival.png"
  grim "$image"
  for title in "$first" "$second"; do
    predicate='r > 0.9 && g < 0.1 && b < 0.1'
    [[ $title == "$second" ]] && predicate='r < 0.1 && g < 0.1 && b > 0.9'
    expected=$(jq -r --arg title "$title" '.[] | select(.title == $title) | "\(.x) \(.y) \(.w) \(.h)"' <<< "$windows")
    actual=$("$UMBRIEL_PIXEL_PROBE" "$image" bbox "$predicate")
    if [[ $actual != "$expected" ]]; then
      echo "$mode $title painted '$actual' after $arrival, expected its tile '$expected'"
      return 1
    fi
  done
}

workspace=1
for mode in master dwindle; do
  workspace=$((workspace + 1))
  "$UMBRIEL" msg workspace-switch "$workspace" > /dev/null
  "$UMBRIEL" msg workspace-set-layout "$mode" > /dev/null
  first="$mode-edges-first"
  second="$mode-edges-second"
  spawn_client "$first" "$UMBRIEL_RUNTIME_DIR/$first.log" FOLLOW_CONFIGURES=1 FILL_COLOR=0xFFFF0000
  "$UMBRIEL" settle
  first_id=$(window_id "$first")
  maximize_first

  spawn_client "$second" "$UMBRIEL_RUNTIME_DIR/$second.log" FOLLOW_CONFIGURES=1 FILL_COLOR=0xFF0000FF
  "$UMBRIEL" settle
  second_id=$(window_id "$second")
  arrival=opening
  assert_tiled_pair

  # Retiling an existing floating peer also restores ordinary tiled placement.
  "$UMBRIEL" msg window-toggle-floating > /dev/null
  maximize_first
  "$UMBRIEL" msg window-focus "$second_id" > /dev/null
  "$UMBRIEL" msg window-toggle-floating > /dev/null
  arrival=retiling
  assert_tiled_pair

  # A tile moving between workspaces takes the same path; the scrolling source keeps its own edge maximize.
  "$UMBRIEL" msg window-move-to-workspace 1 > /dev/null
  maximize_first
  "$UMBRIEL" msg window-focus "$second_id" > /dev/null
  "$UMBRIEL" msg window-move-to-workspace "$workspace" > /dev/null
  "$UMBRIEL" msg workspace-switch "$workspace" > /dev/null
  arrival=moving
  assert_tiled_pair
  if ! "$UMBRIEL" windows --json | jq -e 'any(.[]; .title == "edges-ruled" and .maximized_to_edges)' > /dev/null; then
    echo "a tile arriving in the scrolling workspace cleared its edge maximize"
    exit 1
  fi

  # A floating edge maximize remains intact when a tile joins beside it.
  "$UMBRIEL" msg window-focus "$second_id" > /dev/null
  "$UMBRIEL" msg window-toggle-floating > /dev/null
  "$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
  "$UMBRIEL" settle
  "$UMBRIEL" msg window-focus "$first_id" > /dev/null
  "$UMBRIEL" msg window-toggle-floating > /dev/null
  "$UMBRIEL" msg window-toggle-floating > /dev/null
  "$UMBRIEL" settle
  if ! "$UMBRIEL" windows --json | jq -e --arg title "$second" '
    any(.[]; .title == $title and .floating and .maximized_to_edges)
  ' > /dev/null; then
    echo "$mode cleared a floating edge maximize when a tile joined"
    exit 1
  fi
done

echo "master and dwindle restored tiled geometry and pixels after opening, retiling, and moving another tile in"
