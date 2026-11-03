#!/usr/bin/env bash
# Native sRGB descriptions keep their provenance across both protocol versions.
# Headless outputs stay SDR: capture transitions verify provenance, while real
# gamma 2.2 and compound-power tags render different dark pixels on that output.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance.blur]
enabled = false
[output.HEADLESS-1]
hdr = "off"
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

readonly CAPTURE_LOG="$UMBRIEL_RUNTIME_DIR/color-transfer-capture.log"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/color-transfer.png"
mkfifo "$UMBRIEL_RUNTIME_DIR/color-transfer-capture.fifo"
CAPTURE_CONTROL=1 "$UMBRIEL_CAPTURE_CLIENT" < "$UMBRIEL_RUNTIME_DIR/color-transfer-capture.fifo" > "$CAPTURE_LOG" 2>&1 &
exec {CAPTURE_FD}> "$UMBRIEL_RUNTIME_DIR/color-transfer-capture.fifo"
await_lines "$CAPTURE_LOG" 'session-ready HEADLESS-1' 1
await_lines "$CAPTURE_LOG" 'frame HEADLESS-1' 1

barrier=0
capture_count() {
  barrier=$((barrier + 1))
  printf 'sync transfer-%s\n' "$barrier" >&"$CAPTURE_FD"
  await_lines "$CAPTURE_LOG" "synced transfer-$barrier HEADLESS-1 " 1
  frame_count=$(awk -v label="transfer-$barrier" '$1 == "synced" && $2 == label && $3 == "HEADLESS-1" { print $4 }' "$CAPTURE_LOG")
  [[ $frame_count =~ ^[0-9]+$ ]]
}

request() {
  local command=$1 previous
  previous=$(events "$CLIENT_LOG" "{\"event\":\"snapshot\",\"label\":\"$command\"")
  printf '%s\n' "$command" >&"$CLIENT_FD"
  await_lines "$CLIENT_LOG" "{\"event\":\"snapshot\",\"label\":\"$command\"" "$((previous + 1))"
}

expect_frames() {
  local command=$1 expected=$2 description=$3 before callbacks
  capture_count
  before=$frame_count
  callbacks=$(events "$CLIENT_LOG" '{"event":"frame",')
  request "$command"
  if [[ $command == frame ]]; then
    await_lines "$CLIENT_LOG" '{"event":"frame",' "$((callbacks + 1))"
  fi
  "$UMBRIEL" settle > /dev/null
  capture_count
  if ((frame_count - before != expected)); then
    echo "$mode: $description changed capture count $before to $frame_count, expected $expected drawn frame(s)"
    exit 1
  fi
}

expect_metadata() {
  local transfer=$1 primaries=$2 color
  color=$("$UMBRIEL" color --json)
  if ! jq -e --arg transfer "$transfer" --arg primaries "$primaries" '
    any(.surfaces[]; .app_id == "color-feedback-client"
      and .transfer_function == $transfer and .primaries == $primaries)
  ' <<< "$color" > /dev/null; then
    echo "$mode: surface did not report $transfer/$primaries: $color"
    exit 1
  fi
}

expect_pixel() {
  local expected=$1 description=$2 tolerance=${3:-1} red green blue
  grim "$IMAGE"
  read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 640 360)
  if ((red < expected - tolerance || red > expected + tolerance
       || green < expected - tolerance || green > expected + tolerance
       || blue < expected - tolerance || blue > expected + tolerance)); then
    echo "$mode: $description rendered $red $green $blue, expected $expected per channel"
    exit 1
  fi
  "$UMBRIEL" settle > /dev/null
}

for mode in v1-toplevel v1-subsurface v1-subsurface-desync v2-toplevel v2-subsurface v2-subsurface-desync; do
  version=${mode%%-*}
  surface_mode=${mode#*-}
  CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/color-transfer-$mode.log"
  fifo="$UMBRIEL_RUNTIME_DIR/color-transfer-$mode.fifo"
  mkfifo "$fifo"
  args=(compound untagged "$version")
  [[ $surface_mode == toplevel ]] || args+=("$surface_mode")
  "$UMBRIEL_COLOR_FEEDBACK_CLIENT" "${args[@]}" < "$fifo" > "$CLIENT_LOG" 2>&1 &
  client_pid=$!
  exec {CLIENT_FD}> "$fifo"
  await_lines "$CLIENT_LOG" '{"event":"mapped"}' 1
  "$UMBRIEL" settle > /dev/null
  expect_metadata none none
  expect_pixel 16 'implicit SDR' 0

  # Both states look the same on this SDR output, but must remain distinct so
  # moving the surface to PQ does not replace its explicit piecewise decoding.
  expect_frames tag 1 'implicit SDR to explicit compound-power transfer'
  # The color IPC reports non-HDR metadata for a view's root surface only.
  if [[ $surface_mode == toplevel ]]; then
    expect_metadata 'compound power 2.4' sRGB
  fi
  expect_pixel 16 'explicit compound-power transfer' 0
  if [[ $version == v2 ]]; then
    expect_frames tag 0 'the same compound-power description'
    expect_frames frame 0 'a callback-only compound-power commit'
    expect_frames unset 1 'explicit compound-power transfer to implicit SDR'
    expect_metadata none none
    expect_frames unset 0 'the same unset description'

    expect_frames gamma22 1 'implicit SDR to explicit gamma 2.2'
    if [[ $surface_mode == toplevel ]]; then
      expect_metadata 'gamma 2.2' sRGB
    fi
    expect_pixel 7 'explicit gamma 2.2'
    expect_frames tag 1 'gamma 2.2 to explicit compound-power transfer'
    expect_pixel 16 'compound-power transfer restored after gamma 2.2'
    expect_frames destroy-only 1 'compound-power object destruction without a surface commit'
    expect_metadata none none
    expect_frames frame 0 'a callback after color-object destruction'
    expect_frames recreate 1 'recreating the compound-power description'
    expect_pixel 16 'compound-power transfer restored after recreation'
  fi

  for value in 29 30; do
    printf -v fill 'ff%02x%02x%02x' "$value" "$value" "$value"
    request "color $fill"
    "$UMBRIEL" settle > /dev/null
    expect_pixel "$value" 'explicit sRGB dark pixel' 0
    request unset
    "$UMBRIEL" settle > /dev/null
    expect_pixel "$value" 'implicit SDR dark pixel' 0
    request tag
    "$UMBRIEL" settle > /dev/null
  done

  kill -TERM "$client_pid"
  wait "$client_pid" || true
  exec {CLIENT_FD}>&-
  "$UMBRIEL" settle > /dev/null
done

echo 'native sRGB transfers preserve dark pixels and stay distinct from implicit SDR and gamma 2.2 across parent and child commits'
