#!/usr/bin/env bash
set -euo pipefail

workspace_axis=${workspace_axis:-vertical}
cat >> "$UMBRIEL_CONFIG" <<EOF

[animation]
enabled = true

[animation.overview]
duration_ms = 2000

[output."HEADLESS-1"]
workspace_axis = "$workspace_axis"

[overview]
background_blur = false
shortcuts = false
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL_UNMAP_CLIENT" touchpad-settle 1200 700 > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq length) == 1 ]] && break
  sleep 0.05
done
[[ $("$UMBRIEL" windows --json | jq length) == 1 ]]
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 640 360
"$UMBRIEL" msg overview-open > /dev/null
sleep 2.1

for travel in 70 210; do
  "$UMBRIEL_POINTER_CLIENT" 1280 720 axis "$workspace_axis" "$travel" pause 400 axis-stop "$workspace_axis"
  grim "$UMBRIEL_RUNTIME_DIR/early-$travel.png"
  sleep 0.7
  grim "$UMBRIEL_RUNTIME_DIR/settled-$travel.png"
  difference=$(magick "$UMBRIEL_RUNTIME_DIR/early-$travel.png" "$UMBRIEL_RUNTIME_DIR/settled-$travel.png" \
    -compose difference -composite -format '%[fx:mean]' info:)
  awk -v difference="$difference" 'BEGIN { exit !(difference > 0.002) }' || {
    echo "release snapped instead of animating: travel=$travel difference=$difference"
    exit 1
  }
  expected=1
  [[ $travel == 210 ]] && expected=2
  [[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .index') == "$expected" ]]
  sleep 0.7
  grim "$UMBRIEL_RUNTIME_DIR/late-$travel.png"
  difference=$(magick "$UMBRIEL_RUNTIME_DIR/settled-$travel.png" "$UMBRIEL_RUNTIME_DIR/late-$travel.png" \
    -compose difference -composite -format '%[fx:mean]' info:)
  awk -v difference="$difference" 'BEGIN { exit !(difference < 0.0001) }' || {
    echo "finished spring was overwritten by the overview easing: difference=$difference"
    exit 1
  }
done

echo 'workspace switch and snap-back retain visible motion after release'
