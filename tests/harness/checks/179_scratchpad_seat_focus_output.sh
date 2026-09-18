#!/usr/bin/env bash
# harness: outputs=2
# A scratchpad can own seat focus while the pointer remains on another output.
# Window actions must follow that seat focus instead of reaching the workspace
# focus retained on the pointer-selected output.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly POINTER_BACKGROUND=scratchpad-seat-pointer-background
readonly TARGET_BACKGROUND=scratchpad-seat-target-background
readonly SCRATCH=scratchpad-seat-foreground

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[animation.scratchpad]
scale = 0

[[scratchpad]]
name = "seat"

[[window_rule]]
match.title = "^scratchpad-seat-.*-background$"
default_floating = true
default_floating_size = { width = 0.5, height = 0.5 }

[[window_rule]]
match.title = "^scratchpad-seat-foreground$"
default_floating = true
default_floating_size_px = { width = 420, height = 260 }
EOF
"$UMBRIEL" msg config-reload > /dev/null

windows() { "$UMBRIEL" windows --json; }

field_of() {
  windows | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
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

window_output() {
  local workspace
  workspace=$(field_of "$1" workspace)
  echo "${workspace%%:*}"
}

# Workspace `focused` follows the pointer output, while window `active` follows
# the globally focused seat surface.
cursor_output() {
  "$UMBRIEL" workspaces --json | jq -r '.[] | select(.focused) | .output'
}

output_scale() {
  "$UMBRIEL" outputs | awk -v name="$1" '$1 == name { found = 1; next } found && /Scale:/ { print $2; exit }'
}

wait_for_output_scale() {
  local name=$1 expected=$2 actual=
  for _ in $(seq 80); do
    actual=$(output_scale "$name")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $name scale $expected, got $actual"
  "$UMBRIEL" outputs
  return 1
}

background_signature() {
  windows | jq -c --arg pointer "$POINTER_BACKGROUND" --arg target "$TARGET_BACKGROUND" '
    [.[]
      | select(.title == $pointer or .title == $target)
      | {title, workspace, active, floating, x, y, w, h}]
    | sort_by(.title)
  '
}

"$CLIENT" "$POINTER_BACKGROUND" > "$UMBRIEL_RUNTIME_DIR/$POINTER_BACKGROUND.log" 2>&1 &
wait_for_count 1
wait_for_field "$POINTER_BACKGROUND" w 640
wait_for_field "$POINTER_BACKGROUND" h 360
pointer_output=$(window_output "$POINTER_BACKGROUND")

# The headless outputs are horizontal. Move right to establish a second output
# without assuming which connector name the harness placed first.
"$UMBRIEL" msg output-focus-right > /dev/null
"$CLIENT" "$TARGET_BACKGROUND" > "$UMBRIEL_RUNTIME_DIR/$TARGET_BACKGROUND.log" 2>&1 &
wait_for_count 2
wait_for_field "$TARGET_BACKGROUND" w 640
wait_for_field "$TARGET_BACKGROUND" h 360
target_output=$(window_output "$TARGET_BACKGROUND")
if [[ -z $pointer_output || -z $target_output || $pointer_output == "$target_output" ]]; then
  echo "the fixture did not span two outputs: $(windows)"
  exit 1
fi

"$CLIENT" "$SCRATCH" > "$UMBRIEL_RUNTIME_DIR/$SCRATCH.log" 2>&1 &
wait_for_count 3
wait_for_field "$SCRATCH" w 420
wait_for_field "$SCRATCH" h 260
scratch_id=$(field_of "$SCRATCH" id)
"$UMBRIEL" msg "window-focus:$scratch_id" > /dev/null
"$UMBRIEL" msg window-move-to-scratchpad:seat > /dev/null
wait_for_field "$SCRATCH" workspace ""
"$UMBRIEL" msg scratchpad-toggle:seat > /dev/null
wait_for_field "$SCRATCH" active true

# Return the pointer to the first output, then restore seat focus to the visible
# scratchpad without warping. This is the split state that exposed pointer-based
# scratchpad action routing.
"$UMBRIEL" msg output-focus-left > /dev/null
wait_for_field "$POINTER_BACKGROUND" active true
"$UMBRIEL" msg "window-focus:$scratch_id" > /dev/null
wait_for_field "$SCRATCH" active true
if [[ $(cursor_output) != "$pointer_output" ]]; then
  echo "pointer and scratchpad seat focus did not split across outputs: $(windows)"
  exit 1
fi

# Output reconfiguration must preserve the valid seat owner instead of
# selecting the workspace under the pointer. This also proves the scale change
# happened before inspecting focus.
printf '\n[output.%s]\nscale = 1.25\n' "$target_output" >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
wait_for_output_scale "$target_output" 1.250000
wait_for_field "$SCRATCH" active true
if [[ $(field_of "$POINTER_BACKGROUND" active) != false
    || $(field_of "$TARGET_BACKGROUND" active) != false ]]; then
  echo "output scale reload moved focus from the visible scratchpad: $(windows)"
  exit 1
fi
if [[ $(cursor_output) != "$pointer_output" ]]; then
  echo "output scale reload moved the pointer away from $pointer_output"
  exit 1
fi

baseline_background=$(background_signature)
"$UMBRIEL" msg window-set-primary-extent:0.25 > /dev/null

resized=false
for _ in $(seq 80); do
  current_background=$(background_signature)
  if [[ $current_background != "$baseline_background" ]]; then
    echo "scratchpad resize followed the pointer output's workspace focus: $(windows)"
    exit 1
  fi
  if [[ $(field_of "$SCRATCH" w) == 256
      && $(field_of "$SCRATCH" h) == 260
      && $(field_of "$SCRATCH" active) == true ]]; then
    resized=true
    break
  fi
  sleep 0.1
done
if [[ $resized != true ]]; then
  echo "seat-focused scratchpad did not resize to 256x260: $(windows)"
  exit 1
fi

for _ in $(seq 5); do
  sleep 0.1
  current_background=$(background_signature)
  if [[ $current_background != "$baseline_background" ]]; then
    echo "scratchpad resize followed the pointer output's workspace focus: $(windows)"
    exit 1
  fi
done
if [[ $(cursor_output) != "$pointer_output" ]]; then
  echo "scratchpad resize moved the pointer away from $pointer_output"
  exit 1
fi

echo "scratchpad focus survives output reload and actions follow seat focus across outputs"
