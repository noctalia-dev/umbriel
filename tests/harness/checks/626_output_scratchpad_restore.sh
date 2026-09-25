#!/usr/bin/env bash
# harness: outputs=2
# A global scratchpad is parked on an output rather than on a workspace, and it has to find its way back to that output
# after every monitor goes away and returns.
set -euo pipefail

readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly LAYER_CLIENT="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly OUTPUT_LAYOUT_W=2560
readonly OUTPUT_LAYOUT_H=720
readonly SCRATCHPAD=restore
readonly PANEL_HEIGHT=96
readonly SCRATCHPAD_CONFIG='
[animation]
enabled = false

[animation.scratchpad]
scale = 0

[[scratchpad]]
name = "restore"'

BASELINE=$(< "$UMBRIEL_CONFIG")

write_config() {
  {
    printf '%s\n' "$BASELINE"
    printf '%s\n' "$SCRATCHPAD_CONFIG"
    if [[ -n $1 ]]; then
      printf '\n%s\n' "$1"
    fi
  } > "$UMBRIEL_CONFIG"
}

log_mark() { wc -l < "$UMBRIEL_LOG"; }

wait_for_log_since() {
  local mark=$1 pattern=$2
  for _ in $(seq 40); do
    if tail -n +"$((mark + 1))" "$UMBRIEL_LOG" | grep -q "$pattern"; then
      return 0
    fi
    sleep 0.25
  done
  return 1
}

expect_log_since() {
  local mark=$1 pattern=$2 message=$3
  if wait_for_log_since "$mark" "$pattern"; then
    return 0
  fi
  echo "$message"
  tail -8 "$UMBRIEL_LOG" | sed 's/^/  | /'
  exit 1
}

reload_with() {
  local mark=
  mark=$(log_mark)
  write_config "$1"
  "$UMBRIEL" msg config-reload > /dev/null
  shift
  for expected in "$@"; do
    expect_log_since "$mark" "${expected#*=}" "output ${expected%%=*} did not ${expected#*=} on reload"
  done
}

wait_for_count() {
  local expected=$1 count=
  for _ in $(seq 40); do
    count=$("$UMBRIEL" windows --json | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.25
  done
  echo "expected $expected window(s), got $count"
  return 1
}

field_of() {
  "$UMBRIEL" windows --json \
    | jq -r --arg field "$1" '.[] | select(.title == "scratch-restore") | .[$field]'
}

wait_for_field() {
  local field=$1 expected=$2 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$field")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected scratch-restore field '$field' to be '$expected', got '$actual': $("$UMBRIEL" windows --json)"
  return 1
}

start_panel() {
  local log=$1
  "$LAYER_CLIENT" HEADLESS-2 "$PANEL_HEIGHT" > "$log" 2>&1 &
  PANEL_PID=$!
  for _ in $(seq 80); do
    grep -q '^ready$' "$log" && return 0
    if ! kill -0 "$PANEL_PID" 2>/dev/null; then
      echo "exclusive-zone panel exited before mapping: $(< "$log")"
      return 1
    fi
    sleep 0.1
  done
  echo "exclusive-zone panel did not map: $(< "$log")"
  return 1
}

output_origin() {
  "$UMBRIEL" outputs \
    | awk -v name="$1" '$1 == name { found = 1; next } found && /Position:/ { split($2, p, ","); print p[1], p[2]; exit }'
}

move_pointer_to_output() {
  local output=$1 output_x output_y
  read -r output_x output_y < <(output_origin "$output")
  if [[ -z ${output_x:-} || -z ${output_y:-} ]]; then
    echo "could not resolve origin for $output" >&2
    return 1
  fi
  "$POINTER" "$OUTPUT_LAYOUT_W" "$OUTPUT_LAYOUT_H" move "$((output_x + 640))" "$((output_y + 360))"
}

scratchpad_offset() {
  local output=$1 window_x window_y output_x output_y
  read -r window_x window_y < <(
    "$UMBRIEL" windows --json \
      | jq -r '.[] | select(.title == "scratch-restore") | "\(.x) \(.y)"'
  )
  read -r output_x output_y < <(output_origin "$output")
  printf '{"x":%d,"y":%d}' "$((window_x - output_x))" "$((window_y - output_y))"
}

# Summon the global scratchpad to an output and verify that it focuses its window. Close it again afterward.
shows_scratchpad() {
  local output=$1 active=
  move_pointer_to_output "$output"
  "$UMBRIEL" msg "scratchpad-toggle:$SCRATCHPAD" > /dev/null
  for _ in $(seq 10); do
    active=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "scratch-restore") | .active')
    [[ $active == true ]] && break
    sleep 0.1
  done
  "$UMBRIEL" msg "scratchpad-toggle:$SCRATCHPAD" > /dev/null
  [[ $active == true ]]
}

