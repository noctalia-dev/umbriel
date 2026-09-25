#!/usr/bin/env bash
# A center-region Mod+Right click proposes no resize edge. It must not begin an empty resize grab or clear maximize state.
set -euo pipefail

readonly BTN_RIGHT=273 # evdev BTN_RIGHT
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

spawn_client() {
  foot sh -c 'sleep 120' > /dev/null 2>&1 &
}

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

wait_for_window() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for one window"
  return 1
}

wait_for_floating() {
  local want=$1
  for _ in $(seq 40); do
    [[ $("$UMBRIEL" windows --json | jq -r '.[0].floating') == "$want" ]] && return 0
    sleep 0.1
  done
  echo "timed out waiting for floating=$want: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_maximized_size() {
  for _ in $(seq 40); do
    local windows width height
    windows=$("$UMBRIEL" windows --json)
    width=$(jq -r '.[0].w' <<< "$windows")
    height=$(jq -r '.[0].h' <<< "$windows")
    (( width >= 1200 && height >= 700 )) && return 0
    sleep 0.1
  done
  echo "timed out waiting for maximized geometry: $("$UMBRIEL" windows --json)"
  return 1
}

center_resize_click() {
  local windows x y
  windows=$("$UMBRIEL" windows --json)
  x=$(jq -r '.[0].x + (.[0].w / 2 | floor)' <<< "$windows")
  y=$(jq -r '.[0].y + (.[0].h / 2 | floor)' <<< "$windows")
  pointer move "$x" "$y" mod logo click "$BTN_RIGHT" mod none
  "$UMBRIEL" settle
}

check_maximized_size_unchanged() {
  local kind=$1 before after before_size after_size
  before=$("$UMBRIEL" windows --json)
  before_size=$(jq -r '.[0] | "\(.w)x\(.h)"' <<< "$before")
  center_resize_click
  after=$("$UMBRIEL" windows --json)
  after_size=$(jq -r '.[0] | "\(.w)x\(.h)"' <<< "$after")
  if [[ $after_size != "$before_size" ]]; then
    echo "$kind center resize click changed maximized geometry: $before_size to $after_size"
    return 1
  fi
}

spawn_client
wait_for_window

"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
wait_for_maximized_size
failed=0
check_maximized_size_unchanged tiled || failed=1

"$UMBRIEL" msg window-toggle-floating > /dev/null
wait_for_floating true
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
wait_for_maximized_size
check_maximized_size_unchanged floating || failed=1

if (( failed != 0 )); then
  exit 1
fi

echo "center resize clicks preserve tiled and floating maximize state"
