#!/usr/bin/env bash
# harness: xwayland=true
# harness: xwayland_proxy=association
# A first Wayland buffer can commit before the corresponding X11 association.
# Require the association itself to map it, without a later client repaint.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="$UMBRIEL_XWAYLAND_ASSOCIATION_CLIENT"
readonly PROXY_LOG="$UMBRIEL_XWAYLAND_ASSOCIATION_LOG"
readonly PROXY_CONTROL="$UMBRIEL_XWAYLAND_ASSOCIATION_CONTROL"
declare -A CLIENT_FDS=() CLIENT_PIDS=() CLIENT_LOGS=() CLIENT_WINDOWS=()

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = false

[[window_rule]]
match.title = "^xwayland-association-"
default_floating = true
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

wait_line() {
  local file=$1 pattern=$2 label=$3
  for _ in $(seq 100); do
    if grep -qE "$pattern" "$file"; then
      return 0
    fi
    sleep 0.02
  done
  echo "missing $label: $pattern"
  cat "$file"
  return 1
}

start_client() {
  local name=$1 width=$2 height=$3 mode=${4:-managed} fd
  local fifo="$UMBRIEL_RUNTIME_DIR/association-$name.control"
  CLIENT_LOGS[$name]="$UMBRIEL_RUNTIME_DIR/association-$name.log"
  mkfifo "$fifo"
  exec {fd}<>"$fifo"
  CLIENT_FDS[$name]=$fd
  "$CLIENT" "xwayland-association-$name" "$width" "$height" "$mode" <&"$fd" > "${CLIENT_LOGS[$name]}" 2>&1 &
  CLIENT_PIDS[$name]=$!
  wait_line "${CLIENT_LOGS[$name]}" '^ready window=[0-9]+$' "$name X11 window creation"
  CLIENT_WINDOWS[$name]=$(sed -n 's/^ready window=//p' "${CLIENT_LOGS[$name]}")
}

send_client() {
  local name=$1
  shift
  printf '%s\n' "$@" >&"${CLIENT_FDS[$name]}"
}

expect_state() {
  local name=$1 label=$2 expected=$3
  for attempt in $(seq 100); do
    local token="${label}_$attempt"
    send_client "$name" "state $token"
    wait_line "${CLIENT_LOGS[$name]}" "^state label=$token " "$name X11 state reply"
    if grep -q "^state label=$token $expected$" "${CLIENT_LOGS[$name]}"; then
      return 0
    fi
    sleep 0.02
  done
  echo "$name did not reach $label: $expected"
  cat "${CLIENT_LOGS[$name]}"
  return 1
}

expect_view() {
  local name=$1 expected=$2
  for _ in $(seq 100); do
    local count
    count=$("$UMBRIEL" windows --json | jq --arg title "xwayland-association-$name" \
      '[.[] | select(.title == $title and .xwayland and .visible)] | length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.02
  done
  echo "$name expected $expected visible Xwayland view(s)"
  "$UMBRIEL" windows --json
  return 1
}

stop_client() {
  local name=$1 fd
  send_client "$name" quit
  wait "${CLIENT_PIDS[$name]}"
  fd=${CLIENT_FDS[$name]}
  exec {fd}>&-
}

# Keep the normal client alive as an association-order control and to prevent
# lazy Xwayland teardown between the normal and delayed cases.
start_client normal 479 353
send_client normal map
expect_view normal 1
expect_state normal mapped 'map_state=2 listed=true stacked=true'
normal_window=${CLIENT_WINDOWS[normal]}
if grep -q "held window=$normal_window " "$PROXY_LOG"; then
  echo 'the normal association was unexpectedly held'
  exit 1
fi

delayed_case() {
  local name=$1 width=$2 height=$3 mode=$4 stacked=$5 view_count=$6
  local window before after
  start_client "$name" "$width" "$height" "$mode"
  window=${CLIENT_WINDOWS[$name]}
  printf 'hold %s\n' "$window" > "$PROXY_CONTROL"
  wait_line "$PROXY_LOG" "armed window=$window$" 'association hold acknowledgement'
  send_client "$name" map
  wait_line "$PROXY_LOG" "held window=$window " 'held X11 association'

  # Each unique initial buffer size identifies an applied wl_surface buffer
  # while the X11 association is still withheld. Polling the actual import
  # avoids assuming a fixed delay is sufficient on a loaded machine.
  wait_line "$UMBRIEL_LOG" "Created GL FBO for buffer ${width}x${height}$" 'first buffer before association'
  expect_state "$name" before_association "map_state=2 listed=false stacked=$stacked"
  expect_view "$name" 0
  before=$(grep -c "mapping buffered X11 window=$window on association" "$UMBRIEL_LOG" || true)
  printf 'release %s\n' "$window" > "$PROXY_CONTROL"
  wait_line "$PROXY_LOG" "released window=$window events=1$" 'association release'
  expect_state "$name" associated "map_state=2 listed=true stacked=$stacked"
  expect_view "$name" "$view_count"
  wait_line "$UMBRIEL_LOG" "mapping buffered X11 window=$window on association$" 'buffered association catch-up'
  after=$(grep -c "mapping buffered X11 window=$window on association" "$UMBRIEL_LOG" || true)
  if ((after != before + 1)); then
    echo "$name expected one buffered association, got $((after - before))"
    return 1
  fi
  if [[ $mode == unmanaged ]]; then
    local image="$UMBRIEL_RUNTIME_DIR/association-$name.png" pixel
    grim -o HEADLESS-1 "$image"
    pixel=$("$UMBRIEL_PIXEL_PROBE" "$image" pixel 70 70)
    if [[ $pixel != '48 96 144' ]]; then
      echo "$name did not draw its committed buffer: $pixel"
      return 1
    fi
  fi
  "$UMBRIEL" settle > /dev/null
  [[ $mode == unmanaged ]] && return 0

  # The same managed X window can withdraw and create a new Wayland surface.
  # Removal from stacking acknowledges that XWM has processed its withdrawal.
  # No association hold remains, and no extra repaint rescues either map.
  send_client "$name" unmap
  expect_state "$name" withdrawn 'map_state=0 listed=false stacked=false'
  expect_view "$name" 0
  send_client "$name" map
  expect_state "$name" remapped "map_state=2 listed=true stacked=$stacked"
  expect_view "$name" "$view_count"
}

delayed_case delayed 481 359 managed true 1
delayed_case unmanaged 487 367 unmanaged false 0
stop_client unmanaged
stop_client delayed
stop_client normal
echo 'X11 association maps a committed first buffer, and normal unmap/remap still works'
