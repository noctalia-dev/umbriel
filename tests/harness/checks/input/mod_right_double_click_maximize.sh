#!/usr/bin/env bash
# A second Mod+right-click on the same window within the double-click window
# applies the opposite maximize state and restores the window's geometry, for
# tiled and floating windows alike and wherever the pair lands in the window:
# the first press defers its resize grab (the thirds edge zones resolve an edge
# for most points), so no grab may perturb the state the toggle reads at the
# second press. Single clicks stay resize attempts and never toggle the state
# on their own.
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

# The gesture is opt-in; the check exercises its "maximize" vocabulary.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[input]
mod_right_double_click = "maximize"
EOF
"$UMBRIEL" msg config-reload > /dev/null

wait_for_box() {
  local want=$1
  for _ in $(seq 40); do
    [[ $(window_box) == "$want" ]] && return 0
    sleep 0.1
  done
  echo "timed out waiting for box $want, got $(window_box)"
  return 1
}

wait_for_maximized() {
  local want=$1
  for _ in $(seq 40); do
    [[ $("$UMBRIEL" windows --json | jq -r '.[0].maximized') == "$want" ]] && return 0
    sleep 0.1
  done
  echo "timed out waiting for maximized=$want: $("$UMBRIEL" windows --json)"
  return 1
}

# A double-click pair at a fractional position of the window box: 0.5 is the
# edge-free center third, 0.25/0.85 land in the resize edge zones.
double_click_at() {
  local fx=$1 fy=$2 windows x y
  windows=$("$UMBRIEL" windows --json)
  x=$(jq -r --argjson f "$fx" '.[0].x + (.[0].w * $f | floor)' <<< "$windows")
  y=$(jq -r --argjson f "$fy" '.[0].y + (.[0].h * $f | floor)' <<< "$windows")
  pointer move "$x" "$y" mod logo click "$BTN_RIGHT" click "$BTN_RIGHT" mod none
  "$UMBRIEL" settle
}

check_round_trip() {
  local kind=$1 fx=$2 fy=$3 before after
  before=$(window_box)
  double_click_at "$fx" "$fy"
  wait_for_maximized true || return 1
  if [[ $(window_box) == "$before" ]]; then
    echo "$kind pair at ($fx,$fy) did not change geometry: $(window_box)"
    return 1
  fi
  double_click_at "$fx" "$fy"
  wait_for_maximized false || return 1
  after=$(window_box)
  if [[ $after != "$before" ]]; then
    echo "$kind pair at ($fx,$fy) did not restore $before, got $after"
    return 1
  fi
}

foot sh -c 'sleep 120' > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.25
done
"$UMBRIEL" settle

failed=0
check_round_trip tiled 0.5 0.5 || failed=1
check_round_trip tiled 0.25 0.25 || failed=1
check_round_trip tiled 0.85 0.25 || failed=1

"$UMBRIEL" msg window-toggle-floating > /dev/null
"$UMBRIEL" settle
check_round_trip floating 0.25 0.5 || failed=1
check_round_trip floating 0.5 0.5 || failed=1

# Two clicks past the double-click window are two single clicks: resize
# attempts that must not toggle maximize.
read -r x y < <(jq -r '.[0] | "\(.x + (.w / 2 | floor)) \(.y + (.h / 2 | floor))"' <<< "$("$UMBRIEL" windows --json)")
pointer move "$x" "$y" mod logo click "$BTN_RIGHT" mod none
sleep 0.5 # real time: must exceed the double-click window so the next click pairs with nothing
pointer mod logo click "$BTN_RIGHT" mod none
"$UMBRIEL" settle
if [[ $("$UMBRIEL" windows --json | jq -r '.[0].maximized') != false ]]; then
  echo "slow Mod+right clicks toggled maximize: $("$UMBRIEL" windows --json)"
  failed=1
fi

if (( failed != 0 )); then
  exit 1
fi

echo "Mod+double-right-click toggles maximize and restores geometry on tiled and floating windows"
