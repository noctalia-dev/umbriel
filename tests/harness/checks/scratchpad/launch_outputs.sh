#!/usr/bin/env bash
# harness: outputs=2
# Rule-free spawn_when_empty and ordinary launches join the selected pad. A delayed launch retains its pad when
# another pad replaces it on a scaled output; explicit placement rules override that destination.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly FIFO="$UMBRIEL_RUNTIME_DIR/pad-launch-control"
readonly LOG="$UMBRIEL_RUNTIME_DIR/pad-launch-delayed.log"

cat >> "$UMBRIEL_CONFIG" <<EOF

[animation]
enabled = false

[input.focus]
follows_mouse = false

[output.HEADLESS-1]
mode = "1280x720"
position = [0, 0]
workspaces = ["LEFT"]

[output.HEADLESS-2]
mode = "1280x720"
scale = 2.0
position = [1280, 0]
workspaces = ["RIGHT"]

[[scratchpad]]
name = "auto"
spawn_when_empty = "exec '$CLIENT' pad-auto 200 150 > '$UMBRIEL_RUNTIME_DIR/pad-auto.log' 2>&1"

[[scratchpad]]
name = "manual"

[[window_rule]]
match.title = "^pad-rule-output$"
default_output = "HEADLESS-1"
default_focused = false

[[window_rule]]
match.title = "^pad-rule-workspace$"
default_workspace = "LEFT"
default_output = "HEADLESS-1"
default_focused = false

[[window_rule]]
match.title = "^pad-rule-scratch$"
default_scratchpad = "auto"
default_focused = false

[[window_rule]]
match.title = "^pad-rule-pinned$"
default_pinned = true
default_focused = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

field() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg key "$2" '[.[] | select(.title == $title)] | if length == 0 then "<absent>" else .[0][$key] end'
}

wait_field() {
  local actual=
  for _ in $(seq 80); do
    actual=$(field "$1" "$2")
    [[ $actual == "$3" ]] && return 0
    sleep 0.1
  done
  echo "expected $1 $2=$3, got $actual: $("$UMBRIEL" windows --json)"
  return 1
}

spawn() {
  "$UMBRIEL" msg spawn "exec '$CLIENT' '$1' 200 150 > '$UMBRIEL_RUNTIME_DIR/$1.log' 2>&1" > /dev/null
}

"$POINTER" 1920 720 move 400 300
"$UMBRIEL" msg scratchpad-toggle auto > /dev/null
wait_field pad-auto scratchpad auto
wait_field pad-auto active true
"$POINTER" 1920 720 move 1600 200
"$UMBRIEL" msg scratchpad-toggle manual > /dev/null
spawn pad-manual
wait_field pad-manual scratchpad manual
wait_field pad-manual active true
x=$(field pad-manual x)
if ((x < 1280 || x >= 1920)); then
  echo "scratchpad launch ignored scaled output's logical bounds: $("$UMBRIEL" windows --json)"
  exit 1
fi

mkfifo "$FIFO"
exec {control_fd}<>"$FIFO"
"$UMBRIEL" msg \
  spawn "exec env MAP_ON_STDIN=1 ACTIVATE_ON_START=1 '$CLIENT' pad-delayed 200 150 < '$FIFO' > '$LOG' 2>&1" > /dev/null
for _ in $(seq 80); do
  grep -q '^map-pending$' "$LOG" 2>/dev/null && break
  sleep 0.1
done
grep -q '^map-pending$' "$LOG"
"$UMBRIEL" msg scratchpad-toggle auto > /dev/null
wait_field pad-auto active true
wait_field pad-manual active false
printf m >&"$control_fd"
wait_field pad-delayed scratchpad manual
wait_field pad-delayed active false
wait_field pad-auto active true

spawn pad-rule-output
wait_field pad-rule-output scratchpad ""
wait_field pad-rule-output output HEADLESS-1
spawn pad-rule-workspace
wait_field pad-rule-workspace scratchpad ""
left_workspace=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .active) | .id')
wait_field pad-rule-workspace workspace "$left_workspace"
spawn pad-rule-scratch
wait_field pad-rule-scratch scratchpad auto
spawn pad-rule-pinned
wait_field pad-rule-pinned scratchpad ""
wait_field pad-rule-pinned pinned true
wait_field pad-auto active true

# A pad removed before an already-created toplevel maps cannot leave dangling placement state.
"$UMBRIEL" msg scratchpad-toggle manual > /dev/null
"$UMBRIEL" msg \
  spawn "exec env MAP_ON_STDIN=1 '$CLIENT' pad-removed 200 150 < '$FIFO' > '$UMBRIEL_RUNTIME_DIR/pad-removed.log' 2>&1" > /dev/null
for _ in $(seq 80); do
  grep -q '^map-pending$' "$UMBRIEL_RUNTIME_DIR/pad-removed.log" 2>/dev/null && break
  sleep 0.1
done
grep -q '^map-pending$' "$UMBRIEL_RUNTIME_DIR/pad-removed.log"
# Remove the final, two-line manual definition without changing the auto definition or its rules.
sed -i '/\[\[scratchpad\]\]/{N; /name = "manual"/d; P; D;}' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
printf m >&"$control_fd"
wait_field pad-removed scratchpad ""

echo "rule-free launch, scaled output selection, delayed hidden admission, and rule precedence verified"
