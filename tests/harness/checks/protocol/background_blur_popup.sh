#!/usr/bin/env bash
# A popup that hides itself keeps its backdrop blur hidden. The surface keeps the window geometry a toolkit gave it
# after the buffer is gone, and its host scene tree stays enabled, so a blur derived from those alone outlives the
# unmap: the commit that drops the buffer turns the node back on, and every later blur refresh keeps it on. The popup
# here is transparent, so the only thing a frame can show of it is the blur it asks for.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_POPUP_CLIENT:-./build-debug/tests/popup-client}"
readonly LOG="$UMBRIEL_RUNTIME_DIR/popup-blur.log"
readonly SHOWN="$UMBRIEL_RUNTIME_DIR/popup-blur-shown.png"
readonly HIDDEN="$UMBRIEL_RUNTIME_DIR/popup-blur-hidden.png"
readonly RELOADED="$UMBRIEL_RUNTIME_DIR/popup-blur-reloaded.png"

fail() {
  echo "$1"
  exit 1
}

# Pixels that differ at all between two frames.
differing() { "$UMBRIEL_PIXEL_PROBE" "$1" diff "$2" | cut -d' ' -f2; }

expect_same() {
  local count
  count=$(differing "$1" "$2")
  if ((count != 0)); then
    fail "$3: $count pixels differ"
  fi
}

[[ -x $CLIENT ]] || fail "popup client is not built"

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[appearance.blur]
enabled = true
optimized = false
passes = 2
radius = 6
noise = 0.0
brightness = 1.0
contrast = 1.0
saturation = 1.0

[animation]
enabled = false

[[window_rule]]
match.title = "^popup-focus-regression$"
blur_popups = true
default_floating = true
default_position = { x = 0, y = 0, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

# The wallpaper's square ends at (200, 200); the popup covers (20, 20) to (260, 260), so its blur crosses that edge
# and a frame with the blur differs from one without it.
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 > "$UMBRIEL_RUNTIME_DIR/background.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/background.log" ready 1
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 bottom-layer > "$UMBRIEL_RUNTIME_DIR/square.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/square.log" ready 1
"$UMBRIEL" settle

mkfifo "$UMBRIEL_RUNTIME_DIR/popup-blur.fifo"
FILL_COLOR=0x00000000 POPUP_FILL_COLOR=0x00000000 POPUP_WIDTH=240 POPUP_HEIGHT=240 \
  "$CLIENT" --popup-on-stdin < "$UMBRIEL_RUNTIME_DIR/popup-blur.fifo" > "$LOG" 2>&1 &
exec 3> "$UMBRIEL_RUNTIME_DIR/popup-blur.fifo"
send() { printf '%s\n' "$1" >&3; }
# A popup commit draws no reply, so a frame callback on the toplevel (sent behind it) is the barrier that proves the
# compositor read it; `settle` alone can answer before the commit is even dispatched.
frames=0
synced() {
  frames=$((frames + 1))
  send sync
  await_lines "$LOG" frame "$frames"
}

await_lines "$LOG" ready 1
send popup
await_lines "$LOG" popup-mapped 1
synced
"$UMBRIEL" settle
grim "$SHOWN"

send unmap
await_lines "$LOG" popup-unmapped 1
synced
"$UMBRIEL" settle
grim "$HIDDEN"

blurred=$(differing "$SHOWN" "$HIDDEN")
if ((blurred < 500)); then
  fail "the popup blur is not observable, or it outlived the unmap: only $blurred pixels differ"
fi

# A reload that touches window chrome re-derives every live popup, including the unmapped one.
printf '\n[colors]\naccent_primary = "#7AA3FF"\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
grim "$RELOADED"
expect_same "$RELOADED" "$HIDDEN" "a config reload re-enabled the blur of an unmapped popup"
