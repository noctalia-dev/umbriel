#!/usr/bin/env bash
# harness: outputs=2
# A focused scratchpad is the target for direct window state actions and blocks
# layout-relative actions from reaching the workspace focus retained behind it.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly CLOSE_CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly BTN_LEFT=272
readonly PREFIX=scratchpad-actions
readonly LEFT="${PREFIX}-bg-left"
readonly TOP="${PREFIX}-bg-top"
readonly BOTTOM="${PREFIX}-bg-bottom"
readonly SCRATCH="${PREFIX}-foreground"
readonly CLOSE_SCRATCH="${PREFIX}-close"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[animation.scratchpad]
scale = 0

[output.HEADLESS-1]
mode = "1280x720"
position = [0, 0]
workspaces = ["ONE", "TWO"]

[output.HEADLESS-2]
mode = "1600x900"
position = [1280, 0]
workspaces = ["RIGHT"]

[[scratchpad]]
name = "actions"

[[window_rule]]
match.title = "^scratchpad-actions-foreground$"
default_output = "HEADLESS-1"
default_floating = true
default_floating_size_px = { width = 420, height = 260 }
default_position = { x = 430, y = 230, anchor = "top_left" }

[[window_rule]]
match.title = "^scratchpad-actions-close$"
default_output = "HEADLESS-1"
default_floating = true
default_floating_size_px = { width = 420, height = 260 }
EOF
"$UMBRIEL" msg config-reload > /dev/null

windows() { "$UMBRIEL" windows --json; }

