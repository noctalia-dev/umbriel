#!/usr/bin/env bash
# harness: outputs=2
# When a window rule moves the focused window a scratchpad shows on its own into another scratchpad that is showing a
# different window on the other output, the moved window is not shown there, so it must not keep keyboard focus.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CONTROL="$UMBRIEL_RUNTIME_DIR/transfer-outputs-control"
readonly FRAME="$UMBRIEL_RUNTIME_DIR/show-step-transfer-outputs.png"
readonly RED_LOG="$UMBRIEL_RUNTIME_DIR/transfer-red.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[animation.scratchpad]
dim = 0.0
blur = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[output.HEADLESS-1]
mode = "1280x720"
position = [0, 0]

[output.HEADLESS-2]
mode = "1280x720"
position = [1280, 0]

[[scratchpad]]
name = "a"

[[scratchpad]]
name = "b"

[[window_rule]]
match.title = "^transfer-"
default_floating = true
default_floating_size_px = { width = 300, height = 200 }

[[window_rule]]
match.title = "^moved-out$"
default_scratchpad = "b"
EOF
"$UMBRIEL" msg config-reload > /dev/null

windows() { "$UMBRIEL" windows --json; }
window_of() { windows | jq -c --arg title "$1" '.[] | select(.title == $title)'; }

wait_for_window() {
  for _ in $(seq 80); do
    [[ -n "$(window_of "$1")" ]] && return 0
    sleep 0.05
  done
  echo "window '$1' never appeared: $(windows)"
  return 1
}

# Which of the two colours are on screen across both outputs.
shown() {
  "$UMBRIEL" settle
  grim "$FRAME"
  local visible=() w h
  read -r _ _ w h < <("$UMBRIEL_PIXEL_PROBE" "$FRAME" bbox 'r > 0.9 && g < 0.1 && b < 0.1')
  ((w > 0 && h > 0)) && visible+=(red)
  read -r _ _ w h < <("$UMBRIEL_PIXEL_PROBE" "$FRAME" bbox 'g > 0.9 && r < 0.1 && b < 0.1')
  ((w > 0 && h > 0)) && visible+=(green)
  echo "${visible[*]}"
}

# Scratchpad b shows its green window on its own on the right output.
"$POINTER" 2560 720 move 1920 360
FILL_COLOR=0xFF00FF00 "$CLIENT" transfer-green 300 200 > "$UMBRIEL_RUNTIME_DIR/transfer-green.log" 2>&1 &
wait_for_window transfer-green
"$UMBRIEL" msg window-move-to-scratchpad b > /dev/null
"$UMBRIEL" msg scratchpad-window-show-next b > /dev/null

# Scratchpad a shows its red window on its own, focused, on the left output.
"$POINTER" 2560 720 move 640 360
mkfifo "$CONTROL"
exec {control_fd}<> "$CONTROL"
FILL_COLOR=0xFFFF0000 TITLE_AFTER_MAP=moved-out "$CLIENT" transfer-red 300 200 \
  < "$CONTROL" > "$RED_LOG" 2>&1 &
wait_for_window transfer-red
"$UMBRIEL" msg window-move-to-scratchpad a > /dev/null
"$UMBRIEL" msg scratchpad-window-show-next a > /dev/null
before=$(shown)
if [[ $before != "red green" ]]; then
  echo "both scratchpads did not show their window: [$before]: $(windows)"
  exit 1
fi
if [[ $(window_of transfer-red | jq -r .active) != true ]]; then
  echo "the red window did not have focus before moving: $(windows)"
  exit 1
fi

# The new title moves the red window into b, which keeps showing green on its own.
printf 't\n' >&"$control_fd"
for _ in $(seq 80); do
  [[ $(window_of moved-out | jq -r .scratchpad) == b ]] && break
  sleep 0.05
done
if [[ $(window_of moved-out | jq -r .scratchpad) != b ]]; then
  echo "the window rule did not move the red window into scratchpad b: $(windows)"
  exit 1
fi
after=$(shown)
if [[ $after == *red* ]]; then
  echo "the moved window is shown although b shows another window on its own: [$after]: $(windows)"
  exit 1
fi

# A key typed now must not reach the window nobody can see.
"$POINTER" 1 1 keyboard-only tap 30 > /dev/null
"$UMBRIEL" settle
if grep -q '^key 30 ' "$RED_LOG"; then
  echo "a key typed after the move reached the hidden window: $(grep -E '^keyboard-|^key ' "$RED_LOG" | tr '\n' ' ')"
  exit 1
fi

echo "a window moved into a scratchpad showing another window gave up keyboard focus"
