#!/usr/bin/env bash
# Focusing the hidden dialog of a hidden member, while a scratchpad shows another window on its own, shows that member
# with its dialog rather than the dialog alone, and the dialog takes focus.
set -euo pipefail

readonly PARENT_CLIENT="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly PARENT_LOG="$UMBRIEL_RUNTIME_DIR/activate-parent.log"
readonly FRAME="$UMBRIEL_RUNTIME_DIR/show-step-dialog-activate.png"

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

[[window_rule]]
match.title = "^activate-parent$"
default_floating = true
default_floating_size_px = { width = 640, height = 480 }
default_position = { x = 100, y = 110, anchor = "top_left" }

[[window_rule]]
match.title = "^activate-other$"
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

EXPORT_TOPLEVEL=1 "$PARENT_CLIENT" activate-parent > "$PARENT_LOG" 2>&1 &
wait_for_window activate-parent
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

FILL_COLOR=0xFFFF0000 "$CLIENT" activate-other 200 150 > "$UMBRIEL_RUNTIME_DIR/activate-other.log" 2>&1 &
wait_for_window activate-other
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null

# With every window shown, the parent opens a dialog that joins the scratchpad.
"$UMBRIEL" msg scratchpad-toggle > /dev/null
FILL_COLOR=0xFF00FF00 TRANSIENT_FOREIGN_HANDLE="$handle" \
  "$CLIENT" activate-dialog 300 200 > "$UMBRIEL_RUNTIME_DIR/activate-dialog.log" 2>&1 &
wait_for_window activate-dialog
expect "red green parent" "every window and the dialog did not show"

# Step to the red window, hiding the parent and its dialog.
"$UMBRIEL" msg window-focus "$(window_of activate-parent | jq -r .id)" > /dev/null
"$UMBRIEL" msg scratchpad-window-show-next > /dev/null
expect "red" "stepping from the parent did not show the red window on its own"

# Focusing the hidden dialog brings its parent along.
"$UMBRIEL" msg window-focus "$(window_of activate-dialog | jq -r .id)" > /dev/null
expect "green parent" "focusing the hidden dialog did not show it with its parent"
if [[ $(windows | jq -r '.[] | select(.active) | .title') != activate-dialog ]]; then
  echo "the dialog asked for did not take focus: $(windows)"
  exit 1
fi

echo "focusing a hidden dialog showed it with the window it belongs to"
