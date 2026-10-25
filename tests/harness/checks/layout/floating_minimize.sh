#!/usr/bin/env bash
# Floating windows minimize through the action, the client, and foreign-toplevel; every explicit focus path shows
# them again, automatic focus skips them, and tiling a minimized window shows it.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly FOREIGN_TOPLEVEL="${UMBRIEL_FOREIGN_TOPLEVEL_CLIENT:-./build-debug/tests/foreign-toplevel-client}"
readonly A_LOG="$UMBRIEL_RUNTIME_DIR/minimize-a.log"
readonly B_LOG="$UMBRIEL_RUNTIME_DIR/minimize-b.log"
readonly EVENTS="$UMBRIEL_RUNTIME_DIR/minimize-events.jsonl"

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

wait_for_field() {
  local title=$1 field=$2 expected=$3 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$title" "$field")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' field '$field' to be '$expected', got '$actual': $("$UMBRIEL" windows --json)"
  return 1
}

expect_field() {
  local title=$1 field=$2 expected=$3 actual
  actual=$(field_of "$title" "$field")
  if [[ $actual != "$expected" ]]; then
    echo "expected '$title' field '$field' to be '$expected', got '$actual': $("$UMBRIEL" windows --json)"
    exit 1
  fi
}

# Capability changes must reach both queries and the latest subscriber snapshot.
wait_for_minimizable() {
  local title=$1 expected=$2
  wait_for_field "$title" minimizable "$expected"
  for _ in $(seq 80); do
    if jq -e -s --arg title "$title" --argjson expected "$expected" '
      [.[] | select(.event == "windows")] | last | .data
      | any(.[]; .title == $title and .minimizable == $expected)
    ' "$EVENTS" > /dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected subscriber to report '$title' minimizable=$expected: $(tail -n 1 "$EVENTS")"
  exit 1
}

msg() { "$UMBRIEL" msg "$@" > /dev/null; }

foreign() {
  local reply
  reply=$("$FOREIGN_TOPLEVEL" minimize-a "$OUTPUT" "$1")
  if [[ -n ${2:-} && $reply != "$2" ]]; then
    echo "foreign-toplevel $1 reported '$reply', expected '$2'"
    exit 1
  fi
}

readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/minimize-control"
mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"

cat >> "$UMBRIEL_CONFIG" <<'EOF'
[layout]
mode = "floating"

[animation]
enabled = false
EOF
msg config-reload

MINIMIZE_ON_STDIN=1 "$CLIENT" minimize-a 480 300 <&"$control_fd" > "$A_LOG" 2>&1 &
await_lines "$A_LOG" 'mapped$' 1
"$CLIENT" minimize-b 480 300 > "$B_LOG" 2>&1 &
await_lines "$B_LOG" 'mapped$' 1
wait_for_field minimize-a floating true
wait_for_field minimize-b floating true
A=$(field_of minimize-a id)
B=$(field_of minimize-b id)
OUTPUT=$(field_of minimize-a output)
"$UMBRIEL" subscribe windows > "$EVENTS" &
wait_for_minimizable minimize-a true
wait_for_minimizable minimize-b true

# The action hides the focused window, hands focus on, and taskbars see the state.
msg "window-focus:$A"
wait_for_field minimize-a active true
msg window-minimize
wait_for_field minimize-a minimized true
expect_field minimize-a visible false
wait_for_field minimize-b active true
foreign state minimized=1
wait_for_minimizable minimize-a true

# Focus cycling skips it.
msg window-focus-next
"$UMBRIEL" settle
expect_field minimize-a minimized true
expect_field minimize-b active true

# A taskbar activation shows and focuses it.
foreign activate
wait_for_field minimize-a minimized false
expect_field minimize-a visible true
wait_for_field minimize-a active true

# The client's own minimize request.
printf z >&"$control_fd"
await_lines "$A_LOG" 'minimize-requested$' 1
wait_for_field minimize-a minimized true
wait_for_field minimize-b active true

