#!/usr/bin/env bash
# Explicit scrolling defers hover refresh until its final retargeted animation settles. The first actual pointer
# motion then focuses the exposed column without a border crossing; a hover reveal itself must not arm another refresh.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

wait_count() {
  for _ in $(seq 100); do
    [[ $("$UMBRIEL" windows --json | jq length) == "$1" ]] && return 0
    sleep 0.025
  done
  echo "expected $1 windows: $("$UMBRIEL" windows --json)"
  return 1
}

assert_active() {
  local actual
  actual=$("$UMBRIEL" windows --json | jq -r '.[] | select(.active) | .title')
  [[ $actual == "$1" ]] || { echo "$2: expected $1 active, got $actual"; return 1; }
}

assert_color() {
  local image=$UMBRIEL_RUNTIME_DIR/scroll-hover-$1.png actual
  grim "$image"
  actual=$("$UMBRIEL_PIXEL_PROBE" "$image" pixel "$2" "$3")
  [[ $actual == "$4" ]] || { echo "$1: expected RGB $4 at $2,$3, got $actual"; return 1; }
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'
[layout]
mode = "scrolling"
[layout.scrolling]
default_extent_fraction = 0.6
center_focused = "never"
[animation.windows_in]
enabled = false
[animation.windows_move]
duration_ms = 1000
curve = "linear"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[input.focus]
follows_mouse = "sloppy"
EOF
"$UMBRIEL" msg config-reload > /dev/null

index=0
for color in 0xFFFF0000 0xFF00FF00 0xFF0000FF; do
  index=$((index + 1))
  # Follow configured tile sizes so painted surfaces and hover geometry agree throughout the scroll.
  FILL_COLOR=$color RESIZE_FILL_COLOR=$color "$UMBRIEL_UNMAP_CLIENT" "scene-scroll-$index" 400 300 \
    > "$UMBRIEL_RUNTIME_DIR/scroll-client-$index.log" 2>&1 &
  wait_count "$index"
done
"$UMBRIEL" settle > /dev/null

# Revealing B from C puts B against the left edge. The right side of B will be covered by C halfway through the
# first three scroll steps. Pixel assertions establish the actual presentation instead of trusting target IPC geometry.
"$UMBRIEL" msg window-focus-left > /dev/null
"$UMBRIEL" settle > /dev/null
pointer_hold 1280 720 move 740 360 -- \
  move 741 360 mark during hold \
  move 741 360 mark stationary hold \
  move 742 360 mark refreshed hold
assert_active scene-scroll-2 "initial pointer focus"
assert_color before 740 360 "0 255 0"
"$UMBRIEL" clock-freeze > /dev/null

for _ in $(seq 3); do "$UMBRIEL" msg layout-scroll-right > /dev/null; done
assert_active scene-scroll-2 "pending explicit scroll"
"$UMBRIEL" clock-advance 500 > /dev/null
assert_color during 740 360 "0 0 255"
assert_active scene-scroll-2 "stationary pointer during scrolling"
pointer_step during
assert_active scene-scroll-2 "one-pixel motion during scrolling"

# Retarget the running animation all the way to the strip's end. Extra commands are no-ops at that end and must not
# discard the refresh armed by the successful steps. A halfway frame still suppresses focus after the retarget.
for _ in $(seq 20); do "$UMBRIEL" msg layout-scroll-right > /dev/null; done
"$UMBRIEL" clock-advance 500 > /dev/null
assert_active scene-scroll-2 "retargeted scrolling"
"$UMBRIEL" clock-advance 1000 > /dev/null
"$UMBRIEL" settle > /dev/null
assert_color after 741 360 "0 0 255"
assert_active scene-scroll-2 "stationary pointer after scrolling"
pointer_step stationary
assert_active scene-scroll-2 "zero-distance motion after scrolling"
pointer_step refreshed
assert_active scene-scroll-3 "first real motion after scrolling"
"$UMBRIEL" clock-advance 2000 > /dev/null
"$UMBRIEL" settle > /dev/null
pointer_release
"$UMBRIEL" clock-resume > /dev/null

# Centered hover reveals move an additional column under a nearly stationary pointer. That movement must not arm
# the explicit-scroll refresh, either during its animation or after it has settled.
sed -i 's/center_focused = "never"/center_focused = "always"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg column-focus-first > /dev/null
"$UMBRIEL" msg column-focus-last > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" clock-freeze > /dev/null
assert_color cascade-before 20 360 "0 255 0"
pointer_hold 1280 720 move 640 360 -- \
  move 20 360 mark crossed hold \
  move 20 361 mark reveal-motion hold \
  move 20 362 mark after-reveal hold
assert_active scene-scroll-3 "centered initial focus"
pointer_step crossed
assert_active scene-scroll-2 "hover crossing into B"
"$UMBRIEL" clock-advance 750 > /dev/null
assert_color cascade-during 20 360 "255 0 0"
pointer_step reveal-motion
assert_active scene-scroll-2 "motion while hover reveal exposes A"
"$UMBRIEL" clock-advance 1000 > /dev/null
"$UMBRIEL" settle > /dev/null
pointer_step after-reveal
assert_active scene-scroll-2 "motion after hover reveal exposes A"
pointer_release
"$UMBRIEL" clock-resume > /dev/null

echo "explicit scrolling deferred hover refresh through retargets and no-ops, then refreshed once without a cascade"
