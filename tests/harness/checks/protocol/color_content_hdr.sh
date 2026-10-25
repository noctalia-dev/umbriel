#!/usr/bin/env bash
# A PQ transport does not make SDR-limited content HDR. Real committed metadata
# must move the automatic output request in both directions without changing TF.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[output.HEADLESS-1]
hdr = "auto"
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

request() {
  local command=$1 previous
  previous=$(events "$CLIENT_LOG" "{\"event\":\"snapshot\",\"label\":\"$command\"")
  printf '%s\n' "$command" >&"$CLIENT_FD"
  await_lines "$CLIENT_LOG" "{\"event\":\"snapshot\",\"label\":\"$command\"" "$((previous + 1))"
  "$UMBRIEL" settle > /dev/null
}

expect_request() {
  local requested=$1 fullscreen=$2 phase=$3 color windows
  for _ in $(seq 60); do
    color=$("$UMBRIEL" color --json)
    windows=$("$UMBRIEL" windows --json)
    if jq -e --argjson requested "$requested" '
      .outputs[0].hdr_mode == "auto" and .outputs[0].hdr_requested == $requested
      and .outputs[0].hdr_active == false
      and .outputs[0].fallback_reason == (if $requested then "display does not advertise PQ" else "" end)
    ' <<< "$color" > /dev/null \
      && jq -e --argjson fullscreen "$fullscreen" '
        any(.[]; .app_id == "color-feedback-client" and .fullscreen == $fullscreen)
      ' <<< "$windows" > /dev/null; then
      return 0
    fi
    sleep 0.05
  done
  echo "$mode: $phase did not request automatic HDR=$requested fullscreen=$fullscreen: color=$color windows=$windows"
  exit 1
}

expect_peak() {
  local peak=$1 color
  color=$("$UMBRIEL" color --json)
  # SDR child metadata is intentionally absent from root HDR diagnostics.
  # A root tag and HDR-tagged descendants must preserve their original PQ tag.
  if ! jq -e --argjson peak "$peak" '
    any(.surfaces[]; .app_id == "color-feedback-client" and .transfer_function == "PQ"
      and .primaries == "BT.2020" and .max_cll == $peak and .max_fall == 203)
  ' <<< "$color" > /dev/null; then
    echo "$mode: source PQ metadata changed while selecting output policy: $color"
    exit 1
  fi
}

for mode in toplevel subsurface subsurface-desync; do
  CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/content-light-$mode.log"
  fifo="$UMBRIEL_RUNTIME_DIR/content-light-$mode.fifo"
  mkfifo "$fifo"
  args=(content-light)
  [[ $mode == toplevel ]] || args+=("$mode")
  "$UMBRIEL_COLOR_FEEDBACK_CLIENT" "${args[@]}" < "$fifo" > "$CLIENT_LOG" 2>&1 &
  client_pid=$!
  exec {CLIENT_FD}> "$fifo"
  await_lines "$CLIENT_LOG" '{"event":"mapped"}' 1
  "$UMBRIEL" settle > /dev/null

  expect_request false true 'PQ SDR peak 203 at map'
  [[ $mode != toplevel ]] || expect_peak 203
  request 'content 1'
  expect_request true true 'PQ HDR peak 1000 with SDR frame average 203'
  expect_peak 1000
  request 'content 0'
  expect_request false true 'return to PQ SDR peak 203'
  request 'content 3'
  expect_request true true 'HDR mastering peak 1000 despite default content peak 203'
  expect_peak 203
  request 'content 0'
  expect_request false true 'HDR mastering metadata removed from SDR content'
  request 'content 2'
  expect_request true true 'PQ unknown peak with SDR frame average 203'
  expect_peak 0
  request windowed
  expect_request false false 'HDR metadata on a windowed surface'
  request fullscreen
  expect_request true true 'HDR metadata restored to fullscreen'
  request unset
  expect_request false true 'metadata removed from fullscreen'
  request 'content 0'
  expect_request false true 'PQ SDR cap restored after unset'

  kill -TERM "$client_pid"
  wait "$client_pid" 2>/dev/null || true
  exec {CLIENT_FD}>&-
  "$UMBRIEL" settle > /dev/null
done

echo "automatic HDR follows committed content peaks, preserving SDR-limited PQ and unknown-peak HDR across fullscreen and child surfaces"
