#!/usr/bin/env bash
# harness: xwayland=true
# harness: xwayland_proxy=association
# X11 geometry can be queued while the first Wayland buffer is already ready.
# Delay that real geometry event, then inspect every captured output frame.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CAPTURE_LOG="$UMBRIEL_RUNTIME_DIR/initial-position-capture.log"
readonly PROXY_LOG="$UMBRIEL_XWAYLAND_ASSOCIATION_LOG"
readonly PROXY_CONTROL="$UMBRIEL_XWAYLAND_ASSOCIATION_CONTROL"
client_pid=
capture_pid=
sequence=0

cleanup_client() {
  if [[ -n $client_pid ]]; then
    printf 'quit\n' >&"$control_fd" || true
    wait "$client_pid" || true
    client_pid=
    exec {control_fd}>&-
  fi
}
cleanup() {
  cleanup_client
  if [[ -n $capture_pid ]]; then
    kill "$capture_pid" 2>/dev/null || true
    wait "$capture_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[animation.dim_unfocused]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.title_bar]
mode = "off"
[appearance.shadow]
enabled = false
[colors]
backdrop = "#00FF00FF"
[[window_rule]]
match.app_id = "steam"
default_floating = true
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
CAPTURE_PROBES='640 360 1080 630' "$UMBRIEL_CAPTURE_CLIENT" --output HEADLESS-1 > "$CAPTURE_LOG" 2>&1 &
capture_pid=$!
await_lines "$CAPTURE_LOG" 'probe HEADLESS-1 .*x=640 y=360 rgb=00ff00' 1 0.01

send() { printf '%s\n' "$@" >&"$control_fd"; }
mark() { echo $(($(wc -l < "$CAPTURE_LOG") + 1)); }

start_client() {
  local name=$1 fifo="$UMBRIEL_RUNTIME_DIR/initial-position-$1.control"
  client_log="$UMBRIEL_RUNTIME_DIR/initial-position-$name.log"
  mkfifo "$fifo"
  exec {control_fd}<> "$fifo"
  "$UMBRIEL_XWAYLAND_POPUP_CLIENT" "notificationtoasts_$name" <&"$control_fd" > "$client_log" 2>&1 &
  client_pid=$!
  await_lines "$client_log" ready 1 0.01
  window=$(sed -n 's/^ready window=//p' "$client_log")
  send 'class steam' 'geometry 490 280 300 160'
}

await_probe() {
  local since=$1 x=$2 y=$3 rgb=$4
  for _ in $(seq 100); do
    if tail -n +"$since" "$CAPTURE_LOG" | grep -q "^probe HEADLESS-1 .*x=$x y=$y rgb=$rgb$"; then return 0; fi
    sleep 0.01
  done
  echo "no captured pixel $x,$y=$rgb after line $since"
  cat "$CAPTURE_LOG"
  exit 1
}

expect_x_position() {
  local map=$1 token
  for _ in $(seq 100); do
    sequence=$((sequence + 1))
    token="position_$sequence"
    send "state $token"
    await_lines "$client_log" "state label=$token " 1 0.01
    if grep -q "^state label=$token map=$map .*x=930 y=550 w=300 h=160 " "$client_log"; then return 0; fi
    sleep 0.01
  done
  echo 'the X server did not apply the client position'
  cat "$client_log"
  exit 1
}

expect_blank() {
  local phase=${1:-pending-placement} image="$UMBRIEL_RUNTIME_DIR/initial-position-held.png" center corner
  "$UMBRIEL" settle > /dev/null
  grim -o HEADLESS-1 "$image"
  center=$("$UMBRIEL_PIXEL_PROBE" "$image" pixel 640 360)
  corner=$("$UMBRIEL_PIXEL_PROBE" "$image" pixel 1080 630)
  if [[ $center != '0 255 0' || $corner != '0 255 0' ]]; then
    echo "$phase: window rendered before pending X11 placement was dispatched: center=$center corner=$corner"
    exit 1
  fi
}

expect_no_center_frame() {
  if tail -n +"$1" "$CAPTURE_LOG" | grep -q '^probe HEADLESS-1 .*x=640 y=360 rgb=306090$'; then
    echo 'a first output frame displayed the client at its obsolete centered position'
    cat "$CAPTURE_LOG"
    exit 1
  fi
}

stop_client() {
  local since
  since=$(mark)
  cleanup_client
  await_probe "$since" 1080 630 00ff00
}

hold_position() {
  proxy_start=$(($(wc -l < "$PROXY_LOG") + 1))
  printf 'hold-position %s\n' "$window" > "$PROXY_CONTROL"
  await_proxy "armed-position window=$window$" 1
}

await_proxy() {
  local pattern=$1 expected=$2 count
  for _ in $(seq 100); do
    count=$(tail -n +"$proxy_start" "$PROXY_LOG" | grep -c "$pattern" || true)
    [[ ${count:-0} -ge $expected ]] && return 0
    sleep 0.01
  done
  echo "missing delayed X11 event: $pattern"
  tail -n +"$proxy_start" "$PROXY_LOG"
  exit 1
}

await_held() {
  local fences=${1:-1}
  await_proxy "held-position window=$window " "$fences"
  # This confirms association and map have run while the real position event
  # is delayed. The Wayland buffer can commit independently of this XWM stream.
  await_proxy "held-position-fence window=$window$" "$fences"
  expect_x_position 2
}

release_position() {
  local since=$1
  printf 'release-position %s\n' "$window" > "$PROXY_CONTROL"
  await_probe "$since" 1080 630 306090
  expect_no_center_frame "$since"
}

# A genuinely client-requested center position must be observable by the
# capture client. Moving this already shown window exercises the opposite pixel.
start_client observer-control
send 'override true'
since=$(mark)
send map
await_probe "$since" 640 360 306090
since=$(mark)
send 'geometry 930 550 300 160'
await_probe "$since" 1080 630 306090
stop_client

for mode in override notification override-notification; do
  for order in map-move move-map; do
    start_client "$mode-$order"
    if [[ $mode != override ]]; then send 'type notification'; fi
    if [[ $mode != notification ]]; then send 'override true'; fi
    hold_position
    since=$(mark)
    send "$order"
    await_held
    expect_blank
    expect_no_center_frame "$since"
    release_position "$since"
    stop_client
  done
done

# The first map's held fence must not reveal a later map of the same X window.
start_client remap
send 'override true' 'type notification'
hold_position
since=$(mark)
send map-move
await_held
expect_blank
send unmap
await_lines "$client_log" unmapped 1 0.01
expect_x_position 0
send 'geometry 490 280 300 160' map-move
await_held 2
expect_blank
printf 'release-oldest-position %s\n' "$window" > "$PROXY_CONTROL"
await_proxy "released-oldest-position window=$window " 1
expect_blank stale-map-fence
expect_no_center_frame "$since"
release_position "$since"
stop_client

echo 'first X11 frames wait for queued placement, preserve client map/move ordering, and reject stale remap fences'
