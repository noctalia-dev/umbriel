#!/usr/bin/env bash
# window-toggle-floating on a tiled window while the overview is open raises its card into the floating layer, and the
# overview it leaves matches one built fresh after the toggle, shortcut badges included.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/overview-floating-stacking.png"
readonly FRESH="$UMBRIEL_RUNTIME_DIR/overview-floating-stacking-fresh.png"
readonly BASE_COLOR=0xFFFF0000
readonly TOGGLED_COLOR=0xFF0000FF

if [[ ! -x $CLIENT ]]; then
  echo "unmap client is not built"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[overview]
background_blur = false

[[window_rule]]
match.title = "^stacking-base$"
default_floating = true
default_floating_size = { width = 1.0, height = 1.0 }
EOF
"$UMBRIEL" msg config-reload > /dev/null

window_of() { "$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)'; }

spawn() {
  local title=$1 width=$2 height=$3 color=$4
  RESIZE_FILL_COLOR="$color" "$CLIENT" "$title" "$width" "$height" > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 80); do
    [[ -n "$(window_of "$title")" ]] && return 0
    sleep 0.025
  done
  echo "window '$title' never appeared: $(cat "$UMBRIEL_RUNTIME_DIR/$title.log")"
  return 1
}

# The base window is floating from the start and covers the whole usable area, so wherever the toggled window lands
# once it floats, it lands inside the base window's card.
spawn stacking-base 600 400 "$BASE_COLOR"
spawn stacking-toggled 300 200 "$TOGGLED_COLOR"
toggled_id=$(window_of stacking-toggled | jq -r .id)

"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle

"$UMBRIEL" msg "window-focus:$toggled_id" > /dev/null
"$UMBRIEL" msg window-toggle-floating > /dev/null
"$UMBRIEL" settle

grim "$IMAGE"

read -r bx by bw bh < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox 'b > 0.5 && r < 0.2')
if ((bw == 0 && bh == 0)); then
  echo "toggled window's card is fully hidden behind the base window: it was not raised into the floating layer"
  exit 1
fi

"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
grim "$FRESH"
differing=$(magick compare -metric AE "$IMAGE" "$FRESH" null: 2>&1 || true)
if [[ ${differing%% *} != 0 ]]; then
  echo "the live toggle left the overview differing from a fresh one by ${differing%% *} pixels"
  exit 1
fi

echo "a window toggled floating while the overview is open is raised above a floating window that was already there, as a fresh overview would show it"
