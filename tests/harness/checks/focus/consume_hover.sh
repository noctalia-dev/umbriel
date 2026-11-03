#!/usr/bin/env bash
# Consume and expel can replace the window beneath a stationary pointer. The first motion inside the new window
# must refresh focus, while rearranging the columns alone must preserve the action's focused window.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

cat >> "$UMBRIEL_CONFIG" <<'EOF'
[layout]
mode = "scrolling"
[layout.scrolling]
default_extent_fraction = 0.5
center_focused = "never"
[animation]
enabled = false
[input.focus]
follows_mouse = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

window() {
  "$UMBRIEL" windows --json | jq -c --arg title "consume-$1" '.[] | select(.title == $title)'
}

assert_active() {
  local expected=$1 reason=$2 actual
  actual=$("$UMBRIEL" windows --json | jq -r '.[] | select(.active) | .title')
  if [[ $actual != "consume-$expected" ]]; then
    echo "$reason: expected consume-$expected, got $actual"
    "$UMBRIEL" windows --json
    return 1
  fi
}

wait_for_count() {
  local expected=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq length) == "$expected" ]] && return 0
    sleep 0.05
  done
  echo "expected $expected windows: $("$UMBRIEL" windows --json)"
  return 1
}

for scenario in consume consume-from fallback expel; do
  clients=()
  for name in left right; do
    log="$UMBRIEL_RUNTIME_DIR/$scenario-$name.log"
    "$UMBRIEL_UNMAP_CLIENT" "consume-$name" 1200 700 > "$log" 2>&1 &
    clients+=("$!")
    await_lines "$log" mapped 1
    wait_for_count "${#clients[@]}"
    "$UMBRIEL" settle > /dev/null
  done

  id=$(window left | jq -r .id)
  "$UMBRIEL" msg window-focus "$id" > /dev/null
  "$UMBRIEL" settle > /dev/null
  px=400
  py=180
  case $scenario in
    consume) action=window-consume-right ;;
    consume-from)
      action=window-consume-from-right
      py=540
      ;;
    fallback) action=window-consume-or-expel-right ;;
    expel)
      # Start with the focused window below its neighbor. Expelling it to the right leaves that neighbor beneath
      # the pointer in the lower half of the original column.
      "$UMBRIEL" msg window-consume-right > /dev/null
      "$UMBRIEL" settle > /dev/null
      action=window-consume-or-expel-right
      py=540
      ;;
  esac

  # One persistent pointer removes device creation from the transition under test. The initial motion consumes any
  # pending map-time invalidation. These points lie well inside the presented half-width columns and half-height rows.
  pointer_hold 1280 720 move "$px" "$py" -- move "$((px + 1))" "$py" mark moved hold
  assert_active left "$scenario initial hover"
  "$UMBRIEL" msg "$action" > /dev/null
  "$UMBRIEL" settle > /dev/null
  assert_active left "$scenario stationary pointer changed focus"

  right=$(window right)
  jq -e --argjson x "$px" --argjson y "$py" \
    '.x < $x and $x + 1 < .x + .w and .y < $y and $y < .y + .h' <<< "$right" > /dev/null
  if [[ $scenario == expel ]]; then
    [[ $(window left | jq -r .x) -gt $(jq -r .x <<< "$right") ]]
  else
    [[ $(window left | jq -r .x) == "$(jq -r .x <<< "$right")" ]]
  fi

  pointer_step moved
  assert_active right "$scenario first one-pixel motion failed to refresh hover focus"
  pointer_release
  kill "${clients[@]}"
  for pid in "${clients[@]}"; do
    wait "$pid" || true
  done
  wait_for_count 0
  "$UMBRIEL" settle > /dev/null
done

echo "consume, consume-from, consume-or-expel fallback, and expel refreshed hover on the first pointer motion"
