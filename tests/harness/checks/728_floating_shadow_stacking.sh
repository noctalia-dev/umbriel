#!/usr/bin/env bash
# A resting floating window casts its shadow over whatever it covers, a tile or another floating window, whether it
# opened floating or reached the floating layer by toggling. The shadow used to sit in one layer below every tile, so
# it was hidden by whatever the window covered and came back only while a drag lifted the window into the drag layer.
# Re-tiling has to put it back: a tile must not shadow the tile beside it. Bright green makes the shadow readable
# against the clients underneath.
set -euo pipefail

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/shadow.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[colors]
shadow = "#00FF00FF"

[layout]
gap = 0

[layout.scrolling]
default_extent_fraction = 1.0

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = true
softness = 24
offset_x = 0
offset_y = 0

[[window_rule]]
match.app_id = "^floater$"
default_floating = true
default_position = { x = 900, y = 40, anchor = "top_left" }
default_floating_size_px = { width = 300, height = 200 }

[[window_rule]]
match.app_id = "^upper$"
default_floating = true
default_position = { x = 950, y = 140, anchor = "top_left" }
default_floating_size_px = { width = 300, height = 200 }
EOF
"$UMBRIEL" msg config-reload > /dev/null

window_of() {
  "$UMBRIEL" windows --json | jq -c --arg id "$1" '.[] | select(.app_id == $id)'
}

spawn() {
  foot --app-id="$1" --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
  for _ in $(seq 100); do
    window=$(window_of "$1")
    [[ -n $window ]] && return 0
    sleep 0.05
  done
  echo "window '$1' never appeared"
  return 1
}

box_of() {
  window=$(window_of "$1")
  echo "$(jq -r .x <<< "$window") $(jq -r .y <<< "$window") $(jq -r .w <<< "$window") $(jq -r .h <<< "$window")"
}

# Grab until two consecutive frames match, so a sample never lands on a frame the compositor is still assembling.
shot_settled() {
  local previous=$UMBRIEL_RUNTIME_DIR/.settle.png
  grim "$previous"
  for _ in $(seq 24); do
    sleep 0.25
    grim "$IMAGE"
    cmp -s "$previous" "$IMAGE" && return 0
    mv "$IMAGE" "$previous"
  done
  echo "the output never settled"
  return 1
}

green_at() {
  magick "$IMAGE" -crop "8x8+$1+$2" -format '%[fx:round(mean.g*255)]\n' info:
}

# Mean green just below the bottom edge of the given window, where its shadow falls on the tile underneath.
shadow_below() {
  local x y w h
  read -r x y w h < <(box_of "$1")
  green_at "$((x + w / 2))" "$((y + h + 4))"
}

# One tiled window wide enough to sit under everything else.
spawn backdrop
spawn floater
floater=$(jq -r .id <<< "$window")
shot_settled

opened=$(shadow_below floater)
if ((opened < 40)); then
  echo "a window that opened floating casts no shadow over the tile below it: green $opened"
  exit 1
fi

# A floating window covering another one shadows it too: the pair is ordered window over shadow over window.
spawn upper
upper=$(jq -r .id <<< "$window")
shot_settled
read -r ux uy uw uh < <(box_of upper)
read -r fx fy fw fh < <(box_of floater)
peer=$(green_at "$((ux - 6))" "$((uy + 20))")
if ((peer < 40)); then
  echo "a floating window casts no shadow over the floating window it covers: green $peer"
  exit 1
fi

# The window underneath keeps its own shadow on the tile below it, sampled clear of the window covering it.
under=$(green_at "$((fx + 20))" "$((fy + fh + 4))")
if ((under < 40)); then
  echo "the covered floating window lost its own shadow: green $under"
  exit 1
fi

# Raising the covered window reverses the pair: its shadow has to travel with it, and the window that was on top
# stops shadowing it.
"$UMBRIEL" msg "window-focus:$floater" > /dev/null
shot_settled
reversed=$(green_at "$((fx + fw + 6))" "$((uy + 40))")
if ((reversed < 40)); then
  echo "the raised window did not bring its shadow with it: green $reversed"
  exit 1
fi
stale=$(green_at "$((ux - 6))" "$((uy + 20))")
if ((stale > 50)); then
  echo "the lowered window still shadows the window raised above it: green $stale"
  exit 1
fi

# A window that reaches the floating layer by toggling has to arrive there with its shadow.
"$UMBRIEL" msg "window-focus:$upper" > /dev/null
spawn toggler
toggler=$(jq -r .id <<< "$window")
"$UMBRIEL" msg "window-toggle-floating:$toggler" > /dev/null
"$UMBRIEL" msg window-set-primary-extent:0.4 > /dev/null
"$UMBRIEL" msg window-set-secondary-extent:0.4 > /dev/null
shot_settled

toggled=$(shadow_below toggler)
if ((toggled < 40)); then
  echo "a window toggled floating casts no shadow over the tile below it: green $toggled"
  exit 1
fi

# Re-tiling returns it to the tiled layer, where a window never shadows the tile beside it.
"$UMBRIEL" msg "window-toggle-floating:$toggler" > /dev/null
"$UMBRIEL" msg window-set-primary-extent:0.5 > /dev/null
shot_settled
read -r tx ty tw th < <(box_of toggler)
read -r bx by bw bh < <(box_of backdrop)
seam=$((tx < bx ? bx + 6 : bx + bw - 14))
spill=$(green_at "$seam" "$((by + bh / 2))")
if ((spill > 50)); then
  echo "a re-tiled window still shadows the tile beside it: green $spill at $seam"
  exit 1
fi

echo "floating shadows render over the tile below when opened ($opened) and when toggled ($toggled), over a covered floating window ($peer), and leave the neighbour clean once re-tiled ($spill)"
