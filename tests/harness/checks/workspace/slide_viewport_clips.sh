#!/usr/bin/env bash
# A sliding workspace is a live viewport: content overhanging its own output-sized region must not paint into the
# region the incoming workspace has already uncovered.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly TRANSITION_SHOT="$UMBRIEL_RUNTIME_DIR/slide-viewport-clip.png"

spawn_client() {
  "$CLIENT" "$1" 1200 400 > /dev/null 2>&1 &
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
  local image=$1 x=$2 y=$3
  magick "$image" -crop "20x20+$x+$y" -format '%[fx:round(255*mean.b)]' info:
}

frame_blue() {
  magick "$1" -format '%[fx:round(255*mean.b)]' info:
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 200
curve = "linear"

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[output."HEADLESS-1"]
workspaces = 2
workspace_axis = "horizontal"

[[window_rule]]
match.title = "^slide-overhang$"
default_floating = true
default_floating_size_px = { width = 1200, height = 400 }
default_position = { x = 1000, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

# The window reaches past the right output edge, so the outgoing workspace has
# content outside the viewport it slides with.
spawn_client slide-overhang
wait_for_count 1
"$UMBRIEL" settle

# Only the slide itself is slow: the window is already resting at its overhang.
sed -i 's/^duration_ms = 200$/duration_ms = 10000/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

# Horizontal workspaces slide along X, so the window travels left with its
# workspace while the uncovered right edge belongs to the empty incoming one. The
# visible slice of the window stays 280 logical pixels wide at any progress, so
# neither sample depends on where the eased slide has reached.
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" clock-advance 1000
grim "$TRANSITION_SHOT"
onscreen=$(frame_blue "$TRANSITION_SHOT")
uncovered=$(sample_blue "$TRANSITION_SHOT" 1250 200)
if ((onscreen < 5)); then
  echo "the outgoing window left the screen before the sample: frame blue=$onscreen"
  exit 1
fi
if ((uncovered > 10)); then
  echo "overhanging content painted into the uncovered incoming workspace: blue=$uncovered at x=1250"
  exit 1
fi

echo "a sliding workspace clips its overhanging content to the viewport it travels with"
