#!/usr/bin/env bash
# An empty scratchpad has visible presentation and launch context. Closing its last member preserves both;
# delayed launches retain their destination, and explicit workspace focus relinquishes launch context.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly FIFO="$UMBRIEL_RUNTIME_DIR/empty-launch-control"
readonly LOG="$UMBRIEL_RUNTIME_DIR/empty-launch-delayed.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[animation.scratchpad]
dim = 0.5
blur = false

[colors]
backdrop = "#FFFFFFFF"

[input.focus]
follows_mouse = false
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

assert_backdrop() {
  local red= image="$UMBRIEL_RUNTIME_DIR/empty-launch.png"
  "$UMBRIEL" settle > /dev/null
  grim "$image"
  red=$(magick "$image" -crop 10x10+5+5 -format '%[fx:round(255*mean.r)]' info:)
  if ((red < $1 || red > $2)); then
    echo "expected backdrop in [$1,$2], got $red"
    return 1
  fi
}

spawn() {
  "$UMBRIEL" msg spawn "exec '$CLIENT' '$1' 300 200 > '$UMBRIEL_RUNTIME_DIR/$1.log' 2>&1" > /dev/null
}

assert_backdrop 250 255
"$CLIENT" empty-home 300 200 > "$UMBRIEL_RUNTIME_DIR/empty-home.log" 2>&1 &
wait_field empty-home active true
"$UMBRIEL" msg scratchpad-toggle > /dev/null
assert_backdrop 120 135
"$UMBRIEL" msg scratchpad-toggle > /dev/null
assert_backdrop 250 255
"$UMBRIEL" msg scratchpad-toggle > /dev/null
assert_backdrop 120 135

# Window-relative actions have no target while the pad is empty. The retained workspace focus must stay untouched.
home_workspace=$(field empty-home workspace)
for action in window-toggle-floating column-focus-first "window-move-to-workspace-silent 2" \
  window-move-to-scratchpad window-toggle-scratchpad; do
  "$UMBRIEL" msg $action > /dev/null
  if [[ $(field empty-home floating) != false || $(field empty-home active) != false \
      || $(field empty-home workspace) != "$home_workspace" || $(field empty-home scratchpad) != "" ]]; then
    echo "empty scratchpad let $action reach the workspace window: $("$UMBRIEL" windows --json)"
    exit 1
  fi
done
"$UMBRIEL" msg window-close > /dev/null
spawn empty-first
wait_field empty-first scratchpad default
wait_field empty-first active true
wait_field empty-home workspace "$home_workspace"
first_id=$(field empty-first id)
"$UMBRIEL" msg window-close "$first_id" > /dev/null
for _ in $(seq 80); do
  grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/empty-first.log" && break
  sleep 0.1
done
grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/empty-first.log"
assert_backdrop 120 135
wait_field empty-home active false
spawn empty-second
wait_field empty-second scratchpad default
wait_field empty-second active true

# A real client disconnect must preserve the empty state too, not just an unmap-only close request.
second_pid=$(field empty-second pid)
kill -TERM "$second_pid"
wait_field empty-second title "<absent>"
assert_backdrop 120 135
spawn empty-second
wait_field empty-second scratchpad default
wait_field empty-second active true

# The launch is captured before the toplevel's initial commit, then its pad is hidden before map.
mkfifo "$FIFO"
exec {control_fd}<>"$FIFO"
"$UMBRIEL" msg \
  spawn "exec env MAP_ON_STDIN=1 '$CLIENT' empty-delayed 300 200 < '$FIFO' > '$LOG' 2>&1" > /dev/null
for _ in $(seq 80); do
  grep -q '^map-pending$' "$LOG" 2>/dev/null && break
  sleep 0.1
done
grep -q '^map-pending$' "$LOG"
"$UMBRIEL" msg scratchpad-toggle > /dev/null
assert_backdrop 250 255
printf m >&"$control_fd"
wait_field empty-delayed scratchpad default
wait_field empty-delayed active false
assert_backdrop 250 255

# An unrelated client is not captured merely because the pad is visible. Focusing it changes subsequent launches.
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$CLIENT" empty-workspace 300 200 > "$UMBRIEL_RUNTIME_DIR/empty-workspace.log" 2>&1 &
wait_field empty-workspace active true
wait_field empty-workspace scratchpad ""
spawn empty-workspace-child
wait_field empty-workspace-child scratchpad ""
wait_field empty-workspace-child active true

# An empty-background click relinquishes scratchpad launch context even while its backdrop stays visible.
scratch_id=$(field empty-second id)
"$UMBRIEL" msg window-focus "$scratch_id" > /dev/null
wait_field empty-second active true
"$POINTER" 1280 720 move 1200 650 click 272
wait_field empty-workspace-child active true
spawn empty-after-click
wait_field empty-after-click scratchpad ""
wait_field empty-after-click active true

echo "empty toggles, last-member closure, delayed admission, and workspace launch context verified"
