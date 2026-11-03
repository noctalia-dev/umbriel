#!/usr/bin/env bash
# Window captures and output captures must both resolve committed client color.
# Only output scene surfaces inherit compositor opacity, including late children.
# Check numeric pixels as well as matching views so a shared fallback cannot pass.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.blur]
enabled = false
[appearance.shadow]
enabled = false
[output.HEADLESS-1]
hdr = "off"
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

readonly OUTPUT_IMAGE="$UMBRIEL_RUNTIME_DIR/window-color-output.png"
readonly WINDOW_IMAGE="$UMBRIEL_RUNTIME_DIR/window-color-window.png"
readonly CAPTURE_LOG="$UMBRIEL_RUNTIME_DIR/window-color-capture.log"
mkfifo "$UMBRIEL_RUNTIME_DIR/window-color-capture.fifo"
CAPTURE_CONTROL=1 "$UMBRIEL_CAPTURE_CLIENT" < "$UMBRIEL_RUNTIME_DIR/window-color-capture.fifo" > "$CAPTURE_LOG" 2>&1 &
exec {CAPTURE_FD}> "$UMBRIEL_RUNTIME_DIR/window-color-capture.fifo"
await_lines "$CAPTURE_LOG" 'session-ready HEADLESS-1' 1
await_lines "$CAPTURE_LOG" 'frame HEADLESS-1' 1

barrier=0
capture_count() {
  barrier=$((barrier + 1))
  printf 'sync window-color-%s\n' "$barrier" >&"$CAPTURE_FD"
  await_lines "$CAPTURE_LOG" "synced window-color-$barrier HEADLESS-1 " 1
  frame_count=$(awk -v label="window-color-$barrier" '$1 == "synced" && $2 == label && $3 == "HEADLESS-1" { print $4 }' "$CAPTURE_LOG")
  [[ $frame_count =~ ^[0-9]+$ ]]
}

request() {
  local command=$1 previous
  previous=$(events "$CLIENT_LOG" "{\"event\":\"snapshot\",\"label\":\"$command\"")
  printf '%s\n' "$command" >&"$CLIENT_FD"
  await_lines "$CLIENT_LOG" "{\"event\":\"snapshot\",\"label\":\"$command\"" "$((previous + 1))"
  "$UMBRIEL" settle > /dev/null
}

expect_image_pixel() {
  local image=$1 px=$2 py=$3 expected_red=$4 expected_green=$5 expected_blue=$6 description=$7
  local red green blue
  read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$image" pixel "$px" "$py")
  if ((red < expected_red - 1 || red > expected_red + 1
       || green < expected_green - 1 || green > expected_green + 1
       || blue < expected_blue - 1 || blue > expected_blue + 1)); then
    echo "$mode: $description rendered $red $green $blue, expected $expected_red $expected_green $expected_blue"
    exit 1
  fi
}

expect_pixels() {
  local red=$1 green=$2 blue=$3 description=$4
  grim -o HEADLESS-1 "$OUTPUT_IMAGE"
  grim -T "$window_id" "$WINDOW_IMAGE"
  expect_image_pixel "$OUTPUT_IMAGE" "$((x + width / 2))" "$((y + height / 2))" \
    "$red" "$green" "$blue" "$description on the output"
  expect_image_pixel "$WINDOW_IMAGE" "$((width / 2))" "$((height / 2))" \
    "$red" "$green" "$blue" "$description in the window capture"
  "$UMBRIEL" settle > /dev/null
}

expect_callback_without_damage() {
  local before callbacks
  capture_count
  before=$frame_count
  callbacks=$(events "$CLIENT_LOG" '{"event":"frame",')
  request frame
  await_lines "$CLIENT_LOG" '{"event":"frame",' "$((callbacks + 1))"
  "$UMBRIEL" settle > /dev/null
  capture_count
  if ((frame_count != before)); then
    echo "$mode: a callback-only commit changed output capture count $before to $frame_count"
    exit 1
  fi
}

