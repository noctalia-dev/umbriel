#!/usr/bin/env bash
# A compositor-owned move belongs to its initiating button. Pressing and
# releasing another button must not replace or finish the move before the
# initiating release drops the detached tile back into the layout.
set -euo pipefail

readonly BTN_LEFT=272
readonly BTN_RIGHT=273
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

spawn_client() {
  foot --title="secondary-button-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_count() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $1 ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for $1 window(s)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout.scrolling]
default_width_fraction = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

for id in $(seq 1 4); do
  spawn_client "$id"
  wait_for_count "$id"
done

for _ in $(seq 1 3); do
  "$UMBRIEL" msg window-focus-left > /dev/null
done
sleep 0.5

windows=$("$UMBRIEL" windows --json)
read -r source_x source_y source_w source_h < <(
  jq -r '.[] | select(.title == "secondary-button-1") | "\(.x) \(.y) \(.w) \(.h)"' <<< "$windows"
)
leftmost_other=$(jq -r '[.[] | select(.title != "secondary-button-1") | .x] | min' <<< "$windows")
if (( source_x >= leftmost_other )); then
  echo "test setup did not place the drag source first: $windows"
  exit 1
fi
start_x=$((source_x + source_w / 2))
start_y=$((source_y + source_h / 2))

# Keep Mod and LMB held while RMB is clicked. Motion afterward proves the same
# move remains live, and only the final LMB release may perform the drop.
pointer move "$start_x" "$start_y" mod logo press "$BTN_LEFT" \
  move 1000 "$start_y" press "$BTN_RIGHT" release "$BTN_RIGHT" \
  move 1270 "$start_y" release "$BTN_LEFT" mod none
sleep 0.8

windows=$("$UMBRIEL" windows --json)
source_x=$(jq -r '.[] | select(.title == "secondary-button-1") | .x' <<< "$windows")
rightmost_other=$(jq -r '[.[] | select(.title != "secondary-button-1") | .x] | max' <<< "$windows")
if (( source_x <= rightmost_other )); then
  echo "secondary button interrupted the tiled move instead of leaving it for the initiating release: $windows"
  exit 1
fi

echo "secondary button left the tiled move owned by its initiating button"
