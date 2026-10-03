#!/usr/bin/env bash
# A dialog opened by the window a scratchpad shows on its own joins the scratchpad beside that window instead of
# replacing it, and closing the dialog leaves the window on show.
set -euo pipefail

readonly PARENT_CLIENT="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly PARENT_LOG="$UMBRIEL_RUNTIME_DIR/dialog-parent.log"
readonly FRAME="$UMBRIEL_RUNTIME_DIR/show-step-dialog.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[animation.scratchpad]
dim = 0.0
blur = false
scale = 0

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[[window_rule]]
match.title = "^dialog-parent$"
default_floating = true
default_floating_size_px = { width = 640, height = 480 }
default_position = { x = 100, y = 110, anchor = "top_left" }

[[window_rule]]
match.title = "^dialog-other$"
default_floating = true
default_floating_size_px = { width = 200, height = 150 }
default_position = { x = 900, y = 400, anchor = "top_left" }
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

# Whether the red window, the green dialog, and the parent (anything bright inside its box but clear of the dialog)
# are on screen.
shown() {
  "$UMBRIEL" settle
  grim "$FRAME"
  local visible=() w h parent
  read -r _ _ w h < <("$UMBRIEL_PIXEL_PROBE" "$FRAME" bbox 'r > 0.9 && g < 0.1 && b < 0.1')
  ((w > 0 && h > 0)) && visible+=(red)
  read -r _ _ w h < <("$UMBRIEL_PIXEL_PROBE" "$FRAME" bbox 'g > 0.9 && r < 0.1 && b < 0.1')
  ((w > 0 && h > 0)) && visible+=(green)
  parent=$(magick "$FRAME" -crop 40x40+110+120 -format '%[fx:mean > 0.05 ? 1 : 0]' info:)
  [[ $parent == 1 ]] && visible+=(parent)
  echo "${visible[*]}"
}

expect() {
  local actual
  actual=$(shown)
  if [[ $actual != "$1" ]]; then
    echo "$2: expected [$1] on screen, got [$actual]: $(windows)"
    exit 1
  fi
}

EXPORT_TOPLEVEL=1 "$PARENT_CLIENT" dialog-parent > "$PARENT_LOG" 2>&1 &
wait_for_window dialog-parent
handle=
for _ in $(seq 80); do
  handle=$(sed -n 's/^exported handle=//p' "$PARENT_LOG")
  [[ -n $handle ]] && break
  sleep 0.05
done
if [[ -z $handle ]]; then
  echo "parent did not export an xdg-foreign handle: $(cat "$PARENT_LOG")"
  exit 1
fi
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null

FILL_COLOR=0xFFFF0000 "$CLIENT" dialog-other 200 150 > "$UMBRIEL_RUNTIME_DIR/dialog-other.log" 2>&1 &
wait_for_window dialog-other
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null

# The parent was focused last, so the first step shows it on its own.
"$UMBRIEL" msg scratchpad-show-next > /dev/null
expect "parent" "the scratchpad did not show the parent on its own"

# Its dialog joins the scratchpad and shows beside it, centred over the parent.
FILL_COLOR=0xFF00FF00 TRANSIENT_FOREIGN_HANDLE="$handle" \
  "$CLIENT" dialog-child 300 200 > "$UMBRIEL_RUNTIME_DIR/dialog-child.log" 2>&1 &
child=$!
wait_for_window dialog-child
expect "green parent" "the dialog replaced its parent instead of showing beside it"

# Stepping skips the dialog: it goes with its parent.
"$UMBRIEL" msg scratchpad-show-next > /dev/null
expect "red" "stepping showed the dialog on its own or kept the parent"
"$UMBRIEL" msg scratchpad-show-next > /dev/null
expect "green parent" "stepping back did not bring the parent and its dialog"

# Closing the dialog leaves the parent on show.
kill "$child"
for _ in $(seq 80); do
  [[ -z "$(window_of dialog-child)" ]] && break
  sleep 0.05
done
expect "parent" "closing the dialog hid the scratchpad"

echo "a dialog showed beside the scratchpad window it belongs to and closing it kept that window on show"