# window-focus names it.
msg "window-focus:$A"
wait_for_field minimize-a minimized false
wait_for_field minimize-a active true

# A taskbar's unminimize shows it without taking focus.
foreign minimize minimized=1
wait_for_field minimize-b active true
foreign unminimize minimized=0
expect_field minimize-a visible true
expect_field minimize-b active true

# Activation from another workspace switches back to it.
msg "window-minimize:$A"
wait_for_field minimize-a minimized true
msg workspace-switch:2
wait_for_field minimize-b visible false
foreign activate
wait_for_field minimize-a minimized false
wait_for_field minimize-a active true
wait_for_field minimize-a visible true

# Tiling shows the window, and a tiled window cannot minimize.
msg "window-minimize:$A"
wait_for_field minimize-a minimized true
msg workspace-set-layout:scrolling
wait_for_field minimize-a floating false
wait_for_minimizable minimize-a false
expect_field minimize-a minimized false
expect_field minimize-a visible true
msg "window-focus:$A"
wait_for_field minimize-a active true
if reply=$("$UMBRIEL" msg window-minimize 2>&1); then
  echo "window-minimize unexpectedly succeeded on a tiled window"
  exit 1
fi
if [[ $reply != *"window-minimize needs a floating, unpinned window on a workspace"* ]]; then
  echo "unexpected window-minimize rejection: $reply"
  exit 1
fi
printf z >&"$control_fd"
await_lines "$A_LOG" 'minimize-requested$' 2
"$UMBRIEL" settle
expect_field minimize-a minimized false

# window-restore-minimized brings back the latest first.
msg workspace-set-layout:floating
wait_for_field minimize-a floating true
wait_for_field minimize-b floating true
wait_for_minimizable minimize-a true
wait_for_minimizable minimize-b true
msg "window-focus:$B"
msg window-minimize
wait_for_field minimize-b minimized true
msg "window-focus:$A"
msg window-minimize
wait_for_field minimize-a minimized true
msg window-restore-minimized
wait_for_field minimize-a minimized false
wait_for_field minimize-a active true
expect_field minimize-b minimized true
msg window-restore-minimized
wait_for_field minimize-b minimized false
wait_for_field minimize-b active true
msg window-restore-minimized
"$UMBRIEL" settle
expect_field minimize-a minimized false
expect_field minimize-b active true

# Show desktop minimizes the floating windows and restores them with their focus.
msg workspace-toggle-show-desktop
wait_for_field minimize-a minimized true
wait_for_field minimize-b minimized true
msg workspace-toggle-show-desktop
wait_for_field minimize-a minimized false
wait_for_field minimize-b minimized false
wait_for_field minimize-b active true

# Restoring one another way and minimizing it again leaves nothing to hide, so the toggle restores everything.
msg workspace-toggle-show-desktop
wait_for_field minimize-a minimized true
wait_for_field minimize-b minimized true
msg "window-focus:$A"
wait_for_field minimize-a minimized false
msg window-minimize
wait_for_field minimize-a minimized true
msg workspace-toggle-show-desktop
wait_for_field minimize-a minimized false
wait_for_field minimize-b minimized false
wait_for_field minimize-b active true

# A window minimized before showing the desktop comes back with the rest.
msg "window-minimize:$A"
wait_for_field minimize-a minimized true
msg workspace-toggle-show-desktop
wait_for_field minimize-b minimized true
msg workspace-toggle-show-desktop
wait_for_field minimize-a minimized false
wait_for_field minimize-b minimized false

# Pinning changes eligibility without changing the window's floating placement.
msg "window-focus:$A"
msg window-toggle-pinned
wait_for_field minimize-a pinned true
expect_field minimize-a floating true
wait_for_minimizable minimize-a false
msg window-toggle-pinned
wait_for_field minimize-a pinned false
wait_for_minimizable minimize-a true

echo "floating windows report minimize eligibility, minimize by action, client, and taskbar, return through every explicit focus path, and show desktop restores every minimized window"
