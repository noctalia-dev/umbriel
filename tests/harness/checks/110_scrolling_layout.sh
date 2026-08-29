#!/usr/bin/env bash
# Real clients map into the scrolling layout and land on the geometry the layout math predicts. This is the regression net for the layout sizing path. The size a
# view is first configured with and the size the layout arranges it to must agree, or windows visibly resize on first paint.
set -euo pipefail

spawn_client() {
  foot --title="harness-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_windows() {
  local want=$1
  for _ in $(seq 40); do
    if [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]]; then
      return 0
    fi
    sleep 0.25
  done
  echo "timed out waiting for $want window(s), saw: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_x() {
  local title=$1
  local want=$2
  local message=$3
  local current=0
  for _ in $(seq 40); do
    current=$("$UMBRIEL" windows --json | jq -r --arg title "$title" '.[] | select(.title == $title) | .x')
    [[ $current -eq $want ]] && return 0
    sleep 0.1
  done
  echo "$message, got x=$current"
  return 1
}

# Output is 1280x720 (WLR_HEADLESS_OUTPUTS default mode). With the shipped defaults (gap 8, border 2, no outer border) the derived layout metrics are: totalBorderWidth = 2, edgePad = gap + border = 10, totalGap = gap + 2*border = 12 viewport = 1280 - 2*edgePad = 1260, height = 720 - 2*edgePad = 700 A 0.5 fraction column is then: round(0.5 * (viewport + totalGap)) - totalGap = round(636) - 12 = 624
readonly EXPECT_W=624
readonly EXPECT_H=700
readonly EXPECT_CENTER_X=$(( (1280 - EXPECT_W) / 2 ))

printf '\n[layout.scrolling]\ndefault_width_fraction = 0.5\ncenter_focused = false\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

spawn_client a
wait_for_windows 1

spawn_client b
wait_for_windows 2

windows=$("$UMBRIEL" windows --json)

if ! jq -e 'all(.[]; .floating == false)' <<< "$windows" > /dev/null; then
  echo "expected both windows tiled: $windows"
  exit 1
fi
if ! jq -e 'all(.[]; has("xwayland") and (.xwayland == false))' <<< "$windows" > /dev/null; then
  echo "expected native Wayland windows to report xwayland=false: $windows"
  exit 1
fi


if ! jq -e --argjson w "$EXPECT_W" 'all(.[]; .w == $w)' <<< "$windows" > /dev/null; then
  echo "expected both widths == $EXPECT_W (scrolling 0.5 fraction), got: $(jq -c '[.[].w]' <<< "$windows")"
  exit 1
fi

if ! jq -e --argjson h "$EXPECT_H" 'all(.[]; .h == $h)' <<< "$windows" > /dev/null; then
  echo "expected both heights == $EXPECT_H, got: $(jq -c '[.[].h]' <<< "$windows")"
  exit 1
fi

if ! jq -e '[.[].y] | unique | length == 1' <<< "$windows" > /dev/null; then
  echo "tiled columns should share a top edge, got: $(jq -c '[.[].y]' <<< "$windows")"
  exit 1
fi

if ! jq -e '[.[].x] | unique | length == 2' <<< "$windows" > /dev/null; then
  echo "two columns should occupy distinct x, got: $(jq -c '[.[].x]' <<< "$windows")"
  exit 1
fi

# The focused last column can rest beyond max scroll so its content is centered.
"$UMBRIEL" msg column-center > /dev/null
center_x=0
for _ in $(seq 40); do
  center_x=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "harness-b") | .x')
  [[ $center_x -eq $EXPECT_CENTER_X ]] && break
  sleep 0.1
done
if [[ $center_x -ne $EXPECT_CENTER_X ]]; then
  echo "expected last column centered at x=$EXPECT_CENTER_X, got x=$center_x"
  exit 1
fi

# The first column uses the corresponding negative resting offset.
"$UMBRIEL" msg window-focus-left > /dev/null
"$UMBRIEL" msg column-center > /dev/null
center_x=0
for _ in $(seq 40); do
  center_x=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "harness-a") | .x')
  [[ $center_x -eq $EXPECT_CENTER_X ]] && break
  sleep 0.1
done
if [[ $center_x -ne $EXPECT_CENTER_X ]]; then
  echo "expected first column centered at x=$EXPECT_CENTER_X, got x=$center_x"
  exit 1
fi

# Reloading the focus policy applies it to the currently focused column.
"$UMBRIEL" msg window-focus-right > /dev/null
wait_for_x harness-b 646 "expected disabled center_focused to leave the last column at its bounded position"
sed -i 's/center_focused = false/center_focused = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
wait_for_x harness-b "$EXPECT_CENTER_X" "enabling center_focused did not center the focused column on reload"
sed -i 's/center_focused = true/center_focused = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
wait_for_x harness-b 646 "disabling center_focused did not restore the bounded focused-column position"

# Floating toggle round trip: the focused window flips and comes back.
"$UMBRIEL" msg window-toggle-floating > /dev/null
for _ in $(seq 20); do
  [[ $("$UMBRIEL" windows --json | jq '[.[] | select(.floating)] | length') -eq 1 ]] && break
  sleep 0.2
done
if [[ $("$UMBRIEL" windows --json | jq '[.[] | select(.floating)] | length') -ne 1 ]]; then
  echo "window-toggle-floating did not float the focused window: $("$UMBRIEL" windows --json)"
  exit 1
fi

"$UMBRIEL" msg window-toggle-floating > /dev/null
for _ in $(seq 20); do
  [[ $("$UMBRIEL" windows --json | jq '[.[] | select(.floating)] | length') -eq 0 ]] && break
  sleep 0.2
done
if [[ $("$UMBRIEL" windows --json | jq '[.[] | select(.floating)] | length') -ne 0 ]]; then
  echo "window-toggle-floating did not re-tile the focused window: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "2 clients tiled at ${EXPECT_W}x${EXPECT_H}, edge columns center, float round trip ok"
