#!/usr/bin/env bash
# Five default-width wheel steps must cross two column gaps during a tiled drag. This covers both the doubled drag speed and the refreshed target under a
# stationary pointer.
set -euo pipefail

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly DROP_X=630
readonly DROP_Y=360
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

spawn_client() {
  foot --title="drag-wheel-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout.scrolling]
default_extent_fraction = 0.5

[keybinds]
"Mod+WheelUp" = "window-focus-left"
"Mod+WheelDown" = "window-focus-right"
EOF
"$UMBRIEL" msg config-reload > /dev/null

for id in $(seq 1 6); do
  spawn_client "$id"
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $id ]] && break
    sleep 0.25
  done
done
sleep 0.5

for _ in $(seq 1 5); do
  "$UMBRIEL" msg window-focus-left > /dev/null
done
sleep 0.5

windows=$("$UMBRIEL" windows --json)
if [[ $(jq -r '.[] | select(.focused) | .title' <<< "$windows") != drag-wheel-1 ]]; then
  echo "expected the first column to be focused: $windows"
  exit 1
fi
start_x=$(jq -r '.[] | select(.focused) | (.x + .w / 2 | round)' <<< "$windows")
start_y=$(jq -r '.[] | select(.focused) | (.y + .h / 2 | round)' <<< "$windows")

commands=(mod logo move "$start_x" "$start_y" press "$BTN_LEFT" move "$DROP_X" "$DROP_Y")
for _ in $(seq 1 5); do
  commands+=(notch 1)
done
commands+=(release "$BTN_LEFT" mod none)
pointer "${commands[@]}"
sleep 0.8

windows=$("$UMBRIEL" windows --json)
source_x=$(jq -r '.[] | select(.title == "drag-wheel-1") | .x' <<< "$windows")
left_x=$(jq -r '.[] | select(.title == "drag-wheel-3") | .x' <<< "$windows")
right_x=$(jq -r '.[] | select(.title == "drag-wheel-4") | .x' <<< "$windows")
if (( source_x <= left_x || source_x >= right_x )); then
  echo "drag wheel did not cover two column gaps at doubled speed: $windows"
  exit 1
fi

echo "tiled drag wheel used doubled speed and refreshed the target"
