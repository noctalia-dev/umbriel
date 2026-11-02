#!/usr/bin/env bash
# Floating geometry must reach subscribers during a held move or resize,
# before releasing the grab can trigger unrelated focus or layout updates.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly TITLE=ipc-floating-geometry
readonly EVENTS="$UMBRIEL_RUNTIME_DIR/floating-geometry-events.jsonl"

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[[window_rule]]
match.title = "^ipc-floating-geometry$"
default_floating = true
default_floating_size_px = { width = 480, height = 300 }
default_position = { x = 173, y = 109, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL_FRACTIONAL_CLIENT" "$TITLE" 480 300 > "$UMBRIEL_RUNTIME_DIR/floating-geometry-client.log" 2>&1 &
for _ in $(seq 100); do
  window=$("$UMBRIEL" windows --json | jq -c --arg title "$TITLE" '.[] | select(.title == $title)')
  [[ -n $window ]] && break
  sleep 0.05
done
window_id=$(jq -r '.id' <<< "$window")
"$UMBRIEL" msg window-focus "$window_id" > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" subscribe windows > "$EVENTS" &

await_box() {
  local label=$1 x=$2 y=$3 width=$4 height=$5
  for _ in $(seq 100); do
    if jq -se --arg id "$window_id" --argjson x "$x" --argjson y "$y" \
        --argjson width "$width" --argjson height "$height" '
      [.[] | select(.event == "windows")] | last | .data
      | any(.[]; .id == $id and .floating and .active
          and .x == $x and .y == $y and .w == $width and .h == $height)
    ' "$EVENTS" > /dev/null 2>&1; then
      return 0
    fi
    sleep 0.05
  done
  echo "$label: subscriber missed $width x $height at $x,$y"
  echo "latest event: $(tail -n 1 "$EVENTS")"
  echo "current listing: $("$UMBRIEL" windows --json)"
  exit 1
}

await_box initial 173 109 480 300

pointer_hold 1280 720 move 413 259 mod logo press 272 move 573 339 \
  -- move 653 379 mark moved hold release 272 mod none
await_box held-move 333 189 480 300
pointer_step moved
await_box continued-move 413 229 480 300
pointer_release
"$UMBRIEL" settle > /dev/null
await_box released-move 413 229 480 300

pointer_hold 1280 720 move 873 509 mod logo press 273 move 953 569 \
  -- move 993 589 mark resized hold release 273 mod none
await_box held-resize 413 229 560 360
pointer_step resized
await_box continued-resize 413 229 600 380
pointer_release
"$UMBRIEL" settle > /dev/null

pointer_hold 1280 720 move 433 249 mod logo press 273 move 373 209 \
  -- move 333 189 mark anchored hold release 273 mod none
await_box anchored-resize 353 189 660 420
pointer_step anchored
await_box continued-anchored-resize 313 169 700 440
pointer_release
"$UMBRIEL" settle > /dev/null

"$UMBRIEL" msg window-set-primary-extent 0.5 > /dev/null
"$UMBRIEL" settle > /dev/null
await_box keyboard-resize 313 169 640 440

echo 'floating moves, anchored resizes, and keyboard resizes reached windows subscribers'
