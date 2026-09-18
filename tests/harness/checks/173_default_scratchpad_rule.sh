#!/usr/bin/env bash
# harness: outputs=1
# A default_scratchpad window rule stores a matching window without showing or
# focusing it, preserves its opening floating position, and lets the ordinary
# scratchpad action summon it afterwards.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_APP_ID=scratchpad-terminal
readonly TITLE=default-scratchpad-rule

windows() { "$UMBRIEL" windows --json; }

wait_for_state() {
  local active=$1 state=
  for _ in $(seq 80); do
    state=$(windows)
    if jq -e --arg app_id "$CLIENT_APP_ID" --argjson active "$active" '
      any(.[];
        .app_id == $app_id
        and .scratchpad == "term"
        and .workspace == ""
        and .floating == true
        and .active == $active
        and .focused == false)
    ' <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected '$CLIENT_APP_ID' in the term scratchpad with active=$active: $state"
  return 1
}

wait_for_geometry() {
  local x=$1 y=$2 width=$3 height=$4 state=
  for _ in $(seq 80); do
    state=$(windows)
    if jq -e \
      --arg app_id "$CLIENT_APP_ID" \
      --argjson x "$x" \
      --argjson y "$y" \
      --argjson width "$width" \
      --argjson height "$height" '
      any(.[];
        .app_id == $app_id
        and .x == $x
        and .y == $y
        and .w == $width
        and .h == $height)
    ' <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected '$CLIENT_APP_ID' at ${width}x${height}+${x}+${y}: $state"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[animation.scratchpad]
scale = 0.0
maximize = false
fullscreen = false

[[scratchpad]]
name = "term"

[[window_rule]]
match.app_id = "^scratchpad-terminal$"
default_scratchpad = "term"
default_floating = true
default_floating_size = { width = 0.6, height = 0.5 }
default_position = { x = 0, y = 8, anchor = "top" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

APP_ID="$CLIENT_APP_ID" "$CLIENT" "$TITLE" 480 300 > "$UMBRIEL_RUNTIME_DIR/$TITLE.log" 2>&1 &
wait_for_state false
wait_for_geometry 400 8 480 300

"$UMBRIEL" msg scratchpad-toggle:term > /dev/null
wait_for_state true
wait_for_geometry 400 8 480 300

echo "default scratchpad rule preserved its opening floating position"
