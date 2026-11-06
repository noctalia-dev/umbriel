#!/usr/bin/env bash
# A scrolling workspace holding one window places it where `layout.scrolling.underfull_strip` says, and a reload moves
# the strip without touching the window. The three placements are start, center and end, and an end-aligned strip keeps
# its trailing edge pinned while the window grows.
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

# Polls until the single window matches the jq predicate given in $1, then echoes its geometry. $2 is the failure story.
await_geometry() {
  local predicate=$1 message=$2 windows=
  for _ in $(seq 60); do
    windows=$("$UMBRIEL" windows --json)
    if jq -e "$predicate" <<< "$windows" > /dev/null; then
      echo "$windows"
      return 0
    fi
    sleep 0.1
  done
  echo "$message: $windows"
  return 1
}

# Output is 1280x720. With the shipped defaults (gap 8, border 2) edgePad = 10 and viewport = 1260, so a 0.5 fraction
# column is round(0.5 * (1260 + 12)) - 12 = 624 and the slack is 636.
readonly EXPECT_W=624
readonly STRIP_LEFT=$(( 10 + 0 ))
readonly STRIP_CENTER=$(( 10 + (1260 - EXPECT_W) / 2 ))
readonly STRIP_RIGHT=$(( 10 + 1260 - EXPECT_W ))
# Trailing edge of the viewport inside the output.
readonly VIEWPORT_RIGHT=$(( 1280 - 10 ))

printf '\n[layout.scrolling]\ndefault_extent_fraction = 0.5\nunderfull_strip = "start"\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

spawn_client lone
wait_for_windows 1

geometry=$(await_geometry ".[0].x == $STRIP_LEFT and .[0].w == $EXPECT_W" \
  "a start-aligned single window did not rest at the strip start")
echo "start: $(jq -c '.[0] | {x, w}' <<< "$geometry")"

sed -i 's/underfull_strip = "start"/underfull_strip = "center"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
geometry=$(await_geometry ".[0].x == $STRIP_CENTER and .[0].w == $EXPECT_W" \
  "reloading underfull_strip = \"center\" did not center the single window")
echo "center: $(jq -c '.[0] | {x, w}' <<< "$geometry")"

sed -i 's/underfull_strip = "center"/underfull_strip = "end"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
geometry=$(await_geometry ".[0].x == $STRIP_RIGHT and .[0].w == $EXPECT_W" \
  "reloading underfull_strip = \"end\" did not push the single window to the far edge")
echo "end: $(jq -c '.[0] | {x, w}' <<< "$geometry")"
if [[ $(jq -r '.[0].x + .[0].w' <<< "$geometry") -ne $VIEWPORT_RIGHT ]]; then
  echo "an end-aligned window is not flush with the viewport's trailing edge: $geometry"
  exit 1
fi

# Growing the window keeps the strip's trailing edge pinned, so the leading edge moves instead.
"$UMBRIEL" msg window-modify-primary-extent 0.1 > /dev/null
geometry=$(await_geometry ".[0].w > $EXPECT_W" "window-modify-primary-extent did not grow the window")
echo "end after growth: $(jq -c '.[0] | {x, w}' <<< "$geometry")"
if [[ $(jq -r '.[0].x + .[0].w' <<< "$geometry") -ne $VIEWPORT_RIGHT ]]; then
  echo "an end-aligned strip lost its trailing edge while the window grew: $geometry"
  exit 1
fi
if [[ $(jq -r '.[0].x' <<< "$geometry") -ge $STRIP_RIGHT ]]; then
  echo "the window grew toward the anchored edge instead of away from it: $geometry"
  exit 1
fi

# Shrinking back restores the resting placement.
"$UMBRIEL" msg window-modify-primary-extent -0.1 > /dev/null
geometry=$(await_geometry ".[0].w == $EXPECT_W and .[0].x == $STRIP_RIGHT" \
  "the end-aligned window did not settle back onto its resting position")
echo "end restored: $(jq -c '.[0] | {x, w}' <<< "$geometry")"

# A centered rest carries a scroll offset derived from the alignment, so reloading the alignment must re-judge it:
# with the fix the strip returns to its new resting placement instead of keeping the old offset.
"$UMBRIEL" msg column-center > /dev/null
geometry=$(await_geometry ".[0].x == $STRIP_CENTER" "column-center did not center the single window")
echo "centered rest: $(jq -c '.[0] | {x, w}' <<< "$geometry")"
sed -i 's/underfull_strip = "end"/underfull_strip = "start"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
geometry=$(await_geometry ".[0].x == $STRIP_LEFT" \
  "reloading away from a centered rest left the strip on the old alignment's offset")
echo "after alignment reload: $(jq -c '.[0] | {x, w}' <<< "$geometry")"

echo "single-window strip rests at start, center and end; end stays anchored while the window resizes"
