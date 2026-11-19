#!/usr/bin/env bash
# harness: outputs=2
# An explicit output-focus action, and pointer-driven output crossing under
# follows_mouse, both target an output that already has a visible-but-unfocused
# scratchpad; both must refocus that scratchpad, not dismiss it and fall
# through to the workspace window beneath it. Contrast
# scratchpad_background_click: that action is a deliberate steal away from a
# scratchpad, not a switch of which output is focused, and keeps dismissing it.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly PLAIN_A=scratchpad-output-focus-plain-a
readonly PLAIN_B=scratchpad-output-focus-plain-b
readonly SCRATCH=scratchpad-output-focus-foreground

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[[scratchpad]]
name = "test"

[[window_rule]]
match.title = "^scratchpad-output-focus-plain-.$"
default_floating = true
default_floating_size = { width = 0.5, height = 0.5 }

[[window_rule]]
match.title = "^scratchpad-output-focus-foreground$"
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

"$CLIENT" "$PLAIN_A" > "$UMBRIEL_RUNTIME_DIR/$PLAIN_A.log" 2>&1 &
wait_for_count 1
wait_for_field "$PLAIN_A" active true
output_a=$(field_of "$PLAIN_A" output)

# The headless outputs are horizontal; move right to reach the second output
# without assuming connector names.
"$UMBRIEL" msg output-focus-right > /dev/null
"$CLIENT" "$PLAIN_B" > "$UMBRIEL_RUNTIME_DIR/$PLAIN_B.log" 2>&1 &
wait_for_count 2
wait_for_field "$PLAIN_B" active true
output_b=$(field_of "$PLAIN_B" output)
if [[ -z $output_a || -z $output_b || $output_a == "$output_b" ]]; then
  echo "the fixture did not span two outputs: $(windows)"
  exit 1
fi

"$CLIENT" "$SCRATCH" > "$UMBRIEL_RUNTIME_DIR/$SCRATCH.log" 2>&1 &
wait_for_count 3
scratch_id=$(field_of "$SCRATCH" id)
"$UMBRIEL" msg window-focus "$scratch_id" > /dev/null
"$UMBRIEL" msg window-move-to-scratchpad test > /dev/null
wait_for_field "$SCRATCH" workspace ""
# Output B is still current here, so the scratchpad shows there.
"$UMBRIEL" msg scratchpad-toggle test > /dev/null
wait_for_field "$SCRATCH" active true

# Move explicit focus away to output A. Leaving a scratchpad's output must
# unfocus it, not dismiss it outright.
"$UMBRIEL" msg output-focus-left > /dev/null
wait_for_field "$PLAIN_A" active true
wait_for_field "$SCRATCH" active false
if [[ $(field_of "$SCRATCH" visible) != true ]]; then
  echo "leaving the scratchpad's output dismissed it instead of just unfocusing it: $(windows)"
  exit 1
fi

# The actual repro: explicit output-focus back onto the output holding the
# visible-but-unfocused scratchpad must refocus it, not the workspace window.
"$UMBRIEL" msg output-focus-right > /dev/null
wait_for_field "$SCRATCH" active true
if [[ $(field_of "$PLAIN_B" active) != false ]]; then
  echo "output-focus action dismissed the visible scratchpad to the workspace window: $(windows)"
  exit 1
fi

# Same repro again, this time via Cursor::updatePointerOutput() under
# follows_mouse instead of the explicit action above.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[input.focus]
follows_mouse = "sloppy"
EOF
"$UMBRIEL" msg config-reload > /dev/null

# The default two-output headless layout is 1280x720 each, horizontal. Land
# in each output's empty top-left corner, clear of both floating windows'
# centered rects, so this exercises updatePointerOutput()'s output-crossing
# refocus and not the unrelated viewAt-based hover focus landing on a window.
# A real crossing onto A first, away from the scratchpad on B.
"$POINTER" 2560 720 move 20 20
wait_for_field "$PLAIN_A" active true
wait_for_field "$SCRATCH" active false

# The repro: crossing back onto B, which still has the visible scratchpad.
"$POINTER" 2560 720 move 1300 20
wait_for_field "$SCRATCH" active true
if [[ $(field_of "$PLAIN_B" active) != false ]]; then
  echo "pointer-driven output crossing dismissed the visible scratchpad to the workspace window: $(windows)"
  exit 1
fi

echo "an explicit output-focus action and pointer-driven output crossing both refocus a visible scratchpad instead of dismissing it"
