#!/usr/bin/env bash
# A hide_when_empty pad whose last member unmaps while that member's grabbed popup is open still hands focus back to
# the workspace: keyboard input held through the popup counts as the member's.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="$UMBRIEL_UNMAP_CLIENT"
readonly POPUP_CLIENT="$UMBRIEL_POPUP_CLIENT"
readonly POINTER="$UMBRIEL_POINTER_CLIENT"
readonly LEFT_BUTTON=272
readonly POPUP_LOG="$UMBRIEL_RUNTIME_DIR/popup-client.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[input.focus]
follows_mouse = false

[[scratchpad]]
name = "term"
hide_when_empty = true

[[window_rule]]
match.title = "^popup-focus-regression$"
default_scratchpad = "term"
EOF
"$UMBRIEL" msg config-reload > /dev/null

action() { "$UMBRIEL" msg "$@" > /dev/null; }
field() { window_by_title "$1" | jq -r ".$2"; }
await_log() {
  for _ in $(seq 100); do
    grep -qx "$1" "$POPUP_LOG" 2> /dev/null && return 0
    sleep 0.025
  done
  echo "popup client never printed '$1': $(< "$POPUP_LOG")"
  return 1
}

"$CLIENT" home 300 200 > "$UMBRIEL_RUNTIME_DIR/home.log" 2>&1 &
await_window home

"$POPUP_CLIENT" --unmap-on-close > "$POPUP_LOG" 2>&1 &
await_window popup-focus-regression
[[ $(field popup-focus-regression scratchpad) == term ]] || {
  echo "popup client did not join the pad: $(window_by_title popup-focus-regression)"
  exit 1
}
[[ $(field popup-focus-regression visible) == true ]] || action scratchpad-toggle term
action window-focus "$(field popup-focus-regression id)"

# A click on the member opens a grabbed popup, which takes keyboard focus. The pointer stays connected meanwhile.
"$POINTER" 1280 720 move 640 360 click "$LEFT_BUTTON" pause 1500 > "$UMBRIEL_RUNTIME_DIR/pointer.log" 2>&1 &
await_log popup-mapped

action window-close "$(field popup-focus-regression id)"
await_log unmapped
await_window_closed popup-focus-regression
[[ $(field home active) == true ]] || { echo "workspace window did not regain focus: $("$UMBRIEL" windows --json)"; exit 1; }

echo "hide_when_empty returns focus from a member whose popup held it"
