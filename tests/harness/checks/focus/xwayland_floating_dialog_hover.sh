#!/usr/bin/env bash
# harness: xwayland=true
# A floating transient must keep its keyboard while the pointer travels through
# the parent beneath it. An explicit parent click remains authoritative.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/floating-dialog.log"
readonly CONTROL="$UMBRIEL_RUNTIME_DIR/floating-dialog.control"
readonly PARENT=xwayland-dialog-parent
readonly CHILD=xwayland-dialog-child
pointer_held=false
client_pid=

cleanup() {
  if [[ $pointer_held == true ]]; then pointer_release || true; fi
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

assert_focus() {
  if ! windows | jq -e --arg title "$1" 'any(.[]; .title == $title and .active and .focused)' > /dev/null; then
    echo "$2: expected $1 to hold keyboard focus: $(windows)"
    return 1
  fi
}

wait_focus() {
  for _ in $(seq 100); do
    if windows | jq -e --arg title "$1" 'any(.[]; .title == $title and .active and .focused)' > /dev/null; then return 0; fi
    sleep 0.025
  done
  assert_focus "$1" "$2"
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

type_into() {
  local title=$1 key=$2 phase=$3 before
  before=$(events "$CLIENT_LOG" "key window=$title code=$((key + 8)) state=pressed")
  "$UMBRIEL_POINTER_CLIENT" 1280 720 keyboard-only tap "$key" > /dev/null
  await_events "$CLIENT_LOG" "key window=$title code=$((key + 8)) state=pressed" "$((before + 1))" "$phase"
}

assert_pixel() {
  local phase=$1 x=$2 y=$3 expected=$4 image="$UMBRIEL_RUNTIME_DIR/floating-dialog-$1.png" actual
  grim -o HEADLESS-1 "$image"
  actual=$("$UMBRIEL_PIXEL_PROBE" "$image" pixel "$x" "$y")
  [[ $actual == "$expected" ]] || {
    echo "$phase: expected $expected at $x,$y, got $actual"
    return 1
  }
}

cat >> "$UMBRIEL_CONFIG" << 'CONFIG'
[animation]
enabled = false
[layout]
mode = "scrolling"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[input.focus]
follows_mouse = true
hover_raises = true

[[window_rule]]
match.app_id = "^UmbrielXwaylandDialog$"
opacity = 1.0
blur = false

[[window_rule]]
match.title = "^xwayland-dialog-parent$"
default_floating = false
default_maximize = true

[[window_rule]]
match.title = "^xwayland-dialog-child$"
default_floating = true
default_focused = true
default_floating_size_px = { width = 480, height = 320 }
default_position = { x = 400, y = 200, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$CONTROL"
exec {control_fd}<> "$CONTROL"
"$UMBRIEL_XWAYLAND_DIALOG_CLIENT" <&"$control_fd" > "$CLIENT_LOG" 2>&1 &
client_pid=$!
wait_window "$PARENT"
wait_focus "$PARENT" 'initial parent'
"$UMBRIEL" settle > /dev/null
expect_x_focus "$PARENT" initial
parent_id=$(window "$PARENT" | jq -r .id)

for raises in true false; do
  sed -i "s/^hover_raises = .*/hover_raises = $raises/" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  wait_focus "$PARENT" "hover_raises=$raises parent before map"
  rm -f "$UMBRIEL_RUNTIME_DIR/pointer-hold.log"
  pointer_hold 1280 720 move 128 360 -- \
    move 129 360 mark incidental hold \
    move 260 360 mark approaching hold \
    move 390 360 mark near-child hold \
    move 640 360 mark inside-child hold \
    move 128 360 mark parent-return hold \
    click 272 mark parent-click hold
  pointer_held=true
  assert_pixel "parent-$raises" 128 360 '48 96 144'

  send_client map-dialog
  wait_window "$CHILD"
  wait_focus "$CHILD" "hover_raises=$raises dialog map"
  expect_x_focus "$CHILD" "mapped_$raises"
  "$UMBRIEL" settle > /dev/null
  if ! window "$CHILD" | jq -e --arg parent "$parent_id" \
    '.parent == $parent and .floating and .x == 400 and .y == 200 and .w == 480 and .h == 320' > /dev/null; then
    echo "the fixture did not create the intended floating transient: $(windows)"
    exit 1
  fi
  assert_pixel "child-$raises" 640 360 '96 192 48'
  assert_pixel "parent-after-map-$raises" 129 360 '48 96 144'

  # The first motion remains within the same parent and consumes map-time
  # hover invalidation. Its focus assertion cannot pass through a later entry
  # into the child, and the floating rule excludes the tiled-dialog exception.
  for step in incidental approaching near-child; do
    pointer_step "$step"
    assert_focus "$CHILD" "hover_raises=$raises $step parent motion"
  done
  expect_x_focus "$CHILD" "parent_motion_$raises"
  type_into "$CHILD" 30 "hover_raises=$raises child typing while pointer is over parent"
  pointer_step inside-child
  assert_focus "$CHILD" "hover_raises=$raises pointer reached child"
  expect_x_focus "$CHILD" "inside_$raises"
  type_into "$CHILD" 31 "hover_raises=$raises child typing after pointer entry"

  # Crossing back into the parent is still an intentional hover handoff.
  # Refocus the child with the pointer now stationary over its parent, so only
  # the following click can override the new hover invalidation.
  pointer_step parent-return
  wait_focus "$PARENT" "hover_raises=$raises geometric entry into parent"
  child_id=$(window "$CHILD" | jq -r .id)
  "$UMBRIEL" msg window-focus "$child_id" > /dev/null
  wait_focus "$CHILD" "hover_raises=$raises child focus before parent click"
  expect_x_focus "$CHILD" "before_click_$raises"
  buttons_before=$(events "$CLIENT_LOG" "button window=$PARENT code=1 state=pressed")
  pointer_step parent-click
  wait_focus "$PARENT" "hover_raises=$raises explicit parent click"
  expect_x_focus "$PARENT" "clicked_$raises"
  await_events "$CLIENT_LOG" "button window=$PARENT code=1 state=pressed" "$((buttons_before + 1))" 'parent click delivery'
  type_into "$PARENT" 32 "hover_raises=$raises parent typing after click"
  pointer_release
  pointer_held=false

  send_client unmap-dialog
  for _ in $(seq 100); do
    [[ -z $(window "$CHILD") ]] && break
    sleep 0.025
  done
  if [[ -n $(window "$CHILD") ]]; then
    echo "dialog did not withdraw: $(windows)"
    exit 1
  fi
  wait_focus "$PARENT" "hover_raises=$raises parent after withdrawal"
  expect_x_focus "$PARENT" "withdrawn_$raises"
done

send_client quit
wait "$client_pid"
client_pid=
exec {control_fd}>&-
echo 'floating X11 dialogs retain typing through parent hover, with explicit parent clicks delivered in both hover-raise modes'
