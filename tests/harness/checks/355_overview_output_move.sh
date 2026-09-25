#!/usr/bin/env bash
# harness: outputs=2
# Moving a window between outputs while overview is open must move its preview
# into the destination output tree. IPC geometry alone cannot see a stale card.
# The two outputs arrange their workspaces on opposite axes, so the move also has
# to hand the window to a layout that scrolls perpendicular to the one it left.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly SOURCE_SHOT="$UMBRIEL_RUNTIME_DIR/overview-output-source.png"
readonly TARGET_SHOT="$UMBRIEL_RUNTIME_DIR/overview-output-target.png"

output_field() {
  "$UMBRIEL" outputs |
    awk -v name="$1" -v field="$2" '$1 == name {found = 1; next} found && /Position:/ {split($2, p, ","); print p[field]; exit}'
}

output_x() {
  output_field "$1" 1
}

output_y() {
  output_field "$1" 2
}

# Samples a 40x40 patch centered on the given point of an output-local screenshot.
sample_at() {
  magick "$1" -crop "40x40+$(($2 - 20))+$(($3 - 20))" \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

assert_blue_card() {
  local label=$1 image=$2 x=$3 y=$4 red green blue
  read -r red green blue <<< "$(sample_at "$image" "$x" "$y")"
  if (( blue < 100 || blue < red + 40 )); then
    echo "$label overview card is missing at $x,$y: red=$red green=$green blue=$blue"
    return 1
  fi
}

assert_no_card() {
  local label=$1 image=$2 x=$3 y=$4 red green blue
  read -r red green blue <<< "$(sample_at "$image" "$x" "$y")"
  if (( blue >= 100 )); then
    echo "$label overview card is still present at $x,$y: red=$red green=$green blue=$blue"
    return 1
  fi
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#000000FF"

[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"

[animation]
duration_ms = 100
curve = "linear"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.blur]
enabled = false

[overview]
zoom = 0.5

# Opposite workspace axes: the source scrolls its columns horizontally, the destination stacks them vertically. The
# second destination workspace makes the sideways step between its previews observable.
[output."HEADLESS-1"]
workspace_axis = "vertical"

[output."HEADLESS-2"]
workspaces = 2
workspace_axis = "horizontal"

[[window_rule]]
match.app_id = "^overview-output-move$"
default_output = "HEADLESS-1"

[[window_rule]]
match.app_id = "^overview-output-lane$"
default_output = "HEADLESS-2"
EOF
"$UMBRIEL" msg config-reload > /dev/null

source=HEADLESS-1
target=HEADLESS-2
source_x=$(output_x "$source")
target_x=$(output_x "$target")
if [[ -z $source_x || -z $target_x || $source_x == "$target_x" ]]; then
  echo "two distinct horizontal output positions are required: source=$source_x target=$target_x"
  exit 1
fi

"$POINTER" 2560 720 move "$((source_x + 640))" 360
APP_ID=overview-output-move "$CLIENT" overview-output-move 1200 700 > "$UMBRIEL_RUNTIME_DIR/overview-output-client.log" 2>&1 &

workspace=
for _ in $(seq 60); do
  workspace=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "overview-output-move") | .workspace')
  [[ $workspace == "$source":* ]] && break
  sleep 0.1
done
if [[ $workspace != "$source":* ]]; then
  echo "client did not map on $source: $("$UMBRIEL" windows --json)"
  exit 1
fi

APP_ID=overview-output-lane "$CLIENT" overview-output-lane 1200 700 > "$UMBRIEL_RUNTIME_DIR/overview-output-lane.log" 2>&1 &
for _ in $(seq 60); do
  workspace=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "overview-output-lane") | .workspace')
  [[ $workspace == "$target":* ]] && break
  sleep 0.1
done
if [[ $workspace != "$target":* ]]; then
  echo "lane client did not map on $target: $("$UMBRIEL" windows --json)"
  exit 1
fi
moved_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "overview-output-move") | .id')
"$UMBRIEL" msg "window-focus:$moved_id" > /dev/null
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq -r '.[] | select(.active) | .title') == overview-output-move ]] && break
  sleep 0.05
done

"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
grim -o "$source" "$SOURCE_SHOT"
assert_blue_card source "$SOURCE_SHOT" 640 360

if (( target_x > source_x )); then
  action=window-move-to-output-right
else
  action=window-move-to-output-left
fi
"$UMBRIEL" msg "$action" > /dev/null

for _ in $(seq 60); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "overview-output-move")')
  workspace=$(jq -r '.workspace' <<< "$window")
  [[ $workspace == "$target":* ]] && break
  sleep 0.1
done
if [[ $workspace != "$target":* ]]; then
  echo "window state did not move to $target: $("$UMBRIEL" windows --json)"
  exit 1
fi

target_y=$(output_y "$target")
window_x=$(jq -r '.x' <<< "$window")
window_w=$(jq -r '.w' <<< "$window")
if (( window_x < target_x || window_x + window_w > target_x + 1280 )); then
  echo "window geometry is outside $target after the move: $window"
  exit 1
fi

# Horizontally arranged workspaces scroll vertically, so the destination's two lanes stack along Y at a common X.
# On the source output's axis the same pair would sit side by side.
read -r lane_x lane_y moved_x moved_y <<< "$("$UMBRIEL" windows --json | jq -r \
  '(.[] | select(.title == "overview-output-lane")) as $lane | (.[] | select(.title == "overview-output-move")) as $moved
   | "\($lane.x) \($lane.y) \($moved.x) \($moved.y)"')"
lane_dy=$((moved_y - lane_y))
(( lane_dy < 0 )) && lane_dy=$((-lane_dy))
if (( lane_x != moved_x || lane_dy < 200 )); then
  echo "lanes on $target did not stack vertically: lane=$lane_x,$lane_y moved=$moved_x,$moved_y"
  exit 1
fi

"$UMBRIEL" settle
# 1280x720 at zoom 0.5: the active preview spans 320,180 to 960,540 in output-local coordinates. Sample inside the
# moved card, offset from its origin so the workspace badge in the preview corner cannot answer for it.
card_x=$((320 + (moved_x - target_x) / 2 + 200))
card_y=$((180 + (moved_y - target_y) / 2 + 120))
grim -o "$target" "$TARGET_SHOT"
assert_blue_card destination "$TARGET_SHOT" "$card_x" "$card_y"
grim -o "$source" "$SOURCE_SHOT"
assert_no_card source "$SOURCE_SHOT" 640 360

# Selecting the destination's second workspace slides its previews sideways, because that output arranges workspaces
# along X. The card of the workspace left behind must appear left of the active preview, not above or below it.
"$UMBRIEL" msg "workspace-switch:2/$target" > /dev/null
"$UMBRIEL" settle
grim -o "$target" "$TARGET_SHOT"
assert_no_card "destination active preview" "$TARGET_SHOT" "$card_x" "$card_y"
# Preview step: a 640 px preview plus a 0.1 * 1280 * 0.5 gap, so the previous preview slides 704 px to the left and
# only its right edge stays on the output. The full-width lane still covers this point at the card's own height.
assert_blue_card "destination previous preview" "$TARGET_SHOT" 120 "$card_y"

echo "overview card followed its window from $source to $target and adopted that output's workspace axis"
