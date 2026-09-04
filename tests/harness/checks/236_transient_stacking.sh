#!/usr/bin/env bash
# A parented XDG toplevel stays above a floating, pinned, or fullscreen parent.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/transient-stacking.log"

windows() {
  "$UMBRIEL" windows --json
}

field_of() {
  local id=$1 field=$2
  windows | jq -r --arg id "$id" --arg field "$field" '.[] | select(.id == $id) | .[$field]'
}

wait_for_window_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $(windows | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $(windows)"
  return 1
}

wait_for_field() {
  local id=$1 field=$2 want=$3
  for _ in $(seq 60); do
    [[ $(field_of "$id" "$field") == "$want" ]] && return 0
    sleep 0.1
  done
  echo "expected $id $field=$want, got: $(windows)"
  return 1
}

wait_for_fullscreen() {
  local id=$1 want=$2
  for _ in $(seq 60); do
    if [[ $("$UMBRIEL" tearing --json \
      | jq -r --arg id "$id" '.surfaces[] | select(.id == $id) | .fullscreen') == "$want" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $id fullscreen=$want, got: $("$UMBRIEL" tearing --json)"
  return 1
}

focus_at() {
  local x=$1 y=$2 id=$3 context=$4
  "$POINTER" 1280 720 move "$x" "$y"
  if ! wait_for_field "$id" focused true; then
    echo "$context: $(windows)"
    exit 1
  fi
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[input.focus]
follows_mouse = true

[[window_rule]]
match.title = "^transient-parent$"
default_floating = true
default_position = { x = 100, y = 110, anchor = "top_left" }

[[window_rule]]
match.title = "^transient-child$"
default_floating = true
default_position = { x = 500, y = 110, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

TRANSIENT_SUITE=1 "$CLIENT" transient-child 600 500 > "$CLIENT_LOG" 2>&1 &
wait_for_window_count 3

snapshot=$(windows)
parent_id=$(jq -r '.[] | select(.title == "transient-parent") | .id' <<< "$snapshot")
child_id=$(jq -r '.[] | select(.title == "transient-child") | .id' <<< "$snapshot")
if [[ -z $parent_id || -z $child_id ]]; then
  echo "could not resolve transient-suite windows: $snapshot"
  exit 1
fi
wait_for_field "$parent_id" x 100
wait_for_field "$parent_id" y 110
wait_for_field "$child_id" x 500
wait_for_field "$child_id" y 110

# The first point is only inside the parent. The second is inside both windows.
# Every assertion observes a focus transition caused by real scene hit-testing.
focus_at 200 360 "$parent_id" "could not focus the exposed parent"
wait_for_field "$child_id" focused false
focus_at 600 360 "$child_id" "focusing the floating parent buried its transient"

focus_at 200 360 "$parent_id" "could not refocus the parent before pinning"
"$UMBRIEL" msg window-toggle-pinned > /dev/null
wait_for_field "$child_id" focused false
focus_at 600 360 "$child_id" "the pinned parent covered its transient"

focus_at 200 360 "$parent_id" "could not refocus the pinned parent"
"$UMBRIEL" msg window-toggle-pinned > /dev/null
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen "$parent_id" true
wait_for_fullscreen "$child_id" false
wait_for_field "$child_id" focused false
focus_at 600 360 "$child_id" "the fullscreen parent covered its transient"

echo "transient dialogs stay above floating, pinned, and fullscreen parents"
