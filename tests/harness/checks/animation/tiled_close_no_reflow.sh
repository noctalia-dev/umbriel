#!/usr/bin/env bash
# A closing tiled snapshot with no live layout reflow keeps its captured geometry for the full windows_out timeline.
# windows_move must not reshape a stationary-neighbour or lone-window lifecycle snapshot.
set -euo pipefail

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/tiled-close-no-reflow.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[layout]
mode = "scrolling"

[layout.scrolling]
default_extent_fraction = 0.5
center_focused = "never"
center_underfull_strip = false

[animation.windows_in]
enabled = false

[animation.windows_out]
enabled = true
duration_ms = 2000
curve = "linear"
style = "fade"

[animation.windows_move]
enabled = true
duration_ms = 150
curve = "linear"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn() {
  local title=$1 color=$2 width=${3:-600}
  FILL_COLOR="$color" "$UMBRIEL_UNMAP_CLIENT" "$title" "$width" 700 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)')
    [[ -n $window ]] && return 0
    sleep 0.025
  done
  echo "timed out waiting for $title"
  return 1
}

sample() {
  local x=$1 y=$2
  magick "$IMAGE" -crop "8x8+$((x - 4))+$((y - 4))" \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]\n' info:
}

is_blue() {
  local red=$1 green=$2 blue=$3
  ((blue > 50 && red < 30 && green < 30))
}

is_red() {
  local red=$1 green=$2 blue=$3
  ((red > 50 && green < 30 && blue < 30))
}

is_black() {
  local red=$1 green=$2 blue=$3
  ((red < 30 && green < 30 && blue < 30))
}

spawn tiled-close-survivor 0xFFFF0000
"$UMBRIEL" settle
spawn tiled-close-static 0xFF0000FF
closing=$window
"$UMBRIEL" settle

# Animation time only moves by clock-advance: samples land 800 ms into each 2000 ms windows_out timeline.
"$UMBRIEL" clock-freeze

id=$(jq -r .id <<< "$closing")
x=$(jq -r .x <<< "$closing")
y=$(jq -r .y <<< "$closing")
w=$(jq -r .w <<< "$closing")
h=$(jq -r .h <<< "$closing")
"$UMBRIEL" msg "window-close:$id" > /dev/null
for _ in $(seq 80); do
  grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/tiled-close-static.log" && break
  sleep 0.025
done
if ! grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/tiled-close-static.log"; then
  echo "closing tile did not unmap"
  exit 1
fi
for _ in $(seq 80); do
  if ! "$UMBRIEL" windows --json | jq -e 'any(.[]; .title == "tiled-close-static")' > /dev/null; then
    break
  fi
  sleep 0.025
done
if "$UMBRIEL" windows --json | jq -e 'any(.[]; .title == "tiled-close-static")' > /dev/null; then
  echo "compositor did not observe the closing tile unmap"
  exit 1
fi

near_x=$((x + w / 4))
far_x=$((x + 3 * w / 4))
mid_y=$((y + h / 2))

"$UMBRIEL" clock-advance 800
grim "$IMAGE"
read -r near_red near_green near_blue < <(sample "$near_x" "$mid_y")
read -r far_red far_green far_blue < <(sample "$far_x" "$mid_y")
if ! is_blue "$near_red" "$near_green" "$near_blue"; then
  echo "no-reflow close snapshot was not alive at its near side: $near_red $near_green $near_blue"
  exit 1
fi
if ! is_blue "$far_red" "$far_green" "$far_blue"; then
  echo "windows_move reshaped a no-reflow close snapshot: $far_red $far_green $far_blue"
  exit 1
fi

"$UMBRIEL" clock-advance 2000
"$UMBRIEL" settle
grim "$IMAGE"
read -r near_red near_green near_blue < <(sample "$near_x" "$mid_y")
read -r far_red far_green far_blue < <(sample "$far_x" "$mid_y")
if ! is_black "$near_red" "$near_green" "$near_blue" \
    || ! is_black "$far_red" "$far_green" "$far_blue"; then
  echo "no-reflow close snapshot remained after windows_out"
  exit 1
fi

# Close the remaining tile as well. This is the reported final-window case, with no neighbour at all.
lone=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "tiled-close-survivor")')
if [[ -z $lone ]]; then
  echo "surviving tile disappeared before the lone close phase"
  exit 1
fi
id=$(jq -r .id <<< "$lone")
x=$(jq -r .x <<< "$lone")
y=$(jq -r .y <<< "$lone")
w=$(jq -r .w <<< "$lone")
h=$(jq -r .h <<< "$lone")
"$UMBRIEL" msg "window-close:$id" > /dev/null
for _ in $(seq 80); do
  grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/tiled-close-survivor.log" && break
  sleep 0.025
done
if ! grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/tiled-close-survivor.log"; then
  echo "lone closing tile did not unmap"
  exit 1
fi
for _ in $(seq 80); do
  if ! "$UMBRIEL" windows --json | jq -e 'any(.[]; .title == "tiled-close-survivor")' > /dev/null; then
    break
  fi
  sleep 0.025
done
if "$UMBRIEL" windows --json | jq -e 'any(.[]; .title == "tiled-close-survivor")' > /dev/null; then
  echo "compositor did not observe the lone closing tile unmap"
  exit 1
fi

near_x=$((x + w / 4))
far_x=$((x + 3 * w / 4))
mid_y=$((y + h / 2))

"$UMBRIEL" clock-advance 800
grim "$IMAGE"
read -r near_red near_green near_blue < <(sample "$near_x" "$mid_y")
read -r far_red far_green far_blue < <(sample "$far_x" "$mid_y")
if ! is_red "$near_red" "$near_green" "$near_blue" \
    || ! is_red "$far_red" "$far_green" "$far_blue"; then
  echo "windows_move reshaped the lone close snapshot: near=$near_red $near_green $near_blue, far=$far_red $far_green $far_blue"
  exit 1
fi

"$UMBRIEL" clock-advance 2000
"$UMBRIEL" settle
grim "$IMAGE"
read -r near_red near_green near_blue < <(sample "$near_x" "$mid_y")
read -r far_red far_green far_blue < <(sample "$far_x" "$mid_y")
if ! is_black "$near_red" "$near_green" "$near_blue" \
    || ! is_black "$far_red" "$far_green" "$far_blue"; then
  echo "lone close snapshot remained after windows_out"
  exit 1
fi

echo "stationary-neighbour and lone tiled closes kept their captured geometry through windows_out"
