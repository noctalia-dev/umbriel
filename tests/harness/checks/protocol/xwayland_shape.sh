#!/usr/bin/env bash
# harness: xwayland=true
# Xwayland can export opaque backing pixels outside an X11 bounding shape.
# Observe actual output, overview, and window-capture pixels across shape-only
# transitions; ARGB transparency and the input region are separate controls.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="$UMBRIEL_XWAYLAND_SHAPE_CLIENT"
readonly SCALE=${UMBRIEL_XWAYLAND_SHAPE_SCALE:-1}
readonly OUTPUT_IMAGE="$UMBRIEL_RUNTIME_DIR/xwayland-shape-output.png"
readonly WINDOW_IMAGE="$UMBRIEL_RUNTIME_DIR/xwayland-shape-window.png"

cat >> "$UMBRIEL_CONFIG" <<CONFIG

[animation]
enabled = false
[animation.dim_unfocused]
enabled = false
[colors]
backdrop = "#00FF00FF"
[colors.overview]
background_tint = "#00000000"
workspace_background = "#00FF00FF"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
opaque_fullscreen = true
[appearance.title_bar]
mode = "off"
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = false
[overview]
zoom = 0.5
background_blur = false
workspace_wallpaper = false
shortcuts = false
[output.HEADLESS-1]
mode = "1280x960"
scale = $SCALE
hdr = "off"
[[window_rule]]
match.title = "^xwayland-shape-"
default_floating = true
default_floating_size_px = { width = $((600 / SCALE)), height = $((400 / SCALE)) }
default_position = { x = 100, y = 100, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null

start_client() {
  mode=$1
  local suffix=$2
  shift 2
  title="xwayland-shape-$mode-$suffix"
  CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/$title.log"
  local fifo="$UMBRIEL_RUNTIME_DIR/$title.fifo"
  mkfifo "$fifo"
  exec {CLIENT_FD}<> "$fifo"
  "$CLIENT" "$title" "$mode" "$@" <&"$CLIENT_FD" > "$CLIENT_LOG" 2>&1 &
  client_pid=$!
  await_lines "$CLIENT_LOG" 'done mapped bounding=' 1
  window_id=
  expanded=0
  capture_width=600
  capture_height=400
  if [[ $mode == managed ]]; then
    local window=
    for _ in $(seq 80); do
      window=$("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)')
      [[ -n $window ]] && break
      sleep 0.025
    done
    [[ -n $window ]] || { echo "$title did not map: $(< "$CLIENT_LOG")"; exit 1; }
    read -r x y width height window_id < <(jq -r '"\(.x) \(.y) \(.w) \(.h) \(.id)"' <<< "$window")
    if ((width * SCALE != 600 || height * SCALE != 400)); then
      echo "native X11 geometry differs from the test buffer: ${width}x${height} at scale $SCALE"
      exit 1
    fi
    origin_x=$((x * SCALE))
    origin_y=$((y * SCALE))
  else
    # Override-redirect coordinates already use Xwayland's native pixels.
    origin_x=100
    origin_y=100
  fi
}

stop_client() {
  printf 'quit\n' >&"$CLIENT_FD"
  wait "$client_pid"
  exec {CLIENT_FD}>&-
  "$UMBRIEL" settle > /dev/null
}

request() {
  local command=$1 count
  count=$(events "$CLIENT_LOG" "done $command ")
  printf '%s\n' "$command" >&"$CLIENT_FD"
  await_lines "$CLIENT_LOG" "done $command " "$((count + 1))" 0.025
  "$UMBRIEL" settle > /dev/null
}

output_matches() {
  local left_visible=$1 right_visible=$2 overview=$3 px py sample rgb expected
  local samples=('100 200 left' '450 200 right' '300 200 hole' '25 200 outside' '300 25 top')
  if ((expanded)); then
    samples+=('700 200 expanded')
  fi
  pixels=
  for sample in "${samples[@]}"; do
    read -r px py sample <<< "$sample"
    px=$((origin_x + px))
    py=$((origin_y + py))
    if ((overview)); then
      # zoom 0.5 centers the 1280x960 workspace preview at 320,240.
      px=$((320 + px / 2))
      py=$((240 + py / 2))
    fi
    rgb=$("$UMBRIEL_PIXEL_PROBE" "$OUTPUT_IMAGE" pixel "$px" "$py")
    pixels+=" $sample=$rgb;"
    expected='0 255 0'
    if [[ $sample == left && $left_visible == 1 || $sample == right && $right_visible == 1 || $sample == expanded ]]; then
      expected='255 0 0'
    fi
    [[ $rgb == "$expected" ]] || return 1
  done
}

expect_capture() {
  local label=$1 left_visible=$2 right_visible=$3 sample px py rgb expected
  [[ -n $window_id ]] || return 0
  timeout 10 grim -T "$window_id" "$WINDOW_IMAGE"
  [[ $("$UMBRIEL_PIXEL_PROBE" "$WINDOW_IMAGE" size) == "${capture_width}x${capture_height}" ]] || {
    echo "$label: window capture no longer spans the native ${capture_width}x${capture_height} client"
    exit 1
  }
  # Window capture exposes XRGB. A removed widget must become black even though
  # the client's unchanged backing pixels remain red outside the bounding shape.
  local samples=('100 200 left' '450 200 right' '300 200 hole' '25 200 outside')
  if ((expanded)); then
    samples+=('700 200 expanded')
  fi
  for sample in "${samples[@]}"; do
    read -r px py sample <<< "$sample"
    rgb=$("$UMBRIEL_PIXEL_PROBE" "$WINDOW_IMAGE" pixel "$px" "$py")
    expected='0 0 0'
    if [[ $sample == left && $left_visible == 1 || $sample == right && $right_visible == 1 || $sample == expanded ]]; then
      expected='255 0 0'
    fi
    if [[ $rgb != "$expected" ]]; then
      echo "$label: capture $sample=$rgb, expected $expected"
      exit 1
    fi
  done
}

expect_state() {
  local label=$1 left_visible=$2 right_visible=$3 overview=${4:-0} matched=0
  # The X11 acknowledgement and Wayland capture travel over different sockets.
  # Poll rendered frames instead of assuming the shape watcher has run already.
  for _ in $(seq 80); do
    grim -o HEADLESS-1 "$OUTPUT_IMAGE"
    if output_matches "$left_visible" "$right_visible" "$overview"; then
      matched=1
      break
    fi
    sleep 0.025
  done
  if ((!matched)); then
    echo "$mode $label at scale $SCALE:$pixels"
    cat "$CLIENT_LOG"
    exit 1
  fi
  expect_capture "$label" "$left_visible" "$right_visible"
}

# An unshaped ARGB window must retain its own transparency.
start_client managed unshaped unshaped
expect_state 'unshaped ARGB control' 1 1
stop_client

for mode in managed unmanaged; do
  start_client "$mode" shaped
  expect_state 'bounding shape present before map' 1 1

  if [[ $mode == managed ]]; then
    "$UMBRIEL" msg overview-open > /dev/null
    "$UMBRIEL" settle > /dev/null
    expect_state 'initial overview mirror' 1 1 1
    request 'shape left'
    expect_state 'shape-only update in overview' 1 0 1
    request 'shape both'
    expect_state 'restored overview mirror' 1 1 1
    "$UMBRIEL" msg overview-close > /dev/null
    "$UMBRIEL" settle > /dev/null
  fi

  # Shrinking does not repaint or emit expose events. The old right-hand widget
  # remains in the buffer and only a bounding-shape update can hide it.
  request 'shape left'
  expect_state 'left rectangle after shrinking' 1 0
  request 'shape right'
  expect_state 'region moved to the other rectangle' 0 1
  request 'shape empty'
  expect_state 'empty bounding region' 0 0
  request 'shape both'
  expect_state 'restored from empty region' 1 1

  request 'input empty'
  expect_state 'empty input region leaves both widgets visible' 1 1
  request 'input clear'
  expect_state 'restored input region' 1 1

  request 'shape clear'
  expect_state 'removed bounding shape preserves ARGB transparency' 1 1
  request 'shape both'
  expect_state 'bounding shape reapplied after removal' 1 1

  request 'shape clear'
  request resize
  await_lines "$CLIENT_LOG" 'resized 800x500' 1
  "$UMBRIEL" settle > /dev/null
  expanded=1
  capture_width=800
  capture_height=500
  if [[ $mode == managed ]]; then
    window=$("$UMBRIEL" windows --json | jq -c --arg id "$window_id" '.[] | select(.id == $id)')
    read -r x y width height < <(jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$window")
    if ((width * SCALE != capture_width || height * SCALE != capture_height)); then
      echo "clearing the shape then resizing did not update native geometry: $window"
      exit 1
    fi
    origin_x=$((x * SCALE))
    origin_y=$((y * SCALE))
  fi
  expect_state 'resizing after shape removal exposes new window pixels' 1 1
  stop_client
done

expect_fullscreen_cutout() {
  local label=$1 expected=$2 matched=0 hole widget outside
  for _ in $(seq 80); do
    grim -o HEADLESS-1 "$OUTPUT_IMAGE"
    hole=$("$UMBRIEL_PIXEL_PROBE" "$OUTPUT_IMAGE" pixel 300 300)
    widget=$("$UMBRIEL_PIXEL_PROBE" "$OUTPUT_IMAGE" pixel 100 200)
    outside=$("$UMBRIEL_PIXEL_PROBE" "$OUTPUT_IMAGE" pixel 900 300)
    if [[ $hole == "$expected" && $widget == '255 0 0' && $outside == '0 255 0' ]]; then
      matched=1
      break
    fi
    sleep 0.025
  done
  if ((!matched)); then
    echo "$label at scale $SCALE: fullscreen hole=$hole widget=$widget outside=$outside"
    exit 1
  fi
}

# A fullscreen shape must reveal another window, even with opaque_fullscreen.
# The central hole at native pixel 300,300 overlaps the background's left red
# widget at both scale 1 and native scale 2. A fullscreen backdrop would hide it.
start_client managed fullscreen-background unshaped
background_pid=$client_pid
background_fd=$CLIENT_FD
start_client managed fullscreen-foreground
"$UMBRIEL" msg "window-focus:$window_id" > /dev/null
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
for _ in $(seq 80); do
  window=$("$UMBRIEL" windows --json | jq -c --arg id "$window_id" '.[] | select(.id == $id)')
  if jq -e --argjson scale "$SCALE" \
    '.fullscreen and .x == 0 and .y == 0 and .w * $scale == 1280 and .h * $scale == 960' <<< "$window" > /dev/null; then
    break
  fi
  sleep 0.025
done
if ! jq -e --argjson scale "$SCALE" \
  '.fullscreen and .x == 0 and .y == 0 and .w * $scale == 1280 and .h * $scale == 960' <<< "$window" > /dev/null; then
  echo "shaped foreground did not enter fullscreen: $window"
  exit 1
fi
"$UMBRIEL" settle > /dev/null
expect_fullscreen_cutout 'fullscreen shape reveals the window behind it' '255 0 0'
request 'shape clear'
expect_fullscreen_cutout 'removing the shape restores the opaque fullscreen backdrop' '0 255 0'
request 'shape both'
expect_fullscreen_cutout 'restoring the shape reveals the window again' '255 0 0'
stop_client
client_pid=$background_pid
CLIENT_FD=$background_fd
stop_client

echo "X11 bounding regions clip initial, updated, empty, restored, and unshaped ARGB windows at scale $SCALE"
