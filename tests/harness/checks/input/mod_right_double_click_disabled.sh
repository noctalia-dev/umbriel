#!/usr/bin/env bash
# mod_right_double_click defaults to "none": two quick Mod+right-clicks are two
# resize attempts, never a maximize toggle. The gesture is opt-in so the
# button's default behavior is unchanged.
set -euo pipefail

readonly BTN_RIGHT=273 # evdev BTN_RIGHT
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

if [[ ! -x $POINTER ]]; then
  echo "input helpers are not available"
  exit 1
fi

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

window_box() {
  "$UMBRIEL" windows --json | jq -r '.[0] | "\(.x) \(.y) \(.w) \(.h)"'
}

foot sh -c 'sleep 120' > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.25
done
"$UMBRIEL" settle

# A pair at the edge-free centre and at an edge zone: both stay clicks.
failed=0
for pos in "0.5 0.5" "0.25 0.5"; do
  read -r fx fy <<< "$pos"
  windows=$("$UMBRIEL" windows --json)
  x=$(jq -r --argjson f "$fx" '.[0].x + (.[0].w * $f | floor)' <<< "$windows")
  y=$(jq -r --argjson f "$fy" '.[0].y + (.[0].h * $f | floor)' <<< "$windows")
  pointer move "$x" "$y" mod logo click "$BTN_RIGHT" click "$BTN_RIGHT" mod none
  "$UMBRIEL" settle
  if [[ $("$UMBRIEL" windows --json | jq -r '.[0].maximized') != false ]]; then
    echo "Mod+double-right-click at ($fx,$fy) toggled maximize with the default config"
    failed=1
  fi
done

# The button still does its main job: a drag past the slop resizes with its
# press-time edge. This also proves the pair clicks landed on the window.
windows=$("$UMBRIEL" windows --json)
x=$(jq -r '.[0] | "\(.x + (.w * 0.2 | floor))"' <<< "$windows")
y=$(jq -r '.[0] | "\(.y + (.h / 2 | floor))"' <<< "$windows")
before=$(window_box)
pointer mod logo move "$x" "$y" press "$BTN_RIGHT" move $((x + 60)) "$y" release "$BTN_RIGHT" mod none
"$UMBRIEL" settle
if [[ $(window_box) == "$before" ]]; then
  echo "Mod+right-drag did not resize with mod_right_double_click = none"
  failed=1
fi

if (( failed != 0 )); then
  exit 1
fi

echo "mod_right_double_click = none keeps Mod+right-click a pure resize drag"