write_config '[[window_rule]]
match.title = "^scratch-restore$"
default_output = "HEADLESS-2"
default_floating = true
default_floating_size_px = { width = 420, height = 260 }
default_position = { x = 91, y = 73, anchor = "top_left" }'
"$UMBRIEL" msg config-reload > /dev/null

foot --title=scratch-restore sh -c 'sleep 120' > /dev/null 2>&1 &
wait_for_count 1
move_pointer_to_output HEADLESS-2
"$UMBRIEL" msg "window-move-to-scratchpad:$SCRATCHPAD" > /dev/null
if ! shows_scratchpad HEADLESS-2; then
  echo "scratchpad window could not be shown on HEADLESS-2"
  exit 1
fi
before=$(scratchpad_offset HEADLESS-2)
expected='{"x":91,"y":73}'
if [[ $before != "$expected" ]]; then
  echo "scratchpad did not start at the configured off-center position: expected=$expected got=$before"
  exit 1
fi

# Every monitor goes away at once, the way a suspend takes them.
reload_with \
  '[output.HEADLESS-1]
enabled = false
[output.HEADLESS-2]
enabled = false' \
  "HEADLESS-1=output 'HEADLESS-1': disabled by config" \
  "HEADLESS-2=output 'HEADLESS-2': disabled by config"

# HEADLESS-1 comes back first and takes the window in, though it is not where it belongs.
reload_with '[output.HEADLESS-2]
enabled = false' "HEADLESS-1=output 'HEADLESS-1': applied mode="
if ! shows_scratchpad HEADLESS-1; then
  echo "scratchpad window was not rescued onto the one output that came back"
  exit 1
fi

# HEADLESS-2 comes back and takes its window back.
reload_with '' "HEADLESS-2=output 'HEADLESS-2': applied mode="
after=$(scratchpad_offset HEADLESS-2)
if [[ $after != "$before" ]]; then
  echo "scratchpad output-relative position changed across restoration: before=$before after=$after"
  exit 1
fi
if ! shows_scratchpad HEADLESS-2; then
  echo "scratchpad window did not go back to HEADLESS-2"
  exit 1
fi

# A visible maximized pad has manager-owned geometry. Restore it after its
# output returns at a different mode and before the old exclusive zone returns.
panel_log="$UMBRIEL_RUNTIME_DIR/scratchpad-restore-panel.log"
start_panel "$panel_log"
move_pointer_to_output HEADLESS-2
"$UMBRIEL" msg "scratchpad-toggle:$SCRATCHPAD" > /dev/null
wait_for_field active true
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
wait_for_field x 1280
wait_for_field y "$PANEL_HEIGHT"
wait_for_field w 1280
wait_for_field h "$((720 - PANEL_HEIGHT))"

"$UMBRIEL" output-destroy HEADLESS-2 > /dev/null
if ! wait "$PANEL_PID"; then
  echo "exclusive-zone panel failed while HEADLESS-2 closed: $(< "$panel_log")"
  exit 1
fi
wait_for_field x 0
wait_for_field y 0
wait_for_field w 1280
wait_for_field h 720

write_config '[output.HEADLESS-2]
mode = "960x540"
position = [1280, 0]'
"$UMBRIEL" msg config-reload > /dev/null
created=$("$UMBRIEL" output-create HEADLESS-2)
if [[ $created != HEADLESS-2 ]]; then
  echo "expected recreated output HEADLESS-2, got '$created'"
  exit 1
fi
wait_for_field x 1280
wait_for_field y 0
wait_for_field w 960
wait_for_field h 540

"$UMBRIEL" msg "scratchpad-toggle:$SCRATCHPAD" > /dev/null
if ! shows_scratchpad HEADLESS-2; then
  echo "maximized scratchpad could not be toggled after returning to changed geometry"
  exit 1
fi

echo "a scratchpad returned from output loss with its floating and maximized geometry intact"
