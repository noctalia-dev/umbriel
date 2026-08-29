#!/usr/bin/env bash
# harness: outputs=1
# The width/height verbs resize a focused float as usable-area fractions instead
# of no-opping through layout columns, and a resized float leaves maximized
# state behind rather than snapping back on the next toggle. The default
# 1280x720 output keeps the expected pixels exact; a post-boot mode reload races
# the first client commit's usable-area snapshot, so no custom mode here.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/fractional-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/float-resize.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[[window_rule]]
match.title = "^float-resize$"
default_floating = true
default_width = 0.5
default_height = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

field_of() {
  "$UMBRIEL" windows --json \
    | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

wait_for_field() {
  local title=$1 field=$2 expected=$3 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$title" "$field")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' field '$field' to be '$expected', got '$actual'"
  return 1
}

"$CLIENT" float-resize > "$CLIENT_LOG" 2>&1 &

wait_for_field float-resize floating true
wait_for_field float-resize w 640
wait_for_field float-resize h 360

"$UMBRIEL" msg "window-focus-warp:$(field_of float-resize id)" > /dev/null

# modify: 0.5 + 0.1 of 1280 -> 768
"$UMBRIEL" msg window-modify-width:0.1 > /dev/null
wait_for_field float-resize w 768
# cycle: the next preset past 0.6 is 2/3 of 1280 -> 853
"$UMBRIEL" msg window-cycle-width > /dev/null
wait_for_field float-resize w 853
# cycle back: the previous preset under 2/3 is 0.5 of 1280 -> 640
"$UMBRIEL" msg window-cycle-width-back > /dev/null
wait_for_field float-resize w 640
# cycle: the next preset past 0.5 is 2/3 of 720 -> 480
"$UMBRIEL" msg window-cycle-height > /dev/null
wait_for_field float-resize h 480
# cycle back: the previous preset under 2/3 is 0.5 of 720 -> 360
"$UMBRIEL" msg window-cycle-height-back > /dev/null
wait_for_field float-resize h 360

# Set assigns the fraction outright on the named axis only.
"$UMBRIEL" msg window-set-width:0.25 > /dev/null
wait_for_field float-resize w 320
wait_for_field float-resize h 360

# A resized float must leave maximized state behind, not carry it silently. The
# transition proves it: toggling after the resize has to maximize. If the resize
# left the flag set, this toggle restores the pre-maximize box instead.
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
wait_for_field float-resize w 1280
wait_for_field float-resize h 720
"$UMBRIEL" msg window-modify-width:-0.2 > /dev/null
wait_for_field float-resize w 1024
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
wait_for_field float-resize w 1280
wait_for_field float-resize h 720

echo "resize verbs drive floating windows and clear maximized state"
