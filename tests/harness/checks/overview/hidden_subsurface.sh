#!/usr/bin/env bash
# A card mirrors each surface of its window. A client can hide a subsurface by attaching a null buffer to it, which
# drops the surface out of the commit walk that refreshes those mirrors. The card must hide that mirror: left alone it
# kept the last frame at the zoom it was last laid out at, so a layer hidden during the opening zoom stayed on screen
# larger than the card, anchored at the card's corner. The client's parent is red under a blue child that covers it, so
# blue on screen is the child's mirror. The last two steps also guard reads of a scene node freed with a destroyed
# subsurface role, which only `just mode=asan check` reports.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly LOG="$UMBRIEL_RUNTIME_DIR/hidden-subsurface.log"
readonly CONTROL="$UMBRIEL_RUNTIME_DIR/hidden-subsurface.fifo"
readonly BLUE='b > 0.5 && r < 0.3 && g < 0.3'
readonly RED='r > 0.5 && b < 0.3 && g < 0.3'

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[colors]
backdrop = "#000000FF"

[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"

[overview]
zoom = 0.25
background_blur = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$CONTROL"
exec {control_fd}<> "$CONTROL"
CHILD_ON_STDIN=1 "$UMBRIEL_SUBSURFACE_CLIENT" hidden-subsurface 600 400 animate < "$CONTROL" > "$LOG" 2>&1 &
await_lines "$LOG" mapped 1
"$UMBRIEL" settle > /dev/null

child() {
  printf '%s' "$1" >&"$control_fd"
  await_lines "$LOG" "$2" "$3"
}

# bbox <label> <predicate>: the box the colour covers, "0 0 0 0" when absent.
bbox() {
  grim "$UMBRIEL_RUNTIME_DIR/$1.png"
  "$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/$1.png" bbox "$2"
}

# The popup the child moves into last. Umbriel configures no popup created while the overview is open, so it maps now.
child p popup-mapped 1
"$UMBRIEL" settle > /dev/null

# The card's own box, read from the settled overview before anything is hidden.
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle > /dev/null
read -r _ _ card_w card_h < <(bbox open "$BLUE")
if ((card_w == 0 || card_w > 300)); then
  echo "setup: the overview card did not show the child at card size: ${card_w}x${card_h}"
  exit 1
fi
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" settle > /dev/null

# The layer hides 40 ms into the opening zoom, while the card is still several times its final size.
"$UMBRIEL" clock-freeze > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" clock-advance 40 > /dev/null
child h child-hidden 1
"$UMBRIEL" clock-advance 5000 > /dev/null
"$UMBRIEL" clock-resume > /dev/null
"$UMBRIEL" settle > /dev/null
read -r x y w h < <(bbox zoom-hidden "$BLUE")
if ((w > 0)); then
  echo "a subsurface hidden during the opening zoom left its mirror on screen: ${w}x${h}+${x}+${y}, card ${card_w}x${card_h}"
  exit 1
fi
read -r _ _ w h < <(bbox zoom-hidden-parent "$RED")
if ((w != card_w || h != card_h)); then
  echo "the card did not show its parent under the hidden layer: ${w}x${h}, card ${card_w}x${card_h}"
  exit 1
fi

# Shown again, the layer returns on the card at card size, not at a size from before it hid.
child s child-shown 1
"$UMBRIEL" settle > /dev/null
read -r _ _ w h < <(bbox reshown "$BLUE")
if ((w != card_w || h != card_h)); then
  echo "a re-shown subsurface did not return at card size: ${w}x${h}, card ${card_w}x${card_h}"
  exit 1
fi

# Hidden while the overview is settled, the card shows the parent instead of the layer's last frame.
child h child-hidden 2
"$UMBRIEL" settle > /dev/null
read -r x y w h < <(bbox settled-hidden "$BLUE")
if ((w > 0)); then
  echo "a subsurface hidden in the settled overview kept its mirror: ${w}x${h}+${x}+${y}"
  exit 1
fi

# A client can also retire a subsurface by destroying its role while the surface lives on. That frees the scene node
# the mirror was copied from, so taking the same surface back as a subsurface must mirror it from its new node.
child s child-shown 2
"$UMBRIEL" settle > /dev/null
child d child-detached 1
"$UMBRIEL" settle > /dev/null
read -r x y w h < <(bbox detached "$BLUE")
if ((w > 0)); then
  echo "a subsurface whose role was destroyed kept its mirror: ${w}x${h}+${x}+${y}"
  exit 1
fi
child s child-shown 3
"$UMBRIEL" settle > /dev/null
read -r _ _ w h < <(bbox reattached "$BLUE")
if ((w != card_w || h != card_h)); then
  echo "a surface taken back as a subsurface did not return at card size: ${w}x${h}, card ${card_w}x${card_h}"
  exit 1
fi

# A client may also give a retired surface to another parent, such as a popup. Mapped there, it is no longer part of
# the window the card mirrors, and the card must not draw it from the scene node its old role left behind.
child d child-detached 2
"$UMBRIEL" settle > /dev/null
child m child-moved 1
"$UMBRIEL" settle > /dev/null
read -r x y w h < <(bbox moved "$BLUE")
if ((w > 0)); then
  echo "a surface moved into a popup still drew on the window's card: ${w}x${h}+${x}+${y}"
  exit 1
fi

echo "a hidden, detached or moved subsurface leaves its overview card, and returns at card size when shown again"
