#!/usr/bin/env bash
# harness: xwayland=true
# harness: xcursor-theme=true
# A theme cursor submitted through Xwayland is promoted to Umbriel's named,
# output-scale-aware cursor. An exact-geometry custom image stays client-owned,
# returning to the theme promotes it again, and native cursor surfaces are never
# candidates for the Xwayland-only matcher.
set -euo pipefail

readonly X_CLIENT="${UMBRIEL_XWAYLAND_CURSOR_CLIENT:-./build-debug/tests/xwayland-cursor-client}"
readonly NATIVE_CLIENT="${UMBRIEL_WAYLAND_CURSOR_CLIENT:-./build-debug/tests/wayland-cursor-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly X_LOG="$UMBRIEL_RUNTIME_DIR/xwayland-cursor.log"
readonly NATIVE_LOG="$UMBRIEL_RUNTIME_DIR/wayland-cursor.log"
readonly X_CONTROL="$UMBRIEL_RUNTIME_DIR/xwayland-cursor-control"
readonly X_TITLE=xwayland-cursor-promotion
readonly NATIVE_TITLE=wayland-cursor-control
readonly OUTPUT_W=640
readonly OUTPUT_H=360
readonly CURSOR_BASE=3
readonly CURSOR_SCALED=6

if [[ ! -x $X_CLIENT || ! -x $NATIVE_CLIENT || ! -x $POINTER || -z ${DISPLAY:-} ]]; then
  echo "cursor promotion helpers or the private DISPLAY are not available"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[output."HEADLESS-1"]
scale = 2
EOF
"$UMBRIEL" msg config-reload > /dev/null

cursor_state() {
  "$UMBRIEL" cursor-state --json 2>/dev/null || true
}

await_state() {
  local label=$1 expression=$2 state=
  for _ in $(seq 80); do
    state=$(cursor_state)
    if jq -e "$expression" <<< "$state" > /dev/null 2>&1; then
      CURSOR_STATE=$state
      return 0
    fi
    sleep 0.05
  done
  echo "$label did not reach the expected cursor state: ${state:-no response}"
  echo "X11 log: $(tr '\n' '|' < "$X_LOG" 2>/dev/null || true)"
  echo "native log: $(tr '\n' '|' < "$NATIVE_LOG" 2>/dev/null || true)"
  return 1
}

await_window() {
  local title=$1 log=$2 windows=
  for _ in $(seq 80); do
    windows=$("$UMBRIEL" windows --json)
    if jq -e --arg title "$title" 'any(.[]; .title == $title)' <<< "$windows" > /dev/null; then
      return 0
    fi
    sleep 0.05
  done
  echo "$title did not map: $windows"
  echo "client log: $(tr '\n' '|' < "$log" 2>/dev/null || true)"
  return 1
}

focus_and_enter() {
  local title=$1 window id x y
  window=$("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)')
  id=$(jq -r '.id' <<< "$window")
  "$UMBRIEL" msg window-focus "$id" > /dev/null
  window=$("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)')
  x=$(jq -r '(.x + .w / 2 | round)' <<< "$window")
  y=$(jq -r '(.y + .h / 2 | round)' <<< "$window")
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" move "$x" "$y" > /dev/null
}

mkfifo "$X_CONTROL"
exec {x_control_fd}<> "$X_CONTROL"
"$X_CLIENT" "$X_TITLE" <&"$x_control_fd" > "$X_LOG" 2>&1 &
await_window "$X_TITLE" "$X_LOG"
focus_and_enter "$X_TITLE"

CURSOR_STATE=
await_state "the initial X11 theme image" \
  ".source == \"xcursor\" and .name == \"default\" and .xwayland == true
   and .image.texture_width == $CURSOR_SCALED and .image.texture_height == $CURSOR_SCALED
   and .image.render_width == $CURSOR_SCALED and .image.render_height == $CURSOR_SCALED"

echo hotspot >&"$x_control_fd"
await_state "the matching X11 pixels with a different hotspot" \
  ".source == \"surface\" and .name == \"\" and .xwayland == true
   and .image.texture_width == $CURSOR_BASE and .image.texture_height == $CURSOR_BASE
   and .image.render_width == $CURSOR_SCALED and .image.render_height == $CURSOR_SCALED"

echo theme >&"$x_control_fd"
await_state "the X11 theme image restored after the hotspot mismatch" \
  ".source == \"xcursor\" and .name == \"default\" and .xwayland == true
   and .image.texture_width == $CURSOR_SCALED and .image.texture_height == $CURSOR_SCALED
   and .image.render_width == $CURSOR_SCALED and .image.render_height == $CURSOR_SCALED"

echo custom >&"$x_control_fd"
await_state "the unmatched X11 cursor image" \
  ".source == \"surface\" and .name == \"\" and .xwayland == true
   and .image.texture_width == $CURSOR_BASE and .image.texture_height == $CURSOR_BASE
   and .image.render_width == $CURSOR_SCALED and .image.render_height == $CURSOR_SCALED"

echo theme >&"$x_control_fd"
await_state "the restored X11 theme image" \
  ".source == \"xcursor\" and .name == \"default\" and .xwayland == true
   and .image.texture_width == $CURSOR_SCALED and .image.texture_height == $CURSOR_SCALED
   and .image.render_width == $CURSOR_SCALED and .image.render_height == $CURSOR_SCALED"

# A theme reload must reclassify the current image in both directions without
# waiting for Xwayland to submit the cursor again.
sed -i 's/^size = 24$/size = 48/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
await_state "the old X11 theme image after selecting a different base size" \
  ".source == \"surface\" and .name == \"\" and .xwayland == true
   and .image.texture_width == $CURSOR_BASE and .image.texture_height == $CURSOR_BASE
   and .image.render_width == $CURSOR_SCALED and .image.render_height == $CURSOR_SCALED"

sed -i 's/^size = 48$/size = 24/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
await_state "the X11 theme image promoted again after restoring the base size" \
  ".source == \"xcursor\" and .name == \"default\" and .xwayland == true
   and .image.texture_width == $CURSOR_SCALED and .image.texture_height == $CURSOR_SCALED
   and .image.render_width == $CURSOR_SCALED and .image.render_height == $CURSOR_SCALED"

# A native client-owned cursor containing the same scale-1 theme pixels remains
# a surface even though the X11 fixture has established a match. This is the
# ownership gate against changing native Wayland cursor semantics.
"$NATIVE_CLIENT" "$NATIVE_TITLE" > "$NATIVE_LOG" 2>&1 &
await_window "$NATIVE_TITLE" "$NATIVE_LOG"
focus_and_enter "$NATIVE_TITLE"
await_state "the native matching theme cursor" \
  ".source == \"surface\" and .name == \"\" and .xwayland == false
   and .image.texture_width == $CURSOR_BASE and .image.texture_height == $CURSOR_BASE
   and .image.render_width == $CURSOR_SCALED and .image.render_height == $CURSOR_SCALED"

# Re-entering the X11 window must replay and promote the client's last theme
# cursor without requiring the application to recreate it.
focus_and_enter "$X_TITLE"
await_state "the replayed X11 theme image" \
  ".source == \"xcursor\" and .name == \"default\" and .xwayland == true
   and .image.texture_width == $CURSOR_SCALED and .image.texture_height == $CURSOR_SCALED
   and .image.render_width == $CURSOR_SCALED and .image.render_height == $CURSOR_SCALED"

echo "Xwayland theme cursors promote by content and hotspot, custom and native matching surfaces stay client-owned, and promotion replays"
