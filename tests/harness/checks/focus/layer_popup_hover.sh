#!/usr/bin/env bash
# harness: outputs=2
# A grabbing layer popup retains keyboard focus when follows_mouse crosses a window or another output. An outside
# click still dismisses it, after which ordinary hover focus resumes.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly OUTPUT_W=2560
readonly OUTPUT_H=720
readonly LEFT_BUTTON=272
readonly SPACE=57
readonly WINDOW_LOG="$UMBRIEL_RUNTIME_DIR/layer-popup-hover-window.log"
readonly OTHER_LOG="$UMBRIEL_RUNTIME_DIR/layer-popup-hover-other.log"
readonly PANEL_LOG="$UMBRIEL_RUNTIME_DIR/layer-popup-hover-panel.log"

refuse_events() {
  local file=$1 event=$2 limit=$3 label=$4
  local seen
  seen=$(events "$file" "$event")
  if ((seen > limit)); then
    echo "$label received $seen '$event', expected at most $limit: $(tr '\n' '|' < "$file")"
    return 1
  fi
}

assert_popup_keeps_keyboard() {
  local key_count=$1 transition=$2
  # Receiving this key proves that the menu kept the keyboard after the preceding pointer motion was processed.
  await_events "$PANEL_LOG" "keyboard-key code=$SPACE state=1" "$key_count" "the menu after $transition"
  refuse_events "$PANEL_LOG" popup-done 0 "the menu"
  refuse_events "$PANEL_LOG" keyboard-leave 0 "the menu"
  refuse_events "$WINDOW_LOG" keyboard-enter "$window_enters" "the first-output window"
  refuse_events "$OTHER_LOG" keyboard-enter "$other_enters" "the second-output window"
}

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = false

[input.focus]
follows_mouse = true

[layout.scrolling]
default_extent_fraction = 1.0

[output.HEADLESS-1]
position = [0, 0]
scale = 1

[output.HEADLESS-2]
position = [1280, 0]
scale = 1

[[window_rule]]
match.title = "^layer-popup-hover-window$"
default_output = "HEADLESS-1"

[[window_rule]]
match.title = "^layer-popup-hover-other$"
default_output = "HEADLESS-2"
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

"$UMBRIEL_SEAT_LOG_CLIENT" layer-popup-hover-window > "$WINDOW_LOG" 2>&1 &
await_events "$WINDOW_LOG" keyboard-enter 1 "the first-output window"
"$UMBRIEL_SEAT_LOG_CLIENT" layer-popup-hover-other > "$OTHER_LOG" 2>&1 &
await_events "$OTHER_LOG" keyboard-enter 1 "the second-output window"

window_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "layer-popup-hover-window") | .id')
window_enters=$(events "$WINDOW_LOG" keyboard-enter)
"$UMBRIEL" msg "window-focus:$window_id" > /dev/null
await_events "$WINDOW_LOG" keyboard-enter "$((window_enters + 1))" "the refocused first-output window"
"$UMBRIEL" settle > /dev/null
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 40 popup-on-click > "$PANEL_LOG" 2>&1 &
await_events "$PANEL_LOG" ready 1 "the bar"
window_enters=$(events "$WINDOW_LOG" keyboard-enter)
other_enters=$(events "$OTHER_LOG" keyboard-enter)
window_leaves=$(events "$WINDOW_LOG" keyboard-leave)

pointer_hold "$OUTPUT_W" "$OUTPUT_H" move 640 20 click "$LEFT_BUTTON" -- \
  move 640 360 tap "$SPACE" mark same-output hold \
  move 1920 360 tap "$SPACE" mark other-output hold \
  click "$LEFT_BUTTON" mark dismissed hold \
  move 1921 360 tap "$SPACE"
await_events "$PANEL_LOG" popup-mapped 1 "the menu"
await_events "$PANEL_LOG" keyboard-enter 1 "the menu"
await_events "$WINDOW_LOG" keyboard-leave "$((window_leaves + 1))" "the first-output window"
refuse_events "$PANEL_LOG" popup-done 0 "the menu"

# Keep the two transitions separate so a missing same-output guard cannot be hidden by an output refocus fallback.
pointer_step same-output
assert_popup_keeps_keyboard 1 "same-output hover"
pointer_step other-output
assert_popup_keeps_keyboard 2 "cross-output hover"

# Explicit interaction may take the seat. A popup must not retain a stale grab after an outside click.
pointer_step dismissed
await_events "$PANEL_LOG" popup-done 1 "the menu after an outside click"
await_events "$OTHER_LOG" keyboard-enter "$((other_enters + 1))" "the clicked second-output window"

# Leave the pointer on the second output while focusing elsewhere. The next real motion must restore hover focus
# without relying on the separate focus handoff performed when crossing outputs.
"$UMBRIEL" msg "window-focus:$window_id" > /dev/null
await_events "$WINDOW_LOG" keyboard-enter "$((window_enters + 1))" "the refocused first-output window"
pointer_release
await_events "$OTHER_LOG" keyboard-enter "$((other_enters + 2))" "the hovered second-output window"
await_events "$OTHER_LOG" "keyboard-key code=$SPACE state=pressed" 1 "the hovered second-output window"

echo "layer popup grabs survive window and output hover; outside clicks dismiss and restore hover focus"
