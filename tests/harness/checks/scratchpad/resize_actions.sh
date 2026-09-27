#!/usr/bin/env bash
# harness: outputs=1
# Primary and secondary extent actions follow the active scratchpad float
# instead of the workspace focus retained behind it. Every distinct resize handler is covered,
# and the background box must remain stable throughout.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly BACKGROUND=scratchpad-resize-background
readonly SCRATCHPAD=scratchpad-resize-foreground
readonly BACKGROUND_LOG="$UMBRIEL_RUNTIME_DIR/scratchpad-resize-background.log"
readonly SCRATCHPAD_LOG="$UMBRIEL_RUNTIME_DIR/scratchpad-resize-foreground.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[animation.scratchpad]
scale = 0

[[window_rule]]
match.title = "^scratchpad-resize-background$"
default_floating = true
default_floating_size = { width = 0.5, height = 0.5 }

[[window_rule]]
match.title = "^scratchpad-resize-foreground$"
default_floating = true
default_floating_size_px = { width = 420, height = 260 }
EOF
"$UMBRIEL" msg config-reload > /dev/null

windows() { "$UMBRIEL" windows --json; }

field_of() {
  windows | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

wait_for_field() {
  local title=$1 field=$2 expected=$3 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$title" "$field")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' field '$field' to be '$expected', got '$actual': $(windows)"
  return 1
}

assert_background_box() {
  local action=$1 state box
  state=$(windows)
  box=$(jq -r --arg title "$BACKGROUND" '.[] | select(.title == $title) | "\(.w)x\(.h)"' <<< "$state")
  if [[ $box != 640x360 ]]; then
    echo "$action resized the background float: expected 640x360, got $box: $state"
    return 1
  fi
}

resize_scratchpad() {
  local action=$1 expected_width=$2 expected_height=$3 state width height active
  "$UMBRIEL" msg "$action" > /dev/null
  for _ in $(seq 80); do
    assert_background_box "$action"
    state=$(windows)
    width=$(jq -r --arg title "$SCRATCHPAD" '.[] | select(.title == $title) | .w' <<< "$state")
    height=$(jq -r --arg title "$SCRATCHPAD" '.[] | select(.title == $title) | .h' <<< "$state")
    active=$(jq -r --arg title "$SCRATCHPAD" '.[] | select(.title == $title) | .active' <<< "$state")
    if [[ $width == "$expected_width" && $height == "$expected_height" && $active == true ]]; then
      # Keep observing briefly after the configure arrives so a delayed client
      # commit cannot conceal a background resize.
      for _ in $(seq 5); do
        sleep 0.1
        assert_background_box "$action"
      done
      return 0
    fi
    sleep 0.1
  done
  echo "$action did not resize the active scratchpad to ${expected_width}x${expected_height}: $state"
  return 1
}

"$CLIENT" "$BACKGROUND" > "$BACKGROUND_LOG" 2>&1 &
wait_for_field "$BACKGROUND" floating true
wait_for_field "$BACKGROUND" w 640
wait_for_field "$BACKGROUND" h 360

# Positive control: the same action must reach the background while it owns
# focus, then restore the exact baseline used by the isolation assertions.
"$UMBRIEL" msg window-set-primary-extent:0.6 > /dev/null
wait_for_field "$BACKGROUND" w 768
"$UMBRIEL" msg window-set-primary-extent:0.5 > /dev/null
wait_for_field "$BACKGROUND" w 640

"$CLIENT" "$SCRATCHPAD" > "$SCRATCHPAD_LOG" 2>&1 &
wait_for_field "$SCRATCHPAD" floating true
wait_for_field "$SCRATCHPAD" w 420
wait_for_field "$SCRATCHPAD" h 260

scratchpad_id=$(field_of "$SCRATCHPAD" id)
"$UMBRIEL" msg "window-focus:$scratchpad_id" > /dev/null
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
wait_for_field "$SCRATCHPAD" workspace ""
"$UMBRIEL" msg scratchpad-toggle > /dev/null
wait_for_field "$SCRATCHPAD" active true
assert_background_box setup

# Direct setters, relative changes, and preset cycling have separate handlers.
# Exercise both axes and both cycle directions while the scratchpad owns focus.
resize_scratchpad window-set-primary-extent:0.25 320 260
resize_scratchpad window-modify-primary-extent:+0.1 448 260
resize_scratchpad window-cycle-primary-extent 640 260
resize_scratchpad window-cycle-primary-extent-back 427 260
resize_scratchpad window-set-secondary-extent:0.25 427 180
resize_scratchpad window-modify-secondary-extent:+0.1 427 252
resize_scratchpad window-cycle-secondary-extent 427 360
resize_scratchpad window-cycle-secondary-extent-back 427 240

echo "resize actions target the active scratchpad float without changing the background"
