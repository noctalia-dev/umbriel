#!/usr/bin/env bash
set -euo pipefail

for axis in vertical horizontal; do
  cat > "$UMBRIEL_CONFIG" <<EOF
[general]
xwayland = false
show_cheatsheet = false
autostart = []

[animation]
enabled = false

[overview]
scroll_factor = 3.0
scroll_factor_horizontal = 1.0
scroll_factor_vertical = 0.5

[output."HEADLESS-1"]
workspaces = 3
workspace_axis = "$axis"
EOF
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL_POINTER_CLIENT" 1280 720 move 640 360
  "$UMBRIEL" msg overview-open > /dev/null
  "$UMBRIEL_POINTER_CLIENT" 1280 720 axis "$axis" 250 pause 200 axis-stop "$axis"
  expected=1
  [[ $axis == horizontal ]] && expected=2
  actual=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .index')
  [[ $actual == "$expected" ]] || {
    echo "$axis factor selected workspace $actual, expected $expected"
    exit 1
  }
done

echo 'physical-axis factors override the shared fallback on either workspace arrangement'
