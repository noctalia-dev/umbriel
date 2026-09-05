#!/usr/bin/env bash
# harness: outputs=2
# follows_focus keeps the pointer on the focused window after an in-workspace move, an output focus action, and a
# foreign-toplevel activation such as a taskbar or dock request.
set -euo pipefail

readonly BTN_LEFT=272
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly FOREIGN_TOPLEVEL="${UMBRIEL_FOREIGN_TOPLEVEL_CLIENT:-./build-debug/tests/foreign-toplevel-client}"

windows() { "$UMBRIEL" windows --json; }

wait_for_windows() {
  local want=$1
  for _ in $(seq 60); do
    [[ $(windows | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got $(windows)"
  return 1
}

wait_for_active() {
  local id=$1
  for _ in $(seq 60); do
    [[ $(windows | jq -r --arg id "$id" '.[] | select(.id == $id) | .active') == true ]] && return 0
    sleep 0.1
  done
  echo "expected $id to be active: $(windows)"
  return 1
}

wait_for_moved_x() {
  local id=$1 before=$2
  local actual=$before
  for _ in $(seq 60); do
    actual=$(window_x "$id")
    [[ $actual != "$before" ]] && return 0
    sleep 0.1
  done
  echo "expected $id to move from x=$before: $(windows)"
  return 1
}

window_id() { windows | jq -r --arg title "$1" '.[] | select(.title == $title) | .id'; }
window_x() { windows | jq -r --arg id "$1" '.[] | select(.id == $id) | .x'; }
window_center_x() { windows | jq -r --arg id "$1" '.[] | select(.id == $id) | (.x + .w / 2 | floor)'; }
window_center_y() { windows | jq -r --arg id "$1" '.[] | select(.id == $id) | (.y + .h / 2 | floor)'; }
window_output() {
  local workspace
  workspace=$(windows | jq -r --arg id "$1" '.[] | select(.id == $id) | .workspace')
  echo "${workspace%%:*}"
}
output_x() {
  "$UMBRIEL" outputs | awk -v name="$1" '$1 == name {found = 1; next} found && /Position:/ {split($2, p, ","); print p[1]; exit}'
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
animation_ms = 0

[layout.scrolling]
default_width_fraction = 0.5

[input.cursor]
follows_focus = true

[[window_rule]]
match.title = "^follow-first$"
default_output = "HEADLESS-1"

[[window_rule]]
match.title = "^follow-second$"
default_output = "HEADLESS-1"

[[window_rule]]
match.title = "^follow-other-output$"
default_output = "HEADLESS-2"

[[window_rule]]
match.title = "^follow-other-neighbor$"
default_output = "HEADLESS-2"

[[window_rule]]
match.title = "^follow-other-third$"
default_output = "HEADLESS-2"
EOF
"$UMBRIEL" msg config-reload > /dev/null

foot --title=follow-first sh -c 'sleep 120' > "$UMBRIEL_RUNTIME_DIR/follow-first.log" 2>&1 &
foot --title=follow-second sh -c 'sleep 120' > "$UMBRIEL_RUNTIME_DIR/follow-second.log" 2>&1 &
foot --title=follow-other-output sh -c 'sleep 120' > "$UMBRIEL_RUNTIME_DIR/follow-other-output.log" 2>&1 &
foot --title=follow-other-neighbor sh -c 'sleep 120' > "$UMBRIEL_RUNTIME_DIR/follow-other-neighbor.log" 2>&1 &
foot --title=follow-other-third sh -c 'sleep 120' > "$UMBRIEL_RUNTIME_DIR/follow-other-third.log" 2>&1 &
wait_for_windows 5

first_id=$(window_id follow-first)
second_id=$(window_id follow-second)
other_id=$(windows | jq -r '[.[] | select(.workspace | startswith("HEADLESS-2:"))] | sort_by(.x) | .[0].id')
first_output=$(window_output "$first_id")
other_output=$(window_output "$other_id")
if [[ $first_output == "$other_output" ]]; then
  echo "window rules did not split the fixtures across outputs: $(windows)"
  exit 1
fi

# Put the pointer on the focused window, then swap that window with its neighbor. A focus-only detour and an unmoved
# click must return focus to the moved window at its new pointer-followed position.
"$UMBRIEL" msg "window-focus:$first_id" > /dev/null
"$POINTER" 2560 720 move "$(window_center_x "$first_id")" "$(window_center_y "$first_id")"
before_x=$(window_x "$first_id")
if ((before_x < $(window_x "$second_id"))); then
  move_action=window-move-or-output-right
else
  move_action=window-move-or-output-left
fi
"$UMBRIEL" msg "$move_action" > /dev/null
wait_for_moved_x "$first_id" "$before_x"
after_x=$(window_x "$first_id")
if [[ $after_x == "$before_x" ]]; then
  echo "$move_action did not move follow-first: $(windows)"
  exit 1
fi
"$UMBRIEL" msg "window-focus:$second_id" > /dev/null
"$POINTER" 2560 720 click "$BTN_LEFT"
wait_for_active "$first_id"

# Output focus must use the target window when follows_focus is enabled. The output center itself is outside the
# focused half-width fixture, so a focus-only detour followed by a click distinguishes the two landing points.
first_output_x=$(output_x "$first_output")
other_output_x=$(output_x "$other_output")
if ((other_output_x > first_output_x)); then
  output_action=output-focus-right
else
  output_action=output-focus-left
fi
"$UMBRIEL" msg "window-focus:$other_id" > /dev/null
"$UMBRIEL" msg "window-focus:$first_id" > /dev/null
"$UMBRIEL" msg "$output_action" > /dev/null
wait_for_active "$other_id"
"$UMBRIEL" msg "window-focus:$first_id" > /dev/null
"$POINTER" 2560 720 click "$BTN_LEFT"
wait_for_active "$other_id"

# A dock uses foreign-toplevel activation rather than Umbriel's IPC. It must receive the same configured pointer
# following behavior while the explicit window-focus action remains focus-only.
"$FOREIGN_TOPLEVEL" follow-first "$first_output" activate
wait_for_active "$first_id"
"$UMBRIEL" msg "window-focus:$other_id" > /dev/null
"$POINTER" 2560 720 click "$BTN_LEFT"
wait_for_active "$first_id"

echo "follows_focus tracks local moves, output focus, and foreign-toplevel activation"
