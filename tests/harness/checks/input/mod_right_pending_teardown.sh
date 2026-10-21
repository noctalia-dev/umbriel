#!/usr/bin/env bash
# A deferred Mod+right press must not outlive the view it references: the
# window can unmap while the button is still held, and the next motion would
# otherwise promote the deferred grab against freed storage.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BTN_RIGHT=273 # evdev BTN_RIGHT
readonly OUTPUT_W=1280
readonly OUTPUT_H=720

await_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for $want window(s): $("$UMBRIEL" windows --json | jq -c 'map(.title)')"
  return 1
}

boxes() {
  "$UMBRIEL" windows --json | jq -r '.[] | [.title, .x, .y, .w, .h] | @tsv' | sort
}

await_stable_boxes() {
  local previous= current
  current=$(boxes)
  for _ in $(seq 40); do
    previous=$current
    sleep 0.1
    current=$(boxes)
    [[ $current == "$previous" ]] && return 0
  done
  echo "window boxes never stabilised: $current"
  return 1
}

# Geometry must be readable without settle: it waits for a drawn frame and
# wedges while a button is held, which is exactly the state under test.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[input]
mod_right_double_click = "maximize"
EOF
"$UMBRIEL" msg config-reload > /dev/null

# A window we close through IPC while the press is held. The press lands in the
# left third so it proposes a resize edge and the deferred grab exists.
foot sh -c 'sleep 120' > /dev/null 2>&1 &
await_count 1

read -r x y < <(jq -r '.[0] | "\(.x + (.w * 0.2 | floor)) \(.y + (.h / 2 | floor))"' <<< "$("$UMBRIEL" windows --json)")
window_id=$(jq -r '.[0].id' <<< "$("$UMBRIEL" windows --json)")
pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  move "$x" "$y" mod logo press "$BTN_RIGHT" mod none \
  -- move $((x + 40)) $((y + 40)) release "$BTN_RIGHT" mod none

# Kill the referenced window through IPC with the button still down.
"$UMBRIEL" msg "window-close:$window_id" > /dev/null
await_count 0

# Fresh views are the most likely to reuse the freed storage the pending
# record still points at; their geometry must survive the promotion either
# way, and the compositor must survive the motion.
for _ in $(seq 6); do
  foot sh -c 'sleep 120' > /dev/null 2>&1 &
done
await_count 6
await_stable_boxes
before=$(boxes)

# Motion past the click slop promotes the deferred grab.
pointer_release

after=$(boxes)
if [[ $after != "$before" ]]; then
  echo "the deferred press reached freed storage and moved a live window:"
  echo "before: $before"
  echo "after:  $after"
  exit 1
fi

echo "a deferred Mod+right press is dropped when its view goes away"
