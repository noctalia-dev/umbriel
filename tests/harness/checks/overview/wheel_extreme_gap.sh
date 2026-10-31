#!/usr/bin/env bash
# harness: outputs=1
# Regression check (talia review findings 3+4 on the overview card drag input): the filmstrip
# end-band widening in workspaceAtPoint is DROP-ONLY. Wheel notches — and the pointer-scroll and
# navigation lookups sharing the same hit-test flags — keep the narrow preview hitbox, so a
# Shift-wheel notch over the extreme gap beyond the first preview stays inert instead of focusing
# the end row's columns as the widened drop hitbox would.
# Asserts: the notch over the gap changes nothing, the same notch inside the row's band moves column
# focus (proving the notch driver and direction).
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly OUTPUT_W=1280
readonly OUTPUT_H=720

spawn_client() {
  foot --title="wheel-gap-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

selected_title() {
  "$UMBRIEL" windows --json | jq -r '.[] | select(.focused == true) | .title'
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output."HEADLESS-1"]
workspaces = 3

[layout.scrolling]
default_extent_fraction = 0.5
center_focused = "never"

[overview]
background_blur = false
shortcuts = false
zoom = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

for id in 1 2; do
  spawn_client "$id"
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $id ]] && break
    sleep 0.25
  done
done
"$UMBRIEL" msg column-focus-first > /dev/null
"$UMBRIEL" settle

"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 640 360
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle

before=$(selected_title)

# The extreme gap above the first row's band (rows start at y=180 at zoom 0.5): the notch must be
# inert. A widened drop hitbox would resolve the end row and step its column focus here.
"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 640 100 mod shift notch 1 mod none
"$UMBRIEL" settle
after_gap=$(selected_title)

if [[ $after_gap != "$before" ]]; then
  echo "RED: a wheel notch over the extreme gap focused the end row's columns: $before -> $after_gap"
  exit 1
fi

# Control: the same notch inside the row's band does step column focus.
"$UMBRIEL_POINTER_CLIENT" "$OUTPUT_W" "$OUTPUT_H" move 640 360 mod shift notch 1 mod none
for _ in $(seq 20); do
  after_band=$(selected_title)
  [[ $after_band != "$before" ]] && break
  sleep 0.1
done

if [[ $after_band == "$before" ]]; then
  echo "the control notch inside the row band did not move column focus (driver broken?): $before"
  exit 1
fi

echo "GREEN: wheel notch over the extreme gap stayed inert ($before), inside the band it moved focus ($after_band)"
