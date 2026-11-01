#!/usr/bin/env bash
# harness: outputs=1
# An overview card drag that edge scrolls the workspace rows and then drops back on the active workspace leaves the
# rows resting on that workspace's preview, not where the edge scroll stopped between rows.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720

cat > "$UMBRIEL_CONFIG" <<EOF
[general]
xwayland = false
show_cheatsheet = false
autostart = []

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
shortcuts = false
zoom = 0.5

[output."HEADLESS-1"]
workspaces = 3
workspace_axis = "vertical"
EOF
"$UMBRIEL" msg config-reload > /dev/null

foot --title=card-settle sh -c 'sleep 120' > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.25
done
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
start_x=$(jq -r '.[0] | (.x + .w / 2 | round)' <<< "$windows")
start_y=$(jq -r '.[0] | (.y + .h / 2 | round)' <<< "$windows")
press_x=$((320 + start_x / 2))
press_y=$((180 + start_y / 2))

# Mean brightness of a patch near the bottom of the active preview's card: the edge scroll lifts the card off it.
brightness() {
  grim "$UMBRIEL_RUNTIME_DIR/$1.png"
  magick "$UMBRIEL_RUNTIME_DIR/$1.png" -crop 20x20+630+490 -format '%[fx:round(255*mean)]' info:
}

"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 640 360
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
before=$(brightness before)

# Park at the bottom edge long enough to slide the rows part of a step (delay_ms 100, about 1.9 rows/s), then drop
# over the active preview, which stays under y=200 at any offset reached.
pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  move "$press_x" "$press_y" press "$BTN_LEFT" move $((press_x + 30)) "$press_y" \
  -- move 640 719 mark atedge hold move "$press_x" 200 release "$BTN_LEFT"
pointer_step atedge
sleep 0.4 # real time: the edge scroll runs on a wall-clock timer
pointer_release
"$UMBRIEL" settle

workspace=$("$UMBRIEL" windows --json | jq -r '.[0].workspace')
[[ $workspace == *":1" ]] || {
  echo "card left the active workspace: $workspace"
  exit 1
}
after=$(brightness after)
((after >= before - 3 && after <= before + 3)) || {
  echo "rows did not settle on the active workspace after the drop: brightness $before -> $after"
  exit 1
}

echo "edge-scrolled rows settled on the active workspace after a same-workspace drop (brightness $before -> $after)"
