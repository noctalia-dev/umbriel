#!/usr/bin/env bash
# Opaque cards fade during an overview drag. Client transparency composes with
# the drag multiplier instead of bypassing the compositor-owned opacity.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720

measure_drag_green() {
  local alpha=$1 title=$2
  local screenshot="$UMBRIEL_RUNTIME_DIR/$title.png"
  local client_pid
  foot --config=/dev/null --override=colors.background=000000 --override="colors.alpha=$alpha" \
    --title="$title" sh -c 'sleep 120' > /dev/null 2>&1 &
  client_pid=$!
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
    sleep 0.25
  done
  if [[ $("$UMBRIEL" windows --json | jq 'length') -ne 1 ]]; then
    echo "timed out waiting for $title" >&2
    return 1
  fi

  "$UMBRIEL" msg overview-open > /dev/null
  "$UMBRIEL" settle
  # The single card is centered at (640, 360). Move it right and hold the button so the compositor retains the grab.
  # Animation time only moves by clock-advance while the drag is sampled.
  "$UMBRIEL" clock-freeze > /dev/null
  pointer_hold "$OUTPUT_W" "$OUTPUT_H" move 640 360 press "$BTN_LEFT" move 740 360 -- release "$BTN_LEFT" >&2
  "$UMBRIEL" clock-advance 500 > /dev/null
  grim "$screenshot"

  local green
  # An opaque card at the configured 0.5 drag opacity over pure green leaves 255 * 0.5 = 128 in the encoded pixel.
  green=$(magick "$screenshot" -crop 40x40+680+430 -format '%[fx:round(255*mean.g)]' info:)
  pointer_release >&2
  "$UMBRIEL" clock-resume > /dev/null
  "$UMBRIEL" msg overview-close > /dev/null
  # Both measurements share one compositor instance, so the card from this scenario has to be gone before the next one
  # opens the overview on a single card again.
  kill -KILL "$client_pid" 2>/dev/null || true
  wait "$client_pid" 2>/dev/null || true
  for _ in $(seq 40); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq 0 ]] && break
    sleep 0.1
  done
  printf '%s\n' "$green"
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
insert_hint = "#FF0000FF"

[colors.overview]
background_tint = "#000000FF"
workspace_background = "#00FF00FF"

[appearance]
drag_opacity = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

opaque_green=$(measure_drag_green 1 drag-opaque)
if (( opaque_green < 110 || opaque_green > 145 )); then
  echo "opaque dragged card did not use configured opacity: mean green=$opaque_green"
  exit 1
fi

transparent_green=$(measure_drag_green 0.5 drag-transparent)
if (( transparent_green < 170 || transparent_green > 210 )); then
  echo "client alpha did not compose with configured drag opacity: opaque=$opaque_green transparent=$transparent_green"
  exit 1
fi

echo "drag opacity composed with client alpha: opaque=$opaque_green transparent=$transparent_green"
