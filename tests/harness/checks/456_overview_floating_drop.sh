#!/usr/bin/env bash
# A floating card dropped in the overview stays where it was released, and the window keeps that position once the
# overview closes.
set -euo pipefail

readonly BTN_LEFT=272
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/drop.png"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"

[overview]
zoom = 0.5

[appearance.shadow]
enabled = false

[[window_rule]]
match.title = "^floater$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

position() { "$UMBRIEL" windows --json | jq -r '.[] | select(.title == "floater") | "\(.x) \(.y)"'; }

"$UMBRIEL_UNMAP_CLIENT" floater 300 200 > "$UMBRIEL_RUNTIME_DIR/floater.log" 2>&1 &
for _ in $(seq 80); do
  [[ $(position) == "100 100" ]] && break
  sleep 0.025
done
if [[ $(position) != "100 100" ]]; then
  echo "floater never opened at 100,100: $(position)"
  exit 1
fi

blue_at() { magick "$IMAGE" -crop "4x4+$1+$2" -format '%[fx:round(mean.b*255)]\n' info:; }

# The 1280x720 output previews at half size from 320,180, so the card spans 370,230 to 520,330. Dragging it by
# 100,50 on screen moves the window by 200,100.
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
"$POINTER" 1280 720 move 445 280 press "$BTN_LEFT" move 495 305 move 545 330 pause 300 release "$BTN_LEFT"
"$UMBRIEL" settle
grim "$IMAGE"
dropped=$(blue_at 600 370)
vacated=$(blue_at 380 240)
if ((dropped < 100 || vacated > 20)); then
  echo "the card returned to its old place after the drop: dropped spot blue $dropped, vacated spot blue $vacated"
  exit 1
fi

"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" settle
if [[ $(position) != "300 200" ]]; then
  echo "the window did not keep its dropped position: $(position)"
  exit 1
fi

echo "the dropped floating card stayed at its release point (blue $dropped) and the window kept it"
