#!/usr/bin/env bash
# Animated operations reach a steady state with the geometry the layout predicts. Every animated owner is driven from one registry, in a fixed phase order. Nothing here can watch an animation mid-flight, but it can see the consequence of one that never finishes: an owner dropped from the registry stops being ticked, so its windows never stop moving or never reach their target. Closing a window additionally exercises the fade-out snapshot, the one owner that registers and unregisters at runtime. Absolute positions are asserted only where they are determined. Mid-strip the scroll offset depends on which column has focus and on the neighbour peek, so those steps assert size and spacing. Once two columns exactly fill the viewport the offset has only one legal value, so the close step pins it.
set -euo pipefail

readonly EXPECT_W=624 # 0.5 fraction of the 1260 viewport, gap-aware
readonly EXPECT_H=700 # 720 output minus 2 * edgePad
readonly TOTAL_GAP=12 # gap 8 + 2 * border 2

CLIENT_PIDS=()

printf '\n[layout.scrolling]\ndefault_extent_fraction = 0.5\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

spawn_client() {
  foot sh -c 'sleep 120' > /dev/null 2>&1 &
  CLIENT_PIDS+=($!)
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

# Two identical consecutive samples mean every animation has finished.
settled=""
wait_until_stable() {
  local previous=""
  local current=""
  settled=""
  for _ in $(seq 80); do
    current=$(geometry)
    if [[ -n $previous && $current == "$previous" ]]; then
      settled=$current
      return 0
    fi
    previous=$current
    sleep 0.25
  done
  echo "geometry never stopped changing (last: $previous)"
  return 1
}

# Every column is the predicted size, and consecutive columns are one gap apart.
assert_tiled_strip() {
  local label=$1 want=$2
  wait_until_stable || return 1
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
}

spawn_client
wait_for_count 1
spawn_client
wait_for_count 2
spawn_client
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

# Closing a window runs a fade-out snapshot alongside the survivors' re-tile.
kill -TERM "${CLIENT_PIDS[-1]}" 2>/dev/null || true
wait_for_count 2
assert_tiled_strip "after close" 2

# Two 624 columns plus one gap are exactly the 1260 viewport, so maxScroll is 0 and the strip must sit flush at the left edge pad. Losing the re-anchor on
# removal leaves it scrolled, with a survivor cut off and empty space right.
if [[ $settled != '[{"h":700,"w":624,"x":10},{"h":700,"w":624,"x":646}]' ]]; then
  echo "strip not re-anchored after close: $settled"
  exit 1
fi

echo "column move, workspace round trip, and close all settle"
