#!/usr/bin/env bash
# harness: outputs=2
# Moving a window between outputs while overview is open must move its preview
# into the destination output tree. IPC geometry alone cannot see a stale card.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"
readonly SOURCE_SHOT="$UMBRIEL_RUNTIME_DIR/overview-output-source.png"
readonly TARGET_SHOT="$UMBRIEL_RUNTIME_DIR/overview-output-target.png"

output_x() {
  "$UMBRIEL" outputs | awk -v name="$1" '$1 == name {found = 1; next} found && /Position:/ {split($2, p, ","); print p[1]; exit}'
}

sample_center() {
  magick "$1" -crop 40x40+620+340 -colorspace RGB \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

assert_blue_card() {
  local label=$1 image=$2 red green blue
  read -r red green blue <<< "$(sample_center "$image")"
  if (( blue < 100 || blue < red + 40 )); then
    echo "$label overview card is missing at the output center: red=$red green=$green blue=$blue"
    return 1
  fi
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
animation_ms = 100
border_width = 0
outer_border_width = 0
corner_radius = 0
backdrop_color = "#000000FF"

[appearance.blur]
enabled = false

[overview]
zoom = 0.5
background_tint = "#000000FF"
workspace_background = "#000000FF"

[[window_rule]]
match.app_id = "^overview-output-move$"
default_output = "HEADLESS-1"
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

"$UMBRIEL" msg overview-open > /dev/null
sleep 0.3
grim -o "$source" "$SOURCE_SHOT"
assert_blue_card source "$SOURCE_SHOT"

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

window_x=$(jq -r '.x' <<< "$window")
window_w=$(jq -r '.w' <<< "$window")
if (( window_x < target_x || window_x + window_w > target_x + 1280 )); then
  echo "window geometry is outside $target after the move: $window"
  exit 1
fi

sleep 0.3
grim -o "$target" "$TARGET_SHOT"
assert_blue_card destination "$TARGET_SHOT"

echo "overview card followed its window from $source to $target"
