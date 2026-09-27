#!/usr/bin/env bash
# harness: outputs=2
# Scratchpads use one implicit default until named definitions are configured.
# Named scratchpads roam as a group, retain distinct membership, and replace
# another visible scratchpad only on the output they are summoned to.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly BTN_LEFT=272
readonly BASELINE="$(< "$UMBRIEL_CONFIG")"
readonly DEFAULT_WINDOW=named-scratchpad-default
readonly TERM_A=named-scratchpad-term-a
readonly TERM_B=named-scratchpad-term-b
readonly MUSIC=named-scratchpad-music

write_config() {
  {
    printf '%s\n' "$BASELINE"
    printf '%s\n' "$1"
  } > "$UMBRIEL_CONFIG"
}

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

wait_for_x_range() {
  local title=$1 minimum=$2 maximum=$3 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$title" x)
    if [[ $actual =~ ^-?[0-9]+$ ]] && ((actual >= minimum && actual < maximum)); then
      return 0
    fi
    sleep 0.1
  done
  echo "expected '$title' x in [$minimum,$maximum), got '$actual': $(windows)"
  return 1
}

wait_for_term_active() {
  local state=
  for _ in $(seq 80); do
    state=$(windows)
    if jq -e --arg first "$TERM_A" --arg second "$TERM_B" '
      any(.[]; (.title == $first or .title == $second) and .active)
    ' <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "neither term scratchpad window became active: $state"
  return 1
}

focus_window() {
  local id=
  id=$(field_of "$1" id)
  "$UMBRIEL" msg "window-focus:$id" > /dev/null
}

expect_rejected() {
  local action=$1 expected=$2 output=
  if output=$("$UMBRIEL" msg "$action" 2>&1); then
    echo "expected '$action' to be rejected with '$expected'"
    return 1
  fi
  if [[ $output != *"$expected"* ]]; then
    echo "expected '$action' to mention '$expected', got: $output"
    return 1
  fi
}

readonly COMMON_CONFIG='
[animation]
enabled = false

[animation.scratchpad]
scale = 0

[output.HEADLESS-1]
mode = "1280x720"
position = [0, 0]
workspaces = ["LEFT"]

[output.HEADLESS-2]
mode = "1280x720"
position = [1280, 0]
workspaces = ["RIGHT"]

[[window_rule]]
match.title = "^named-scratchpad-"
default_output = "HEADLESS-1"
default_floating = true
default_floating_size_px = { width = 360, height = 220 }
default_position = { x = 240, y = 180, anchor = "top_left" }
'

# With no definitions, bare actions address the implicit default scratchpad.
write_config "$COMMON_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$POINTER" 2560 720 move 640 360

"$CLIENT" "$DEFAULT_WINDOW" > "$UMBRIEL_RUNTIME_DIR/$DEFAULT_WINDOW.log" 2>&1 &
wait_for_count 1
focus_window "$DEFAULT_WINDOW"
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
wait_for_field "$DEFAULT_WINDOW" scratchpad default
"$UMBRIEL" msg scratchpad-toggle > /dev/null
wait_for_field "$DEFAULT_WINDOW" active true
"$UMBRIEL" msg scratchpad-toggle > /dev/null

# Defining any named scratchpad removes the implicit default. Its existing
# member returns to its saved workspace during the inventory reconciliation.
write_config "$COMMON_CONFIG

[[scratchpad]]
name = \"term\"

[[scratchpad]]
name = \"music\""
"$UMBRIEL" msg config-reload > /dev/null
wait_for_field "$DEFAULT_WINDOW" scratchpad ""
if [[ -z $(field_of "$DEFAULT_WINDOW" workspace) ]]; then
  echo "the removed implicit default did not restore its window: $(windows)"
  exit 1
fi
expect_rejected scratchpad-toggle "scratchpad name"
expect_rejected scratchpad-toggle:missing "unknown scratchpad"

for title in "$TERM_A" "$TERM_B"; do
  expected_count=$(windows | jq 'length')
  expected_count=$((expected_count + 1))
  "$CLIENT" "$title" > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  wait_for_count "$expected_count"
  focus_window "$title"
  "$UMBRIEL" msg window-move-to-scratchpad:term > /dev/null
  wait_for_field "$title" scratchpad term
done

"$CLIENT" "$MUSIC" > "$UMBRIEL_RUNTIME_DIR/$MUSIC.log" 2>&1 &
wait_for_count 4
focus_window "$MUSIC"
"$UMBRIEL" msg window-move-to-scratchpad:music > /dev/null
wait_for_field "$MUSIC" scratchpad music

# Show term on the left, then invoke it from the right. A visible global
# scratchpad is summoned rather than hidden when the invoking output changes.
"$UMBRIEL" msg scratchpad-toggle:term > /dev/null
wait_for_field "$TERM_A" active true
"$POINTER" 2560 720 move 1920 360
"$UMBRIEL" msg scratchpad-toggle:term > /dev/null
wait_for_x_range "$TERM_A" 1280 2560
wait_for_x_range "$TERM_B" 1280 2560
wait_for_term_active

