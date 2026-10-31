#!/usr/bin/env bash
# harness: outputs=1
# Regression check (talia review findings 1+2 on the overview card drag input): while a card drag is
# held, the layout-strip edge auto-scroll must slide the strip of the ROW UNDER THE DRAG POINTER (the
# drop target), not the active workspace's — a corner push must reveal the hovered preview's offscreen
# columns, the same row the finger-pan path resolves through navigationWorkspace(). Pins the lookup
# against a regression to group->active().
# Asserts: while the held card rides at the strip edge over the second row's band, that row's columns
# slide (>50 px) and the active row's columns stay put, then the drop lands on the hovered row.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720

spawn_client() {
  foot --title="card-row-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

window_x() {
  jq -r --arg title "$1" '.[] | select(.title == $title) | .x' <<< "$2"
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output."HEADLESS-1"]
workspaces = 2

[layout.scrolling]
default_extent_fraction = 0.5
center_focused = "never"

[overview]
background_blur = false
shortcuts = false
zoom = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

count=0
for ws in 1 2; do
  "$UMBRIEL" msg "workspace-switch:$ws" > /dev/null
  for id in $(seq 1 4); do
    spawn_client "$ws-$id"
    count=$((count + 1))
    for _ in $(seq 60); do
      [[ $("$UMBRIEL" windows --json | jq 'length') -eq $count ]] && break
      sleep 0.25
    done
  done
done
"$UMBRIEL" msg column-focus-first > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" msg workspace-switch:1 > /dev/null
"$UMBRIEL" msg column-focus-first > /dev/null
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
if (( $(window_x card-row-2-4 "$windows") < OUTPUT_W )); then
  echo "workspace 2's last column began inside its viewport instead of offscreen: $windows"
  exit 1
fi

"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 640 360
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle

# The card comes from the active row (workspace 1). At zoom 0.5 rows step 396 px: the pointer at
# y=650 rides inside workspace 2's band (576..936) and outside the filmstrip's bottom trigger zone
# (y>690), while x=1279 is inside the strip's right edge zone.
start_x=$(jq -r '.[] | select(.title == "card-row-1-1") | (.x + .w / 2 | round)' <<< "$windows")
start_y=$(jq -r '.[] | select(.title == "card-row-1-1") | (.y + .h / 2 | round)' <<< "$windows")
press_x=$((320 + start_x / 2))
press_y=$((180 + start_y / 2))

pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  move "$press_x" "$press_y" press "$BTN_LEFT" move $((press_x + 20)) "$press_y" \
  -- move 1279 650 mark atedge hold release "$BTN_LEFT"

windows=$("$UMBRIEL" windows --json)
base_hover_x=$(window_x card-row-2-4 "$windows")
base_active_x=$(window_x card-row-1-4 "$windows")
echo "probe: post-detach baselines hovered row card-row-2-4 x=$base_hover_x, active row card-row-1-4 x=$base_active_x"

pointer_step atedge

slid=false
hover_x=$base_hover_x
for _ in $(seq 120); do
  windows=$("$UMBRIEL" windows --json)
  hover_x=$(window_x card-row-2-4 "$windows")
  if ((hover_x < base_hover_x - 50)); then
    slid=true
    break
  fi
  sleep 0.05
done

active_x=$(window_x card-row-1-4 "$windows")
pointer_release
"$UMBRIEL" settle

if [[ $slid != true ]]; then
  echo "RED: the edge scroll did not slide the hovered row's strip: card-row-2-4 x $base_hover_x -> $hover_x (active row moved x $base_active_x -> $active_x)"
  exit 1
fi

if ((active_x > base_active_x + 10 || active_x < base_active_x - 10)); then
  echo "RED: the edge scroll panned the ACTIVE row while the card hovered another: card-row-1-4 x $base_active_x -> $active_x"
  exit 1
fi

windows=$("$UMBRIEL" windows --json)
dropped=$(jq -r '.[] | select(.title == "card-row-1-1") | .workspace' <<< "$windows")
if [[ $dropped != *":2" ]]; then
  echo "the drop did not land on the hovered row: $dropped ($windows)"
  exit 1
fi

echo "GREEN: edge scroll slid the hovered row (card-row-2-4 x $base_hover_x -> $hover_x) with the active row still (x $base_active_x), drop on $dropped"
