#!/usr/bin/env bash
# A tall vertical strip is presented as one live viewport during workspace slides and inside each overview row.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly OVERVIEW_SHOT="$UMBRIEL_RUNTIME_DIR/vertical-overview-clip.png"
readonly TRANSITION_SHOT="$UMBRIEL_RUNTIME_DIR/vertical-transition-clip.png"

spawn_client() {
  "$CLIENT" "$1" 1200 700 > /dev/null 2>&1 &
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

sample_blue() {
  local image=$1 y=$2
  magick "$image" -crop "20x20+630+$y" -format '%[fx:round(255*mean.b)]' info:
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1000

[colors]
backdrop = "#000000FF"

[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[overview]
zoom = 0.5

[layout.scrolling]
direction = "vertical"

[output."HEADLESS-1"]
workspaces = 2
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client vertical-clip-first
wait_for_count 1
spawn_client vertical-clip-second
wait_for_count 2
spawn_client vertical-clip-third
wait_for_count 3
first_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "vertical-clip-first") | .id')
if [[ -z $first_id ]]; then
  echo "could not resolve the first vertical lane: $("$UMBRIEL" windows --json)"
  exit 1
fi
"$UMBRIEL" msg "window-focus:$first_id" > /dev/null
sleep 0.2

"$UMBRIEL" msg overview-open > /dev/null
sleep 1.1
grim "$OVERVIEW_SHOT"
overview_visible=$(sample_blue "$OVERVIEW_SHOT" 350)
overview_overflow=$(sample_blue "$OVERVIEW_SHOT" 600)
if (( overview_visible < 80 )); then
  echo "overview did not render the visible vertical lane: blue=$overview_visible"
  exit 1
fi
if (( overview_overflow > 10 )); then
  echo "vertical lane painted into the next overview row: blue=$overview_overflow"
  exit 1
fi

"$UMBRIEL" msg overview-close > /dev/null
sleep 1.1
sed -i 's/duration_ms = 1000/duration_ms = 10000/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg workspace-switch:2 > /dev/null
sleep 1
grim "$TRANSITION_SHOT"
transition_overflow=$(sample_blue "$TRANSITION_SHOT" 600)
if (( transition_overflow > 10 )); then
  echo "hidden vertical lane entered the outgoing workspace transition: blue=$transition_overflow"
  exit 1
fi

echo "vertical overview rows and workspace transitions clip to their live viewport"