# Showing music on the occupied right output hides term. A focus request for
# the hidden term pad must not take focus back from music.
"$UMBRIEL" msg scratchpad-toggle:music > /dev/null
wait_for_field "$MUSIC" active true
"$UMBRIEL" msg scratchpad-focus-next:term > /dev/null
for _ in $(seq 5); do
  sleep 0.1
  if [[ $(field_of "$MUSIC" active) != true
      || $(field_of "$TERM_A" active) == true
      || $(field_of "$TERM_B" active) == true ]]; then
    echo "term remained visible after music replaced it on HEADLESS-2: $(windows)"
    exit 1
  fi
done

# Summoning term to the free left output leaves music visible on the right.
# Both names must then accept focus independently.
"$POINTER" 2560 720 move 640 360
"$UMBRIEL" msg scratchpad-toggle:term > /dev/null
wait_for_x_range "$TERM_A" 0 1280
wait_for_x_range "$TERM_B" 0 1280
"$UMBRIEL" msg scratchpad-focus-next:music > /dev/null
wait_for_field "$MUSIC" active true
"$UMBRIEL" msg scratchpad-focus-next:term > /dev/null
wait_for_term_active

# Separate the two term members on the left, then drag one onto the right.
# The dragged member keeps its drop position while its sibling follows with
# the same output-relative placement. Music is displaced only on the target.
focus_window "$TERM_B"
"$POINTER" 2560 720 \
  move 400 280 mod logo press "$BTN_LEFT" move 680 400 release "$BTN_LEFT" mod none
wait_for_field "$TERM_B" x 520
wait_for_field "$TERM_B" y 300

focus_window "$TERM_A"
"$POINTER" 2560 720 \
  move 400 280 mod logo press "$BTN_LEFT" move 1500 400 release "$BTN_LEFT" mod none
wait_for_field "$TERM_A" x 1340
wait_for_field "$TERM_A" y 300
wait_for_field "$TERM_B" x 1800
wait_for_field "$TERM_B" y 300
if [[ $(field_of "$MUSIC" active) == true ]]; then
  echo "dragging term onto HEADLESS-2 did not hide music: $(windows)"
  exit 1
fi

# Starting a drag from maximize-to-edges restores the dragged member's
# floating box. The whole scratchpad still follows it to the destination.
focus_window "$TERM_A"
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
wait_for_field "$TERM_A" x 1280
wait_for_field "$TERM_A" y 0
wait_for_field "$TERM_A" w 1280
wait_for_field "$TERM_A" h 720
"$POINTER" 2560 720 \
  move 1600 360 mod logo press "$BTN_LEFT" move 500 360 release "$BTN_LEFT" mod none
wait_for_field "$TERM_A" x 240
wait_for_field "$TERM_A" y 300
wait_for_field "$TERM_A" w 360
wait_for_field "$TERM_A" h 220
wait_for_field "$TERM_B" x 520
wait_for_field "$TERM_B" y 300

# Regular floating maximize follows the same drag contract.
focus_window "$TERM_A"
"$UMBRIEL" msg window-toggle-maximize > /dev/null
wait_for_field "$TERM_A" x 0
wait_for_field "$TERM_A" y 0
wait_for_field "$TERM_A" w 1280
wait_for_field "$TERM_A" h 720
"$POINTER" 2560 720 \
  move 320 360 mod logo press "$BTN_LEFT" move 1500 360 release "$BTN_LEFT" mod none
wait_for_field "$TERM_A" x 1420
wait_for_field "$TERM_A" y 300
wait_for_field "$TERM_A" w 360
wait_for_field "$TERM_A" h 220
wait_for_field "$TERM_B" x 1800
wait_for_field "$TERM_B" y 300

# A fullscreen drag must preserve the ordinary floating-maximize state beneath
# fullscreen. One maximize toggle after exiting must therefore restore the
# pre-maximize box, rather than maximize an already output-sized window again.
focus_window "$TERM_A"
"$UMBRIEL" msg window-toggle-maximize > /dev/null
wait_for_field "$TERM_A" x 1280
wait_for_field "$TERM_A" y 0
wait_for_field "$TERM_A" w 1280
wait_for_field "$TERM_A" h 720
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_field "$TERM_A" x 1280
wait_for_field "$TERM_A" y 0
wait_for_field "$TERM_A" w 1280
wait_for_field "$TERM_A" h 720
"$POINTER" 2560 720 \
  move 1500 360 mod logo press "$BTN_LEFT" move 500 360 release "$BTN_LEFT" mod none
wait_for_field "$TERM_A" x 0
wait_for_field "$TERM_A" y 0
wait_for_field "$TERM_A" w 1280
wait_for_field "$TERM_A" h 720
wait_for_field "$TERM_B" x 520
wait_for_field "$TERM_B" y 300
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_field "$TERM_A" x 0
wait_for_field "$TERM_A" y 0
wait_for_field "$TERM_A" w 1280
wait_for_field "$TERM_A" h 720
"$UMBRIEL" msg window-toggle-maximize > /dev/null
wait_for_field "$TERM_A" x 140
wait_for_field "$TERM_A" y 300
wait_for_field "$TERM_A" w 360
wait_for_field "$TERM_A" h 220

echo "implicit and named scratchpads retained identity, roamed as groups, and kept one visible pad per output"
