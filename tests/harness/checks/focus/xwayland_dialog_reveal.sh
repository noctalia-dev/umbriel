#!/usr/bin/env bash
# harness: xwayland=true
# A tiled transient takes focus while its parent remains beneath the pointer
# during scrolling reveal. Incidental motion must not steal that focus back.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="$UMBRIEL_XWAYLAND_DIALOG_CLIENT"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/xwayland-dialog.log"
readonly CONTROL="$UMBRIEL_RUNTIME_DIR/xwayland-dialog.control"
readonly PARENT=xwayland-dialog-parent
readonly CHILD=xwayland-dialog-child
pointer_held=false
client_pid=

cleanup() {
  if [[ $pointer_held == true ]]; then pointer_release || true; fi
  "$UMBRIEL" clock-resume > /dev/null 2>&1 || true
  if [[ -n $client_pid ]]; then
    printf 'quit\n' >&"$control_fd" || true
    wait "$client_pid" || true
  fi
}
trap cleanup EXIT

windows() { "$UMBRIEL" windows --json; }
window() { windows | jq -c --arg title "$1" '.[] | select(.title == $title)'; }
send_client() { printf '%s\n' "$@" >&"$control_fd"; }

wait_window() {
  for _ in $(seq 100); do
    [[ -n $(window "$1") ]] && return 0
    sleep 0.025
  done
  echo "$1 did not map: $(windows)"
  cat "$CLIENT_LOG"
  return 1
}

assert_active() {
  local actual
  actual=$(windows | jq -r '.[] | select(.active) | .title')
  [[ $actual == "$1" ]] || { echo "$2: expected $1 active, got $actual: $(windows)"; return 1; }
}

wait_active() {
  for _ in $(seq 100); do
    if [[ $(windows | jq -r '.[] | select(.active) | .title') == "$1" ]]; then return 0; fi
    sleep 0.025
  done
  assert_active "$1" "$2"
}

expect_x_focus() {
  local title=$1 phase=$2 token
  for attempt in $(seq 100); do
    token="${phase}_$attempt"
    send_client "state $token"
    await_lines "$CLIENT_LOG" "state label=$token " 1 0.01
    if grep -q "^state label=$token input=$title active=$title$" "$CLIENT_LOG"; then return 0; fi
    sleep 0.025
  done
  echo "$phase: X input and active window did not become $title"
  cat "$CLIENT_LOG"
  return 1
}

assert_pixel() {
  local phase=$1 x=$2 y=$3 expected=$4 image="$UMBRIEL_RUNTIME_DIR/dialog-$1.png" actual
  grim -o HEADLESS-1 "$image"
  actual=$("$UMBRIEL_PIXEL_PROBE" "$image" pixel "$x" "$y")
  [[ $actual == "$expected" ]] || { echo "$phase: expected $expected at $x,$y, got $actual"; return 1; }
}

type_into() {
  local title=$1 key=$2 phase=$3 before
  before=$(events "$CLIENT_LOG" "key window=$title code=$((key + 8)) state=pressed")
  "$UMBRIEL_POINTER_CLIENT" 1280 720 tap "$key" > /dev/null
  await_events "$CLIENT_LOG" "key window=$title code=$((key + 8)) state=pressed" "$((before + 1))" "$phase"
}

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[layout]
mode = "scrolling"
[layout.scrolling]
default_extent_fraction = 0.5
center_focused = "never"
[animation.windows_in]
enabled = false
[animation.windows_out]
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
follows_mouse = true

[[window_rule]]
match.app_id = "^UmbrielXwaylandDialog$"
default_floating = false
opacity = 1.0
blur = false

[[window_rule]]
match.title = "^xwayland-dialog-parent$"
default_maximize = true
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$CONTROL"
exec {control_fd}<> "$CONTROL"
"$CLIENT" <&"$control_fd" > "$CLIENT_LOG" 2>&1 &
client_pid=$!
wait_window "$PARENT"
wait_active "$PARENT" 'initial parent'
"$UMBRIEL" settle > /dev/null
expect_x_focus "$PARENT" initial
await_events "$CLIENT_LOG" "take-focus window=$PARENT" 1 'globally active parent handshake'
type_into "$PARENT" 30 'initial parent keyboard'
parent_id=$(window "$PARENT" | jq -r .id)

