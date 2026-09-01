#!/usr/bin/env bash
# A modified middle-button bind pans the scrolling layout continuously and settles it through the same path as a
# three-finger gesture. In overview, an unmodified middle drag steps workspace rows while a stationary middle click
# remains the close gesture; delaying close until release lets motion disambiguate the two without swallowing ordinary
# middle clicks outside the overview.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly BTN_MIDDLE=274
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"
readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/workspace-client}"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[layout.scrolling]
direction = "horizontal"
default_width_fraction = 0.5

[keybinds]
"Mod+MouseMiddle" = "layout-scroll-drag"
EOF
"$UMBRIEL" msg config-reload > /dev/null

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

window_count() {
  "$UMBRIEL" windows --json | jq 'length'
}

wait_for_count() {
  for _ in $(seq 80); do
    [[ $(window_count) -eq $1 ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for $1 windows, have $(window_count)"
  return 1
}

wait_for_a_visible() {
  local field=$1
  local value=-1
  for _ in $(seq 20); do
    value=$("$UMBRIEL" windows --json | jq -r --arg field "$field" '.[] | select(.title == "A") | .[$field]')
    ((value >= 0)) && {
      echo "$value"
      return 0
    }
    sleep 0.1
  done
  echo "A did not become visible on the $field axis" >&2
  return 1
}

count=0
for title in A B C D; do
  foot --title="$title" sh -c 'sleep 120' > /dev/null 2>&1 &
  count=$((count + 1))
  wait_for_count "$count"
done

# New windows focus at the strip end. Return to A and wait for its arrangement so dragging left has room to pan.
"$UMBRIEL" msg column-focus-first > /dev/null
before_x=$(wait_for_a_visible x)

pointer move 900 360 mod logo press "$BTN_MIDDLE" move 850 360 move 350 360 release "$BTN_MIDDLE" mod none
after_x=$before_x
for _ in $(seq 20); do
  after_x=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "A") | .x')
  ((after_x < before_x)) && break
  sleep 0.1
done
if ((after_x >= before_x)); then
  echo "layout-scroll-drag did not pan toward strip end: A x $before_x -> $after_x"
  exit 1
fi

# Reload the same workspace vertically and verify that the drag follows the configured strip axis.
sed -i 's/^direction = "horizontal"$/direction = "vertical"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
vertical_rows=0
for _ in $(seq 20); do
  vertical_rows=$("$UMBRIEL" windows --json | jq '[.[].y] | unique | length')
  ((vertical_rows > 1)) && break
  sleep 0.1
done
if ((vertical_rows <= 1)); then
  echo "scrolling layout did not reload with a vertical strip"
  exit 1
fi
"$UMBRIEL" msg column-focus-first > /dev/null
before_y=$(wait_for_a_visible y)
pointer move 640 650 mod logo press "$BTN_MIDDLE" move 640 600 move 640 100 release "$BTN_MIDDLE" mod none
after_y=$before_y
for _ in $(seq 20); do
  after_y=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "A") | .y')
  ((after_y < before_y)) && break
  sleep 0.1
done
if ((after_y >= before_y)); then
  echo "layout-scroll-drag did not pan down the vertical strip: A y $before_y -> $after_y"
  exit 1
fi

# Restore the horizontal presentation and first column so a known card sits under the overview test point.
sed -i 's/^direction = "vertical"$/direction = "horizontal"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
horizontal_rows=0
for _ in $(seq 20); do
  horizontal_rows=$("$UMBRIEL" windows --json | jq '[.[].y] | unique | length')
  ((horizontal_rows == 1)) && break
  sleep 0.1
done
if ((horizontal_rows != 1)); then
  echo "scrolling layout did not restore its horizontal strip"
  exit 1
fi
"$UMBRIEL" msg column-focus-first > /dev/null
wait_for_a_visible x > /dev/null

# The configured modified drag pans the scrolling row under the pointer in the overview too. Overview cards are scaled,
# so the gesture maps pointer travel back through that scale before updating the workspace viewport.
pointer move 560 360
"$UMBRIEL" msg overview-open > /dev/null
overview_before_x=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "A") | .x')
pointer mod logo press "$BTN_MIDDLE" move 510 360 move 310 360 release "$BTN_MIDDLE" mod none
overview_after_x=$overview_before_x
for _ in $(seq 20); do
  overview_after_x=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "A") | .x')
  ((overview_after_x < overview_before_x)) && break
  sleep 0.1
done
if ((overview_after_x >= overview_before_x)); then
  echo "layout-scroll-drag did not pan the overview row: A x $overview_before_x -> $overview_after_x"
  exit 1
fi

# Bare horizontal middle drag pans the same row without requiring the configured modifier.
bare_before_x=$overview_after_x
pointer press "$BTN_MIDDLE" move 360 360 move 560 360 release "$BTN_MIDDLE"
bare_after_x=$bare_before_x
for _ in $(seq 20); do
  bare_after_x=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "A") | .x')
  ((bare_after_x > bare_before_x)) && break
  sleep 0.1
done
if ((bare_after_x <= bare_before_x)); then
  echo "bare middle drag did not pan the overview row: A x $bare_before_x -> $bare_after_x"
  exit 1
fi

# Bare vertical middle drag retains the overview's row navigation. The first motion crosses the drag threshold; the
# second crosses one row step. Releasing after motion must not close the card under the original press.
pointer move 560 360
pointer press "$BTN_MIDDLE" move 560 330 move 560 150 release "$BTN_MIDDLE"

for _ in $(seq 20); do
  [[ $("$WORKSPACE") == 2 ]] && break
  sleep 0.1
done
if [[ $("$WORKSPACE") != 2 ]]; then
  echo "overview middle drag did not select workspace 2"
  exit 1
fi
if [[ $(window_count) -ne 4 ]]; then
  echo "overview middle drag closed a card instead of navigating"
  exit 1
fi

# Return to the occupied row, then prove the click half of the gesture is
# deferred: the press alone leaves the card mapped and the matching release
# sends exactly one close request.
pointer press "$BTN_MIDDLE" move 560 180 move 560 360 release "$BTN_MIDDLE"
for _ in $(seq 20); do
  [[ $("$WORKSPACE") == 1 ]] && break
  sleep 0.1
done
if [[ $("$WORKSPACE") != 1 ]]; then
  echo "overview middle drag did not return to workspace 1"
  exit 1
fi
if [[ $(window_count) -ne 4 ]]; then
  echo "overview return drag closed a card instead of navigating"
  exit 1
fi
pointer move 560 360 press "$BTN_MIDDLE" pause 500 release "$BTN_MIDDLE" &
middle_click_pid=$!
sleep 0.2
if [[ $(window_count) -ne 4 ]]; then
  echo "overview middle press closed a card before release"
  exit 1
fi
wait "$middle_click_pid"
wait_for_count 3

echo "bound and bare mouse drags pan overview strips, navigate rows, and preserve release-only middle-click close"
