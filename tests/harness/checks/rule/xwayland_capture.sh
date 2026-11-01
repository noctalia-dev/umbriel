#!/usr/bin/env bash
# harness: xwayland=true
# Managed X11 transients inherit protection; unowned override-redirect windows
# are conservatively excluded while an X11 view is protected.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[[window_rule]]
match.title = "^xwayland-dialog-parent$"
block_capture = true
default_floating = true
[[window_rule]]
match.title = "^xwayland-dialog-child$"
block_capture = false
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
mkfifo "$UMBRIEL_RUNTIME_DIR/dialog" "$UMBRIEL_RUNTIME_DIR/popup"
exec 8<> "$UMBRIEL_RUNTIME_DIR/dialog"
exec 9<> "$UMBRIEL_RUNTIME_DIR/popup"
"$UMBRIEL_XWAYLAND_DIALOG_CLIENT" <&8 > "$UMBRIEL_RUNTIME_DIR/dialog.log" 2>&1 &
await_window xwayland-dialog-parent
printf 'map-dialog\n' >&8
await_window xwayland-dialog-child
"$UMBRIEL_XWAYLAND_POPUP_CLIENT" capture-popup <&9 > "$UMBRIEL_RUNTIME_DIR/popup.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/popup.log" ready 1
printf 'override true\nmap\n' >&9
await_lines "$UMBRIEL_RUNTIME_DIR/popup.log" mapped 1
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/capture.png"
# The fixtures use #306090 and #60c030. Borders and shadows are disabled.
pixels() {
  "$UMBRIEL" settle > /dev/null
  grim "$IMAGE"
  "$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r>0.17&&r<0.21&&g>0.35&&g<0.40&&b>0.53&&b<0.59' 'r>0.35&&r<0.40&&g>0.72&&g<0.78&&b>0.17&&b<0.21'
}
read -r parent child < <(pixels)
(( parent == 0 && child == 0 ))
sed -i 's/^block_capture = true$/block_capture = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
read -r parent child < <(pixels)
(( parent > 1000 && child > 1000 ))
sed -i '0,/block_capture = false/s//block_capture = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
read -r parent child < <(pixels)
(( parent == 0 && child == 0 ))
echo 'X11 transient inheritance, unowned popup exclusion, and reload restoration verified'
