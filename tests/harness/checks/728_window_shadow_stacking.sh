#!/usr/bin/env bash
# A window's shadow falls on every window below it: floating windows on tiles and on each other, and pinned and
# scratchpad windows on their peers. Raising a window brings its shadow along, and a closing window keeps its shadow
# for the fade. Tiles never shadow each other, whichever of two neighbours is on top. Every sample is compared with a
# point on the same window that no shadow reaches, and a bright green shadow makes the difference unmistakable.
set -euo pipefail
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/shadow.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 4000
curve = "linear"
[animation.windows_in]
enabled = false
[animation.windows_move]
enabled = false
[animation.windows_out]
enabled = true
style = "fade"
[animation.scratchpad]
enabled = false
dim = 0.0

[colors]
shadow = "#00FF00FF"

[layout]
gap = 0

[layout.scrolling]
default_extent_fraction = 0.5

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = true
softness = 24
offset_x = 0
offset_y = 0

[[scratchpad]]
name = "shade"

[[window_rule]]
match.title = "^floater$"
default_floating = true
default_position = { x = 700, y = 40, anchor = "top_left" }

[[window_rule]]
match.title = "^upper$"
default_floating = true
default_position = { x = 760, y = 120, anchor = "top_left" }

[[window_rule]]
match.title = "^pin-"
default_floating = true
default_pinned = true

[[window_rule]]
match.title = "^pin-low$"
default_position = { x = 80, y = 60, anchor = "top_left" }

[[window_rule]]
match.title = "^pin-high$"
default_position = { x = 140, y = 140, anchor = "top_left" }

[[window_rule]]
match.title = "^scratch-"
default_scratchpad = "shade"
default_floating = true

[[window_rule]]
match.title = "^scratch-low$"
default_position = { x = 700, y = 300, anchor = "top_left" }

[[window_rule]]
match.title = "^scratch-high$"
default_position = { x = 760, y = 380, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
# Animation time only moves by clock-advance, so every sample sees an exact instant.
"$UMBRIEL" clock-freeze

window_of() { "$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)'; }

spawn() {
  "$UMBRIEL_UNMAP_CLIENT" "$1" "$2" "$3" > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 80); do
    window=$(window_of "$1")
    [[ -n $window ]] && return 0
    sleep 0.025
  done
  echo "window '$1' never appeared"
  return 1
}

box_of() {
  local w
  w=$(window_of "$1")
  jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$w"
}

id_of() { window_of "$1" | jq -r .id; }

# Samples after every animation started so far has finished.
capture() {
  "$UMBRIEL" clock-advance 4000
  grim "$IMAGE"
}

rgb_at() {
  magick "$IMAGE" -crop "4x4+$1+$2" \
    -format '%[fx:round(mean.r*255)] %[fx:round(mean.g*255)] %[fx:round(mean.b*255)]\n' info:
}

# shadowed LABEL X Y REF_X REF_Y: the sample is greener, and no redder or bluer, than the reference.
shadowed() {
  local r g b rr rg rb
  read -r r g b < <(rgb_at "$2" "$3")
  read -r rr rg rb < <(rgb_at "$4" "$5")
  if ! ((g > rg + 12 && r <= rr && b <= rb)); then
    echo "$1: no shadow at $2,$3 ($r $g $b against $rr $rg $rb)"
    exit 1
  fi
}

# clean LABEL X Y REF_X REF_Y: the sample matches the reference.
clean() {
  local r g b rr rg rb
  read -r r g b < <(rgb_at "$2" "$3")
  read -r rr rg rb < <(rgb_at "$4" "$5")
  if ((g > rg + 4 || g < rg - 4)); then
    echo "$1: unexpected shadow at $2,$3 ($r $g $b against $rr $rg $rb)"
    exit 1
  fi
}

# Two tiles side by side: neither shadows the other, whichever is on top. Tiles redraw at their configured size.
RESIZE_FILL_COLOR=0xFF5577AA spawn tile-left 200 200
RESIZE_FILL_COLOR=0xFF5577AA spawn tile-right 200 200
for _ in $(seq 80); do
  (($(box_of tile-right | cut -d" " -f3) > 200)) && break
  sleep 0.025
done
read -r lx ly lw lh < <(box_of tile-left)
read -r rx ry rw rh < <(box_of tile-right)
seam_y=$((ry + rh / 2))
for focus in tile-left tile-right; do
  "$UMBRIEL" msg "window-focus:$(id_of "$focus")" > /dev/null
  capture
  clean "tile-right beside $focus on top" "$((rx + 4))" "$seam_y" "$((rx + rw / 2))" "$seam_y"
  clean "tile-left beside $focus on top" "$((rx - 8))" "$seam_y" "$((lx + lw / 2))" "$seam_y"
done

# A floating window shadows the tile below it.
spawn floater 300 200
capture
read -r fx fy fw fh < <(box_of floater)
shadowed "floating over a tile" "$((fx + 20))" "$((fy + fh + 4))" "$((fx + 20))" "$((ry + rh - 40))"

# A floating window shadows the floating window it covers.
spawn upper 300 200
capture
read -r ux uy uw uh < <(box_of upper)
shadowed "floating over floating" "$((ux - 8))" "$((uy + 20))" "$((fx + 20))" "$((fy + 20))"

# Raising the covered window reverses the pair.
"$UMBRIEL" msg "window-focus:$(id_of floater)" > /dev/null
capture
shadowed "raised floating window" "$((fx + fw + 4))" "$((uy + 40))" "$((ux + uw - 20))" "$((uy + uh - 20))"
clean "lowered floating window" "$((ux - 8))" "$((uy + 20))" "$((fx + 20))" "$((fy + 20))"

# A closing window keeps its shadow on the window below while it fades.
"$UMBRIEL" msg "window-focus:$(id_of upper)" > /dev/null
capture
"$UMBRIEL" msg "window-close:$(id_of upper)" > /dev/null
"$UMBRIEL" clock-advance 200
grim "$IMAGE"
shadowed "closing floating window" "$((ux - 8))" "$((uy + 20))" "$((fx + 20))" "$((fy + 20))"
"$UMBRIEL" clock-advance 4000

# Pinned windows shadow each other.
spawn pin-low 300 200
spawn pin-high 300 200
capture
read -r ax ay _ _ < <(box_of pin-low)
read -r bx by _ _ < <(box_of pin-high)
shadowed "pinned over pinned" "$((bx - 8))" "$((by + 20))" "$((ax + 20))" "$((ay + 20))"

# Scratchpad windows shadow each other.
spawn scratch-low 300 200
spawn scratch-high 300 200
"$UMBRIEL" msg scratchpad-toggle:shade > /dev/null
"$UMBRIEL" msg "window-focus:$(id_of scratch-high)" > /dev/null
capture
read -r sx sy _ _ < <(box_of scratch-low)
read -r tx ty _ _ < <(box_of scratch-high)
shadowed "scratchpad over scratchpad" "$((tx - 8))" "$((ty + 20))" "$((sx + 20))" "$((sy + 20))"

echo "shadows fall on lower floating, pinned, scratchpad, and closing-over windows, never across tiles"
