#!/usr/bin/env bash
# harness: outputs=1
# The floating resize verbs animate the presented size, and the keep-visible
# clamp travels with that animation: a float hanging off the left edge shrinks
# without its origin snapping ahead of the size it is being clamped for.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly TITLE=floating-resize-animation
readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/fractional-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/floating-resize-animation.log"

# 1000 wide at x=-900 keeps the 75px minimum on screen. Shrinking to 0.2 of the
# 1280 output (256px) tightens that minimum to 64px, so the origin has to travel
# to -192 and the visible sliver shrinks from 100px to 64px.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 2000
curve = "linear"

[animation.windows_in]
enabled = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[[window_rule]]
match.title = "^floating-resize-animation$"
default_floating = true
default_floating_size_px = { width = 1000, height = 300 }
default_position = { x = -900, y = 200, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

ipc_box() {
  "$UMBRIEL" windows --json \
    | jq -r --arg title "$TITLE" '.[] | select(.title == $title) | "\(.w)x\(.h)+\(.x)+\(.y)"'
}

wait_for_box() {
  local expected=$1 actual=
  for _ in $(seq 100); do
    actual=$(ipc_box)
    [[ $actual == "$expected" ]] && return 0
    sleep 0.05
  done
  echo "expected IPC box $expected, got $actual"
  return 1
}

capture_box() {
  local image="$UMBRIEL_RUNTIME_DIR/$1.png"
  grim "$image"
  "$UMBRIEL_PIXEL_PROBE" "$image" bbox 'r > 0.01 || g > 0.01 || b > 0.01'
}

"$CLIENT" "$TITLE" 1000 300 > "$CLIENT_LOG" 2>&1 &
wait_for_box 1000x300+-900+200
"$UMBRIEL" settle

read -r before_x before_y before_w before_h < <(capture_box before)
if (( before_x != 0 || before_y != 200 || before_w != 100 || before_h != 300 )); then
  echo "float did not open hanging off the left edge: ${before_w}x${before_h}+${before_x}+${before_y}"
  exit 1
fi

# Animation time only moves by clock-advance: samples land at 250 ms and 1250 ms of the 2000 ms linear timeline.
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg window-set-primary-extent 0.2 > /dev/null
"$UMBRIEL" clock-advance 250
read -r first_x first_y first_w first_h < <(capture_box shrink-250ms)
"$UMBRIEL" clock-advance 1000
read -r second_x second_y second_w second_h < <(capture_box shrink-1250ms)

# The sliver stays between its start and end widths at every sample. An origin
# that ignores the requested size lets the float shrink off the output; an origin
# that snaps to it reveals the not yet shrunk buffer and blows past 100px; a
# snapped size lands on 64px before the animation has run.
if ! (( first_x == 0 && second_x == 0
    && first_y == 200 && second_y == 200
    && first_h == 300 && second_h == 300
    && 100 > first_w && first_w > second_w && second_w > 64 )); then
  echo "shrink did not keep origin and size in step: first=${first_w}x${first_h}+${first_x}+${first_y}, second=${second_w}x${second_h}+${second_x}+${second_y}"
  exit 1
fi

"$UMBRIEL" clock-advance 2000
"$UMBRIEL" settle
wait_for_box 256x300+-192+200
read -r after_x after_y after_w after_h < <(capture_box after)
if (( after_x != 0 || after_y != 200 || after_w != 64 || after_h != 300 )); then
  echo "float settled at ${after_w}x${after_h}+${after_x}+${after_y}, expected 64x300+0+200"
  exit 1
fi

echo "floating resize animated the size with the keep-visible clamp"

"$UMBRIEL" clock-resume

# A pointer resize never shows a buffer at a size it was not drawn for: the frame of a slow client stays at its drawn
# size through the grab and release, then lands with its opposite edges fixed once the client redraws. Observe the
# border rather than IPC geometry, which reports client content.
sed -i '/^\[animation\]$/a enabled = false' "$UMBRIEL_CONFIG"
sed -i 's/^border_width = 0$/border_width = 4/' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors.border]
focused = "#FF00FFFF"
unfocused = "#000000FF"

[[window_rule]]
match.title = "^floating-resize-slow$"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 400, y = 250, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