pointer_hold 1280 720 move 128 360 -- move 129 360 mark incidental hold
pointer_held=true
"$UMBRIEL" clock-freeze > /dev/null
assert_pixel before-map 128 360 '48 96 144'
send_client map-dialog
wait_window "$CHILD"
wait_active "$CHILD" 'dialog map'
expect_x_focus "$CHILD" mapped
await_events "$CLIENT_LOG" "take-focus window=$CHILD" 1 'globally active dialog handshake'
child_id=$(window "$CHILD" | jq -r .id)
if ! window "$CHILD" | jq -e --arg parent "$parent_id" \
  '.parent == $parent and .floating == false and .column == 1' > /dev/null; then
  echo "the fixture did not create a forced tiled transient: $(windows)"
  exit 1
fi

# Frozen animation and an actual pixel establish that the pending reveal has
# left the parent under both pointer coordinates. This cannot pass merely
# because an idle scene has already carried the dialog beneath the pointer.
assert_pixel pending-reveal 129 360 '48 96 144'
pointer_step incidental
assert_active "$CHILD" 'one-pixel motion inside the same parent during reveal'
expect_x_focus "$CHILD" incidental
type_into "$CHILD" 31 'dialog keyboard during reveal'
pointer_release
pointer_held=false
"$UMBRIEL" clock-advance 2000 > /dev/null
"$UMBRIEL" settle > /dev/null
assert_active "$CHILD" 'completed dialog reveal'

# An explicit strip scroll intentionally refreshes hover after it settles.
# The pointer stays inside the same parent while the child owns the seat,
# so this also distinguishes that refresh from map-time invalidation.
"$UMBRIEL" msg layout-scroll-left > /dev/null
assert_active "$CHILD" 'pending explicit strip scroll'
"$UMBRIEL" clock-advance 2000 > /dev/null
"$UMBRIEL" settle > /dev/null
assert_pixel scrolled-parent 129 360 '48 96 144'
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 130 360 > /dev/null
wait_active "$PARENT" 'hover refresh after explicit strip scroll'
expect_x_focus "$PARENT" scrolled
"$UMBRIEL" msg window-focus "$child_id" > /dev/null
wait_active "$CHILD" 'child focus after explicit scroll control'
"$UMBRIEL" clock-advance 2000 > /dev/null
"$UMBRIEL" settle > /dev/null

# A real crossing into the parent remains an intentional hover focus change.
child_x=$(window "$CHILD" | jq -r '.x + .w / 2 | floor')
parent_x=$(window "$PARENT" | jq -r '(([.x, 0] | max) + ([.x + .w, 1280] | min)) / 2 | floor')
assert_pixel revealed "$child_x" 360 '96 192 48'
"$UMBRIEL_POINTER_CLIENT" 1280 720 move "$child_x" 360 > /dev/null
wait_active "$CHILD" 'pointer inside revealed dialog'
"$UMBRIEL_POINTER_CLIENT" 1280 720 move "$parent_x" 360 > /dev/null
wait_active "$PARENT" 'geometric entry into parent'
expect_x_focus "$PARENT" entered
"$UMBRIEL" clock-advance 2000 > /dev/null
"$UMBRIEL" settle > /dev/null

# Explicit clicks also override a child's pending reveal while the pointer
# has remained inside the parent. No geometric entry is needed for a click.
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 128 360 > /dev/null
"$UMBRIEL" msg window-focus "$child_id" > /dev/null
wait_active "$CHILD" 'explicit child focus'
assert_pixel pending-click 128 360 '48 96 144'
buttons_before=$(events "$CLIENT_LOG" "button window=$PARENT code=1 state=pressed")
"$UMBRIEL_POINTER_CLIENT" 1280 720 click 272 > /dev/null
wait_active "$PARENT" 'explicit parent click'
expect_x_focus "$PARENT" clicked
await_events "$CLIENT_LOG" "button window=$PARENT code=1 state=pressed" "$((buttons_before + 1))" 'parent click delivery'

# Withdrawing the focused child restores a usable parent and retires its
# pending reveal state rather than retaining a stale dialog focus target.
"$UMBRIEL" msg window-focus "$child_id" > /dev/null
wait_active "$CHILD" 'child focus before withdrawal'
send_client unmap-dialog
for _ in $(seq 100); do
  [[ -z $(window "$CHILD") ]] && break
  sleep 0.025
done
if [[ -n $(window "$CHILD") ]]; then echo "dialog did not withdraw: $(windows)"; exit 1; fi
wait_active "$PARENT" 'parent after dialog withdrawal'
expect_x_focus "$PARENT" withdrawn
type_into "$PARENT" 32 'parent keyboard after dialog withdrawal'
send_client quit
wait "$client_pid"
client_pid=
exec {control_fd}>&-
"$UMBRIEL" clock-resume > /dev/null

echo 'tiled X11 dialog reveal survives incidental parent motion while entry, clicks, and withdrawal retain focus control'