field_of() {
  windows | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

accepts() {
  local action=$1 out
  if ! out=$("$UMBRIEL" msg "$action" 2>&1); then
    echo "expected '$action' to be accepted, got: $out"
    return 1
  fi
}

wait_for_count() {
  local expected=$1 count=
  for _ in $(seq 80); do
    count=$(windows | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected windows, got $count: $(windows)"
  return 1
}

wait_for_field() {
  local title=$1 field=$2 expected=$3 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$title" "$field")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' field '$field' to be '$expected', got '$actual': $(windows)"
  return 1
}

wait_for_stack() {
  local state=
  for _ in $(seq 80); do
    state=$(windows)
    if jq -e --arg top "$TOP" --arg bottom "$BOTTOM" '
      first(.[] | select(.title == $top)) as $top
      | first(.[] | select(.title == $bottom)) as $bottom
      | $top.x == $bottom.x and $top.y < $bottom.y
    ' <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "background stack did not settle: $state"
  return 1
}

active_workspace() {
  "$UMBRIEL" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .active) | .name'
}

wait_for_workspace() {
  local expected=$1 actual=
  for _ in $(seq 80); do
    actual=$(active_workspace)
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected workspace '$expected' to be active, got '$actual'"
  return 1
}

background_signature() {
  windows | jq -c --arg prefix "${PREFIX}-bg-" '
    [.[]
      | select(.title | startswith($prefix))
      | {title, workspace, active, focused, floating, x, y, w, h}]
    | sort_by(.title)
  '
}

background_geometry() {
  windows | jq -c --arg prefix "${PREFIX}-bg-" '
    [.[]
      | select(.title | startswith($prefix))
      | {title, workspace, floating, x, y, w, h}]
    | sort_by(.title)
  '
}

scratch_signature() {
  windows | jq -c --arg title "$SCRATCH" '
    first(.[] | select(.title == $title))
    | {workspace, active, floating, x, y, w, h}
  '
}

assert_isolated() {
  local action=$1 current_background current_scratch
  accepts "$action"
  for _ in $(seq 10); do
    sleep 0.1
    current_background=$(background_signature)
    current_scratch=$(scratch_signature)
    if [[ $current_background != "$baseline_background" || $current_scratch != "$baseline_scratch" ]]; then
      echo "'$action' escaped the focused scratchpad: $(windows)"
      return 1
    fi
  done
}

rejects_without_focus_escape() {
  local action=$1 expected=$2 out current_background current_scratch
  if out=$("$UMBRIEL" msg "$action" 2>&1); then
    echo "expected '$action' to reject with '$expected', but it succeeded"
    return 1
  fi
  if [[ $out != *"$expected"* ]]; then
    echo "expected '$action' to mention '$expected', got: $out"
    return 1
  fi
  current_background=$(background_signature)
  current_scratch=$(scratch_signature)
  if [[ $current_background != "$baseline_background" || $current_scratch != "$baseline_scratch" ]]; then
    echo "'$action' touched the background before its fallback rejected: $(windows)"
    return 1
  fi
}

wait_for_scratch_box() {
  local expected_x=$1 expected_y=$2 expected_width=$3 expected_height=$4 state=
  for _ in $(seq 80); do
    state=$(windows)
    if jq -e \
      --arg title "$SCRATCH" \
      --argjson x "$expected_x" \
      --argjson y "$expected_y" \
      --argjson width "$expected_width" \
      --argjson height "$expected_height" '
        any(.[];
          .title == $title
          and .workspace == ""
          and .active
          and .floating
          and .x == $x
          and .y == $y
          and .w == $width
          and .h == $height)
      ' <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "scratchpad did not reach ${expected_width}x${expected_height} at ${expected_x},${expected_y}: $state"
  return 1
}

wait_for_scratch_restore() {
  local state=
  for _ in $(seq 80); do
    state=$(scratch_signature)
    [[ $state == "$baseline_scratch" ]] && return 0
    sleep 0.1
  done
  echo "scratchpad did not restore its original box: $(windows)"
  return 1
}

wait_for_straddled_scratch() {
  local state=
  for _ in $(seq 80); do
    state=$(windows)
    if jq -e --arg title "$SCRATCH" '
      any(.[];
        .title == $title
        and .active
        and .workspace == ""
        and .x == 1100
        and .y == 230
        and .w == 420
        and .h == 260)
    ' <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "scratchpad did not stay straddled after assignment to HEADLESS-2: $state"
  return 1
}

assert_direct_toggle() {
  local action=$1
  accepts "$action"
  wait_for_scratch_box 1280 0 1600 900
  if [[ $(background_signature) != "$baseline_background" ]]; then
    echo "'$action' changed the background instead of only the scratchpad: $(windows)"
    return 1
  fi
  accepts "$action"
  wait_for_scratch_restore
  if [[ $(background_signature) != "$baseline_background" ]]; then
    echo "restoring '$action' changed the background: $(windows)"
    return 1
  fi
}

accepts workspace-switch:ONE/HEADLESS-1

background_count=0
for title in "$LEFT" "$TOP" "$BOTTOM"; do
  "$CLIENT" "$title" > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  background_count=$((background_count + 1))
  wait_for_count "$background_count"
done

# Form a two-row right column. Keeping its top row as the remembered workspace
# focus makes every representative stale-focus action below observable.
accepts "window-focus:$(field_of "$BOTTOM" id)"
accepts window-consume-left
wait_for_stack
accepts "window-focus:$(field_of "$TOP" id)"

"$CLIENT" "$SCRATCH" > "$UMBRIEL_RUNTIME_DIR/$SCRATCH.log" 2>&1 &
scratch_pid=$!
wait_for_count 4
wait_for_field "$SCRATCH" w 420
wait_for_field "$SCRATCH" h 260
wait_for_field "$SCRATCH" x 430
wait_for_field "$SCRATCH" y 230
scratch_id=$(field_of "$SCRATCH" id)
"$POINTER" 2880 900 move 640 360
accepts "window-focus:$scratch_id"
accepts window-move-to-scratchpad:actions
wait_for_field "$SCRATCH" workspace ""
accepts scratchpad-toggle:actions
# Drop the straddling window with its pointer on HEADLESS-2. Scratchpad drags
# roam the named pad without restoring it to the workspace underneath.
"$POINTER" 2880 900 \
  move 640 360 mod logo press "$BTN_LEFT" move 800 360 move 1310 360 release "$BTN_LEFT" mod none
wait_for_straddled_scratch
# Recreate the deliberate split between pointer output and scratchpad seat
# focus that the action-isolation assertions exercise.
"$POINTER" 2880 900 move 640 360
accepts "window-focus:$scratch_id"
if [[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.focused) | .output') != HEADLESS-1 ]]; then
  echo "pointer did not remain on HEADLESS-1 while the HEADLESS-2 scratchpad held seat focus"
  exit 1
fi

baseline_background=$(background_signature)
baseline_geometry=$(background_geometry)
baseline_scratch=$(scratch_signature)

# A composite output action has no layout neighbor from a scratchpad. From the
# pointer-selected left output it must reach, and reject from, its output fallback.
rejects_without_focus_escape window-focus-or-output-left "no output"

# Focus, column order, row order, workspace transfer, and membership are
# separate handler families. None may use the workspace focus behind the
# scratchpad. The floating toggle is an explicitly inert scratchpad action.
assert_isolated window-focus-left
assert_isolated column-move-left
assert_isolated window-move-down
assert_isolated window-swap-next
assert_isolated window-move-to-workspace-next
assert_isolated column-move-to-workspace-next
assert_isolated window-toggle-floating

# window-move-to-scratchpad and the move branch of window-toggle-scratchpad are
# intentionally excluded. Their documented source is the pointer-selected
# active workspace, even while another output's scratchpad owns seat focus.

# A composite workspace action still keeps its promised fallback. The stacked
# background has a downward neighbor, so selecting TWO proves that it did not
# navigate that stale stack first.
accepts window-focus-or-workspace-down
wait_for_workspace TWO
if [[ $(background_geometry) != "$baseline_geometry" ]]; then
  echo "workspace fallback changed the background layout: $(windows)"
  exit 1
fi
accepts workspace-switch:ONE/HEADLESS-1
accepts "window-focus:$(field_of "$TOP" id)"
accepts "window-focus:$scratch_id"
wait_for_field "$SCRATCH" active true
baseline_background=$(background_signature)
baseline_scratch=$(scratch_signature)

# Direct state actions use the scratchpad's assigned HEADLESS-2 output, not the
# output containing its top-left corner. They must restore the exact straddled
# box afterward without changing the background.
assert_direct_toggle window-toggle-maximize
assert_direct_toggle window-toggle-maximize-to-edges
assert_direct_toggle window-toggle-fullscreen

# Switching from maximize-to-edges to ordinary maximize must keep a coherent
# maximized state. The next ordinary toggle then restores the original box.
accepts window-toggle-maximize-to-edges
wait_for_scratch_box 1280 0 1600 900
accepts window-toggle-maximize
wait_for_scratch_box 1280 0 1600 900
accepts window-toggle-maximize
wait_for_scratch_restore
if [[ $(background_signature) != "$baseline_background" ]]; then
  echo "switching scratchpad maximize modes changed the background: $(windows)"
  exit 1
fi

# Bare close follows actual seat focus. It must close the scratchpad, not the
# remembered workspace window. The fractional client intentionally ignores
# close, so replace it with the harness client that unmaps on close.
kill "$scratch_pid"
wait_for_count 3
"$CLOSE_CLIENT" "$CLOSE_SCRATCH" 420 260 > "$UMBRIEL_RUNTIME_DIR/$CLOSE_SCRATCH.log" 2>&1 &
wait_for_count 4
close_scratch_id=$(field_of "$CLOSE_SCRATCH" id)
accepts "window-focus:$close_scratch_id"
accepts window-move-to-scratchpad:actions
wait_for_field "$CLOSE_SCRATCH" workspace ""
accepts scratchpad-toggle:actions
wait_for_field "$CLOSE_SCRATCH" active true
accepts window-close
wait_for_count 3
if ! grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/$CLOSE_SCRATCH.log"; then
  echo "window-close did not reach the seat-focused scratchpad: $(< "$UMBRIEL_RUNTIME_DIR/$CLOSE_SCRATCH.log")"
  exit 1
fi
if [[ $(background_geometry) != "$baseline_geometry" ]]; then
  echo "window-close removed or changed a background window: $(windows)"
  exit 1
fi

echo "scratchpad state actions target the seat focus and layout actions leave the background isolated"