readonly SLOW_LOG="$UMBRIEL_RUNTIME_DIR/floating-resize-slow.log"
fifo="$UMBRIEL_RUNTIME_DIR/floating-resize-slow-control"
mkfifo "$fifo"
exec {control_fd}<>"$fifo"
FILL_COLOR=0xFFFF0000 FOLLOW_CONFIGURES=1 RESIZE_ON_STDIN=1 LOG_CONFIGURES=1 \
  "$UMBRIEL_UNMAP_CLIENT" floating-resize-slow 400 300 < "$fifo" > "$SLOW_LOG" 2>&1 &
await_window floating-resize-slow
if ! "$UMBRIEL" settle; then
  echo "slow client did not settle after map: $(cat "$SLOW_LOG")"
  exit 1
fi

# Magenta distinguishes the focused border from the first client's blue/green stripes.
frame_box() {
  local image="$UMBRIEL_RUNTIME_DIR/slow-$1.png"
  grim "$image"
  "$UMBRIEL_PIXEL_PROBE" "$image" bbox 'r > 0.9 && b > 0.9 && g < 0.1'
}

read -r frame_x frame_y frame_w frame_h < <(frame_box before)
if ((frame_w == 0 || frame_h == 0)); then
  echo "the slow client has no focused border before resizing"
  exit 1
fi

expect_frame() {
  local label=$1 dx=$2 dy=$3 x y w h
  read -r x y w h < <(frame_box "$label")
  if ((x != frame_x - dx || y != frame_y - dy || w != frame_w + dx || h != frame_h + dy)); then
    echo "$label: slow client's frame was ${w}x${h}+${x}+${y}, expected $((frame_w + dx))x$((frame_h + dy))+$((frame_x - dx))+$((frame_y - dy))"
    exit 1
  fi
}

pointer_hold 1280 720 \
  move 402 252 mod logo press 273 move 352 227 \
  -- move 302 202 mark second hold release 273 mod none
await_lines "$SLOW_LOG" 'resize-pending 450x325' 1 0.025
expect_frame first 0 0
pointer_step second
expect_frame second 0 0
pointer_release
# The last target is flushed even though the client has not answered the first one.
await_lines "$SLOW_LOG" 'resize-pending 500x350' 1 0.025
expect_frame released 0 0
echo r >&"$control_fd"
await_lines "$SLOW_LOG" 'resized 500x350' 1 0.025
if ! "$UMBRIEL" settle; then
  echo "slow client did not settle after redraw: $(cat "$SLOW_LOG")"
  exit 1
fi
expect_frame redrawn 100 50

echo "a slow floating client kept its drawn frame through resize and release, then landed with anchored edges"

# A client may acknowledge a smaller size by committing the new window geometry over its old buffer. The buffer is
# cropped to that geometry; drawing it whole leaves pixels outside the frame that later frames never repaint.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.title = "^floating-resize-ahead$"
default_floating = true
default_floating_size_px = { width = 200, height = 100 }
default_position = { x = 20, y = 20, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
readonly AHEAD_LOG="$UMBRIEL_RUNTIME_DIR/floating-resize-ahead.log"
ahead_fifo="$UMBRIEL_RUNTIME_DIR/floating-resize-ahead-control"
mkfifo "$ahead_fifo"
exec {ahead_fd}<>"$ahead_fifo"
FILL_COLOR=0xFFFFFF00 FOLLOW_CONFIGURES=1 RESIZE_ON_STDIN=1 GEOMETRY_AHEAD=1 \
  "$UMBRIEL_UNMAP_CLIENT" floating-resize-ahead 200 100 <&"$ahead_fd" > "$AHEAD_LOG" 2>&1 &
await_window floating-resize-ahead
"$UMBRIEL" settle

content_box() {
  local image="$UMBRIEL_RUNTIME_DIR/ahead-$1.png"
  grim "$image"
  "$UMBRIEL_PIXEL_PROBE" "$image" bbox 'r > 0.9 && g > 0.9 && b < 0.1'
}

pointer_hold 1280 720 move 210 110 mod logo press 273 move 160 110 -- release 273 mod none
await_lines "$AHEAD_LOG" 'resize-pending 150x100' 1 0.025
ahead_w=
for _ in $(seq 40); do
  read -r _ _ ahead_w _ < <(content_box acked)
  ((ahead_w == 150)) && break
  sleep 0.025
done
pointer_release
if ((ahead_w != 150)); then
  echo "a 200px buffer under 150px acknowledged geometry was drawn ${ahead_w}px wide"
  exit 1
fi

echo "a buffer under smaller acknowledged geometry was cropped to it"
