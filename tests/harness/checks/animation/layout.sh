#!/usr/bin/env bash
# Animated operations end with every window drawn where the layout puts it. Every animated owner is driven from one
# registry, in a fixed phase order, and an owner dropped from it stops being ticked: its windows stay drawn at the old
# geometry while `windows --json` already reports the target. So after each operation the frozen animation clock runs
# past every timeline, settle confirms nothing is still animating, and each window's drawn box must equal its layout
# box. Closing a window additionally exercises the fade-out snapshot, the one owner that registers and unregisters at
# runtime. Absolute positions are asserted only where they are determined. Mid-strip the scroll offset depends on which
# column has focus and on the neighbour peek, so those steps assert size and spacing. Once two columns exactly fill the
# viewport the offset has only one legal value, so the close step pins it.
set -euo pipefail

readonly EXPECT_W=624 # 0.5 fraction of the 1260 viewport, gap-aware
readonly EXPECT_H=700 # 720 output minus 2 * edgePad
readonly TOTAL_GAP=12 # gap 8 + 2 * border 2
readonly SHOT="$UMBRIEL_RUNTIME_DIR/layout-animation.png"

declare -A PREDICATE=(
  [layout-red]='r > 0.8 && g < 0.2 && b < 0.2'
  [layout-green]='g > 0.8 && r < 0.2 && b < 0.2'
  [layout-blue]='b > 0.8 && r < 0.2 && g < 0.2'
)
declare -A CLIENT_PID=()

printf '\n[layout.scrolling]\ndefault_extent_fraction = 0.5\n\n[colors]\nbackdrop = "#000000FF"\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
# Animation time only moves by clock-advance, and 10 s runs past every default timeline.
"$UMBRIEL" clock-freeze

spawn_client() {
  local title=$1 color=$2
  FILL_COLOR=$color RESIZE_FILL_COLOR=$color "$UMBRIEL_UNMAP_CLIENT" "$title" 400 300 \
    > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  CLIENT_PID[$title]=$!
}

geometry() { "$UMBRIEL" windows --json | jq -Sc '[.[] | {w, h, x}] | sort_by(.x)'; }

wait_for_count() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $1 ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for $1 window(s), saw $("$UMBRIEL" windows --json | jq 'length')"
  return 1
}

# Runs every started animation to its end; settle then fails at once if any animation is still running.
finish() {
  "$UMBRIEL" clock-advance 10000
  "$UMBRIEL" settle
}

# Each window's colour is drawn exactly over its layout box, clipped to the output.
assert_drawn_at_layout() {
  local label=$1 window title x y w h drawn expected
  grim "$SHOT"
  while read -r window; do
    title=$(jq -r .title <<< "$window")
    read -r x y w h < <(jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$window")
    ((x + w > 1280)) && w=$((1280 - x))
    ((x < 0)) && w=$((w + x)) && x=0
    ((w <= 0)) && continue
    drawn=$("$UMBRIEL_PIXEL_PROBE" "$SHOT" bbox "${PREDICATE[$title]}")
    expected="$x $y $w $h"
    if [[ $drawn != "$expected" ]]; then
      echo "$label: $title is drawn at $drawn, but its layout box is $expected"
      return 1
    fi
  done < <("$UMBRIEL" windows --json | jq -c '.[]')
}

# Every column is the predicted size, consecutive columns are one gap apart, and each is drawn where it is laid out.
assert_tiled_strip() {
  local label=$1 want=$2 settled
  finish
  settled=$(geometry)
  if ! jq -e --argjson n "$want" --argjson w "$EXPECT_W" --argjson h "$EXPECT_H" \
      'length == $n and all(.[]; .w == $w and .h == $h)' <<< "$settled" > /dev/null; then
    echo "$label: expected $want columns of ${EXPECT_W}x${EXPECT_H}, got $settled"
    return 1
  fi
  if ! jq -e --argjson gap "$TOTAL_GAP" --argjson w "$EXPECT_W" \
      '[range(0; length - 1) as $i | .[$i + 1].x - .[$i].x] | all(.[]; . == $w + $gap)' <<< "$settled" \
      > /dev/null; then
    echo "$label: columns are not one gap apart: $settled"
    return 1
  fi
  assert_drawn_at_layout "$label"
}

spawn_client layout-red 0xFFFF0000
wait_for_count 1
spawn_client layout-green 0xFF00FF00
wait_for_count 2
spawn_client layout-blue 0xFF0000FF
wait_for_count 3
assert_tiled_strip "initial" 3

# Moving a column animates positions; the strip must reassemble.
"$UMBRIEL" msg column-move-left > /dev/null
assert_tiled_strip "after column-move-left" 3
"$UMBRIEL" msg column-move-right > /dev/null
assert_tiled_strip "after column-move-right" 3

# A workspace switch animates the slide, in a different phase from the views.
"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" msg workspace-switch:1 > /dev/null
assert_tiled_strip "after workspace round trip" 3

# Closing a window animates its snapshot and the surviving layout from the same spatial transition.
kill -TERM "${CLIENT_PID[layout-blue]}" 2> /dev/null || true
wait_for_count 2
finish
# Two 624 columns plus one gap exactly fill the 1260 viewport.
expected_close='[{"h":700,"w":624,"x":10},{"h":700,"w":624,"x":646}]'
if [[ $(geometry) != "$expected_close" ]]; then
  echo "strip did not re-anchor after close: $(geometry)"
  exit 1
fi
assert_drawn_at_layout "after close"
if [[ $("$UMBRIEL_PIXEL_PROBE" "$SHOT" count "${PREDICATE[layout-blue]}") != 0 ]]; then
  echo "the closed window's fade-out snapshot was still drawn after its timeline"
  exit 1
fi

echo "column move, workspace round trip, and close all end with every window drawn at its layout box"