for mode in toplevel subsurface subsurface-desync; do
  CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/window-color-$mode.log"
  fifo="$UMBRIEL_RUNTIME_DIR/window-color-$mode.fifo"
  mkfifo "$fifo"
  args=(compound untagged)
  [[ $mode == toplevel ]] || args+=("$mode")
  "$UMBRIEL_COLOR_FEEDBACK_CLIENT" "${args[@]}" < "$fifo" > "$CLIENT_LOG" 2>&1 &
  client_pid=$!
  exec {CLIENT_FD}> "$fifo"
  await_lines "$CLIENT_LOG" '{"event":"mapped"}' 1
  "$UMBRIEL" settle > /dev/null
  window=$("$UMBRIEL" windows --json | jq -ce '.[] | select(.app_id == "color-feedback-client")')
  read -r x y width height window_id < <(jq -r '"\(.x) \(.y) \(.w) \(.h) \(.id)"' <<< "$window")

  expect_pixels 16 16 16 'implicit SDR'
  request gamma22
  expect_pixels 7 7 7 'explicit gamma 2.2'
  expect_callback_without_damage
  expect_pixels 7 7 7 'gamma 2.2 after a callback-only commit'
  request unset
  expect_pixels 16 16 16 'unset gamma 2.2'
  request tag
  expect_pixels 16 16 16 'explicit compound-power transfer'
  request gamma22
  expect_pixels 7 7 7 'gamma 2.2 before color-object destruction'

  # No client surface commit occurs between destruction and either capture.
  request destroy-only
  expect_pixels 16 16 16 'gamma color-object destruction without a commit'
  request recreate
  expect_pixels 16 16 16 'recreated compound-power description'

  request 'color FF807060'
  expect_pixels 128 112 96 'colored compound-power source'
  request pq
  # ST2084 decodes RGB 128/112/96 at a 203 nit reference white. The standard
  # BT.2020 to sRGB matrix and piecewise sRGB encoding produce 206/128/91.
  # Ignoring primaries would instead produce 181/135/98.
  expect_pixels 206 128 91 'colored PQ with BT.2020 primaries'
  expect_callback_without_damage
  expect_pixels 206 128 91 'PQ after a callback-only commit'
  request unset
  expect_pixels 128 112 96 'unset colored PQ'
  request pq
  expect_pixels 206 128 91 'retagged colored PQ'

  if [[ $mode == subsurface ]]; then
    request cache
    expect_pixels 206 128 91 'PQ while the synchronized unset remains cached'
    request parent
    expect_pixels 128 112 96 'cached unset applied by the parent'
    request apply
    expect_pixels 206 128 91 'pending PQ applied by the child and parent'
  fi

  request destroy-only
  expect_pixels 128 112 96 'PQ color-object destruction without a commit'
  request recreate
  expect_pixels 128 112 96 'compound-power recreation after PQ destruction'
  request pq
  expect_pixels 206 128 91 'PQ restored before client teardown'

  # Keep a real image-copy output session alive while each capture scene and
  # its surfaces disappear. The next mode creates another capture scene.
  capture_count
  before_teardown=$frame_count
  kill -TERM "$client_pid"
  wait "$client_pid" || true
  exec {CLIENT_FD}>&-
  "$UMBRIEL" settle > /dev/null
  capture_count
  if ((frame_count <= before_teardown)); then
    echo "$mode: client teardown did not reach the ongoing output capture"
    exit 1
  fi
done

# The child is created after the parent's 0.5 opacity has settled. Its first
# independent commit must inherit that opacity without a rule or config change.
# The isolated capture presents the same wl_surfaces at their client opacity.
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[colors]
backdrop = "#00FF00FF"
[[window_rule]]
match.app_id = "^capture-opacity-initial$"
default_floating = true
opacity = 0.5
[[window_rule]]
match.app_id = "^capture-opacity-remapped$"
default_floating = true
opacity = 0.25
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null

mode=late-subsurface
readonly OPACITY_LOG="$UMBRIEL_RUNTIME_DIR/window-capture-opacity.log"
mkfifo "$UMBRIEL_RUNTIME_DIR/window-capture-opacity.fifo"
env APP_ID=capture-opacity-initial APP_ID_AFTER_ACTIVATION=capture-opacity-remapped \
  FILL_COLOR=0xFFFFFFFF CONTENT_TYPE_ON_SUBSURFACE=1 CONTENT_TYPE_AFTER_MAP=game REMAP_ON_STDIN=1 \
  "$UMBRIEL_UNMAP_CLIENT" capture-opacity 320 240 \
  < "$UMBRIEL_RUNTIME_DIR/window-capture-opacity.fifo" > "$OPACITY_LOG" 2>&1 &
opacity_client_pid=$!
exec {OPACITY_FD}> "$UMBRIEL_RUNTIME_DIR/window-capture-opacity.fifo"
await_lines "$OPACITY_LOG" 'mapped$' 1

opacity_window() {
  local app_id=$1 window
  for _ in $(seq 60); do
    window=$("$UMBRIEL" windows --json | jq -ce --arg app_id "$app_id" '.[] | select(.app_id == $app_id)') || true
    if [[ -n $window ]]; then
      read -r x y width height window_id < <(jq -r '"\(.x) \(.y) \(.w) \(.h) \(.id)"' <<< "$window")
      return 0
    fi
    sleep 0.05
  done
  echo "$mode: no mapped window with app id $app_id"
  return 1
}

expect_opacity_pixels() {
  local expected=$1 description=$2
  "$UMBRIEL" settle > /dev/null
  grim -o HEADLESS-1 "$OUTPUT_IMAGE"
  grim -T "$window_id" "$WINDOW_IMAGE"
  expect_image_pixel "$OUTPUT_IMAGE" "$((x + width / 2))" "$((y + height / 2))" \
    "$expected" 255 "$expected" "$description on the output"
  expect_image_pixel "$WINDOW_IMAGE" "$((width / 2))" "$((height / 2))" \
    255 255 255 "$description in the isolated window capture"
}

opacity_window capture-opacity-initial
expect_opacity_pixels 128 'the parent at 0.5 compositor opacity'
printf 'u' >&"$OPACITY_FD"
await_lines "$OPACITY_LOG" 'content-type-updated$' 1
expect_opacity_pixels 191 'the late child stacked over its parent at 0.5 opacity'

# Remapping retains the wl_surfaces while the new app id selects 0.25 opacity.
# The first remapped frame must use the new multiplier for both surfaces.
"$UMBRIEL" msg window-close "$window_id" > /dev/null
await_lines "$OPACITY_LOG" 'unmapped$' 1
"$UMBRIEL" settle > /dev/null
printf 'r' >&"$OPACITY_FD"
await_lines "$OPACITY_LOG" 'mapped$' 2
opacity_window capture-opacity-remapped
expect_opacity_pixels 112 'the remapped parent and child at 0.25 opacity'

kill -TERM "$opacity_client_pid"
wait "$opacity_client_pid" || true
exec {OPACITY_FD}>&-

echo 'window and output captures resolved client color and kept compositor opacity on late and remapped output surfaces'
