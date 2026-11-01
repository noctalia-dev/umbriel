#!/usr/bin/env bash
# harness: xwayland=true
# A notification must retain client geometry from its first visible frame, even
# before a later override-redirect change could rescue a misclassified window.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly NATIVE_TITLE=xwayland-notification-native
readonly NATIVE_LOG="$UMBRIEL_RUNTIME_DIR/notification-native.log"
client_pid=
native_pid=
sequence=0

cleanup() {
  if [[ -n $client_pid ]]; then
    printf 'quit\n' >&"$control_fd" || true
    wait "$client_pid" || true
  fi
  if [[ -n $native_pid ]]; then
    kill "$native_pid" 2>/dev/null || true
    wait "$native_pid" 2>/dev/null || true
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
match.title = "^xwayland-notification-native$"
default_floating = true
default_floating_size_px = { width = 400, height = 200 }
default_position = { x = 20, y = 20, anchor = "top_left" }

# The reporter's rules. Toast titles do not match the main window's title rule.
[[window_rule]]
match.app_id = "steam"
default_floating = true
[[window_rule]]
match.title = "Steam"
default_floating = false
default_maximize = true
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

views() { "$UMBRIEL" windows --json | jq -c '[.[] | select(.app_id == "steam")]'; }
send() { printf '%s\n' "$@" >&"$control_fd"; }

start_client() {
  local name=$1 fifo="$UMBRIEL_RUNTIME_DIR/notification-$1.control"
  client_log="$UMBRIEL_RUNTIME_DIR/notification-$name.log"
  mkfifo "$fifo"
  exec {control_fd}<> "$fifo"
  "$UMBRIEL_XWAYLAND_POPUP_CLIENT" "notificationtoasts_$name" <&"$control_fd" > "$client_log" 2>&1 &
  client_pid=$!
  await_lines "$client_log" ready 1 0.025
  send 'class steam' 'geometry 930 550 300 160'
}

stop_client() {
  send quit
  wait "$client_pid"
  client_pid=
  exec {control_fd}>&-
}

sync_client() {
  local before
  before=$(events "$client_log" "synced $1")
  send "sync $1"
  await_lines "$client_log" "synced $1" "$((before + 1))" 0.01
}

expect_state() {
  local pattern=$1 x_focus=${2:-} token
  for _ in $(seq 100); do
    sequence=$((sequence + 1))
    token="notification_$sequence"
    send "state $token"
    await_lines "$client_log" "state label=$token " 1 0.01
    if grep -qE "^state label=$token $pattern$" "$client_log"; then
      if [[ -z $x_focus ]] || grep -qE "^focus label=$token input=($x_focus) active=0$" "$client_log"; then
        return 0
      fi
    fi
    sleep 0.025
  done
  echo "notification did not reach X11 state: $pattern"
  cat "$client_log"
  exit 1
}

expect_unmanaged() {
  for _ in $(seq 100); do
    [[ $(views | jq length) == 0 ]] && return 0
    sleep 0.025
  done
  echo "notification remained managed: $(views)"
  exit 1
}

expect_notification() {
  # Lazy Xwayland may start at PointerRoot; an actual X-to-native handoff must clear it to None.
  local override=$1 x_focus=${2:-'0|1'}
  expect_state "map=2 override=$override type=notification x=930 y=550 w=300 h=160 input=false active=false .*max_horz=false max_vert=false fullscreen=false" "$x_focus"
  expect_unmanaged
}

expect_client_geometry() {
  local since=${1:-} history
  sync_client geometry
  history=$(awk -v since="$since" '
    !since { ready = 1 }
    $0 == "synced " since { ready = 1; next }
    ready && /^configure / { print }
  ' "$client_log")
  if [[ -z $history ]] || grep -qEv '^configure x=930 y=550 w=300 h=160 override=(true|false)$' <<< "$history"; then
    echo "notification received a compositor placement or resize: $history"
    exit 1
  fi
}

expect_corner_pixels() {
  local image="$UMBRIEL_RUNTIME_DIR/notification.png" center corner
  "$UMBRIEL" settle > /dev/null
  grim -o HEADLESS-1 "$image"
  center=$("$UMBRIEL_PIXEL_PROBE" "$image" pixel 640 360)
  corner=$("$UMBRIEL_PIXEL_PROBE" "$image" pixel 1080 630)
  if [[ $center != '0 255 0' || $corner != '48 96 144' ]]; then
    echo "notification pixels: center=$center corner=$corner"
    exit 1
  fi
}

expect_native_held() {
  "$UMBRIEL" settle > /dev/null
  if [[ $(events "$NATIVE_LOG" keyboard-leave) != "$native_leaves" ]]; then
    echo "notification took native keyboard focus: $(cat "$NATIVE_LOG")"
    exit 1
  fi
  "$UMBRIEL" windows --json | jq -e --arg title "$NATIVE_TITLE" '.[] | select(.title == $title) | .active' > /dev/null
}

"$UMBRIEL_SEAT_LOG_CLIENT" "$NATIVE_TITLE" > "$NATIVE_LOG" 2>&1 &
native_pid=$!
await_lines "$NATIVE_LOG" keyboard-enter 1 0.025
native_id=$("$UMBRIEL" windows --json | jq -r --arg title "$NATIVE_TITLE" '.[] | select(.title == $title) | .id')
native_leaves=$(events "$NATIVE_LOG" keyboard-leave)

# No Input=False hint and no OR flag: NOTIFICATION itself owns the policy.
start_client before-map
send 'type notification' map
await_lines "$client_log" mapped 1 0.025
"$UMBRIEL" settle > /dev/null
expect_client_geometry
expect_native_held
expect_notification false
expect_corner_pixels
stop_client

# The same client is a centered managed window until its late notification type.
start_client late-type
send map
for _ in $(seq 100); do
  [[ $(views | jq length) == 1 ]] && break
  sleep 0.025
done
views | jq -e 'length == 1 and .[0].floating and (.[0].x == 490) and (.[0].y == 280)' > /dev/null
"$UMBRIEL" msg window-focus "$native_id" > /dev/null
await_lines "$NATIVE_LOG" keyboard-enter 2 0.025
native_leaves=$(events "$NATIVE_LOG" keyboard-leave)
sync_client before-notification
send 'type notification'
expect_notification false 0
expect_client_geometry before-notification
expect_native_held
expect_corner_pixels
stop_client

# A popup already has the unmanaged wrapper, so the type update must also release
# its keyboard without recreating or repositioning the surface.
start_client popup-type
send 'type popup' map
expect_state 'map=2 override=false type=popup x=930 y=550 w=300 h=160 input=true active=true .*'
native_enters=$(events "$NATIVE_LOG" keyboard-enter)
send 'type notification'
await_lines "$NATIVE_LOG" keyboard-enter "$((native_enters + 1))" 0.025
native_leaves=$(events "$NATIVE_LOG" keyboard-leave)
expect_notification false 0
expect_client_geometry
expect_native_held
expect_corner_pixels
send unmap
expect_state 'map=0 override=false type=notification .*'
send map
await_lines "$client_log" mapped 2 0.025
expect_notification false 0
expect_native_held
expect_client_geometry
stop_client

# Even an application rule forcing maximize and tiling must not own a toast.
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[[window_rule]]
match.app_id = "^steam$"
default_floating = false
default_maximize = true
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
for mode in broad override no-input; do
  start_client "$mode"
  send 'type notification'
  override=false
  if [[ $mode == override ]]; then
    send 'override true'
    override=true
  elif [[ $mode == no-input ]]; then
    send 'input false'
  fi
  send map
  await_lines "$client_log" mapped 1 0.025
  "$UMBRIEL" settle > /dev/null
  expect_client_geometry
  expect_native_held
  expect_notification "$override"
  expect_corner_pixels
  stop_client
done

native_keys=$(events "$NATIVE_LOG" 'keyboard-key code=39 state=pressed')
"$UMBRIEL_POINTER_CLIENT" 1280 720 keyboard-only tap 39 > /dev/null
await_lines "$NATIVE_LOG" 'keyboard-key code=39 state=pressed' "$((native_keys + 1))" 0.025

echo 'X11 notifications retain client geometry from first map, bypass Steam rules, and preserve native keyboard focus'
