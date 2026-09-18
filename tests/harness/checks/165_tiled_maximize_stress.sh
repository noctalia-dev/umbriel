#!/usr/bin/env bash
# Slow tiled maximize and restore transitions must keep every visible tile disjoint when new actions arrive before an
# earlier transition settles. Two half-red client presentations produce a bright red overlap.
set -euo pipefail

readonly CLIENT="$UMBRIEL_UNMAP_CLIENT"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/tiled-maximize-stress.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
background = "#000000FF"

[colors.border]
focused = "#000000FF"
unfocused = "#000000FF"
outer = "#000000FF"

[appearance]
border_width = 2
outer_border_width = 13
corner_radius = 0

[appearance.shadow]
enabled = false

[layout]
mode = "dwindle"
gap = 5

[animation]
duration_ms = 3000
curve = "maximize_flow"

[animation.beziers]
maximize_flow = [0.25, 0.46, 0.35, 1.0]

[animation.windows_in]
enabled = false

[animation.windows_move]
duration_ms = 3000
curve = "maximize_flow"
EOF
"$UMBRIEL" msg config-reload > /dev/null

wait_for_count() {
  local want=$1
  for _ in $(seq 1 120); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.025
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

spawn_red() {
  local title=$1
  FILL_COLOR=0x80800000 "$CLIENT" "$title" 1200 700 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
}

window_id() {
  local title=$1
  "$UMBRIEL" windows --json | jq -r --arg title "$title" '.[] | select(.title == $title) | .id'
}

focus_and_toggle() {
  local title=$1 id
  id=$(window_id "$title")
  if [[ -z $id ]]; then
    echo "could not find maximize stress window '$title'"
    return 1
  fi
  "$UMBRIEL" msg "window-focus:$id" > /dev/null
  "$UMBRIEL" msg window-toggle-maximize > /dev/null
}

assert_no_overlap() {
  local label=$1 overlap
  grim "$SHOT"
  overlap=$(magick "$SHOT" \
    -fx 'r > 0.65 && g < 0.1 && b < 0.1 ? 1 : 0' \
    -format '%[fx:mean]' info:)
  if awk -v overlap="$overlap" 'BEGIN { exit !(overlap > 0.00001) }'; then
    echo "$label: tiled maximize presentations overlap ($overlap of the output)"
    return 1
  fi
}

run_burst() {
  local mode=$1
  focus_and_toggle "$mode-maximize-b"
  sleep 0.2
  focus_and_toggle "$mode-maximize-c"
  sleep 0.2
  focus_and_toggle "$mode-maximize-a"
  sleep 0.2
  focus_and_toggle "$mode-maximize-b"
  sleep 0.2
  focus_and_toggle "$mode-maximize-c"
  sleep 0.2
  focus_and_toggle "$mode-maximize-a"
}

run_layout_stress() {
  local mode=$1
  spawn_red "$mode-maximize-a"
  spawn_red "$mode-maximize-b"
  spawn_red "$mode-maximize-c"
  wait_for_count 3
  sleep 3.2

  run_burst "$mode" &
  local burst_pid=$!
  for frame in $(seq 1 20); do
    assert_no_overlap "$mode maximize stress frame $frame" || {
      wait "$burst_pid" || true
      return 1
    }
    sleep 0.04
  done
  wait "$burst_pid"
  for frame in $(seq 21 36); do
    assert_no_overlap "$mode maximize settle frame $frame"
    sleep 0.4
  done

  local id
  while read -r id; do
    "$UMBRIEL" msg "window-close:$id" > /dev/null
  done < <("$UMBRIEL" windows --json | jq -r '.[].id')
  wait_for_count 0
  sleep 3.2
}

run_layout_stress dwindle
sed -i 's/mode = "dwindle"/mode = "master"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
run_layout_stress master
sed -i 's/mode = "master"/mode = "scrolling"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
run_layout_stress scrolling

echo "slow Dwindle, Master, and scrolling maximize bursts kept every tiled presentation disjoint"
