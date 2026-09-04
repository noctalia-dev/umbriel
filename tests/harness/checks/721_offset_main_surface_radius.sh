#!/usr/bin/env bash
# A window's rounding belongs to the window box, not to whichever buffer happens to draw there. This client puts its
# window geometry origin at a subsurface placed above and left of its main surface, so the main surface is inset inside
# the content box: rounding it as if its own quad were the content box cuts an arc out of the middle of the window.
# The client paints its parent surface red and the subsurface underneath it blue over a green backdrop, so one sample
# reports which surface drew a pixel and whether an arc was cut where no window corner is.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/tests/subsurface-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/offset-subsurface-client.log"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/offset-main-surface-radius.png"
readonly RADIUS=64
readonly OFFSET=40

if [[ ! -x $CLIENT ]]; then
  echo "subsurface client not built at $CLIENT"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<EOF

[animation]
duration_ms = 1

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = $RADIUS
backdrop_color = "#00FF00FF"

[appearance.shadow]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

OFFSET_GEOMETRY=$OFFSET "$CLIENT" "offset-radius" > "$CLIENT_LOG" 2>&1 &

for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "subsurface client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi

window_box() {
  "$UMBRIEL" windows --json | jq -r '.[] | select(.title == "offset-radius") | "\(.x) \(.y) \(.w) \(.h)"'
}
box=
for _ in $(seq 60); do
  box=$(window_box)
  [[ -n $box ]] && break
  sleep 0.1
done
if [[ -z $box ]]; then
  echo "offset-radius window never appeared in the window list"
  exit 1
fi
# Sample the box the layout settled on, after the (1 ms) open animation has run.
sleep 0.5
read -r win_x win_y win_w win_h <<< "$(window_box)"
grim "$SCREENSHOT"

sample() {
  magick "$SCREENSHOT" -crop "1x1+$1+$2" \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

# (OFFSET+3, OFFSET+3) from the window origin is the main surface's own top-left, ~86 px from an arc centre it must not
# have. Red is the main surface; blue is the subsurface it would uncover if that corner were rounded.
read -r inner_r inner_g inner_b <<< "$(sample $((win_x + OFFSET + 3)) $((win_y + OFFSET + 3)))"
if (( inner_r < 200 || inner_b > 60 )); then
  echo "an arc was cut out of the inset main surface: r=$inner_r g=$inner_g b=$inner_b"
  exit 1
fi

# The window's own corners still round. The subsurface owns the top-left, the main surface reaches the bottom-right, so
# a correctly rounded window leaves the backdrop visible at both.
read -r tl_r tl_g tl_b <<< "$(sample $((win_x + 3)) $((win_y + 3)))"
if (( tl_g < 200 || tl_r > 60 || tl_b > 60 )); then
  echo "window top-left corner was not rounded: r=$tl_r g=$tl_g b=$tl_b"
  exit 1
fi
read -r br_r br_g br_b <<< "$(sample $((win_x + win_w - 4)) $((win_y + win_h - 4)))"
if (( br_g < 200 || br_r > 60 || br_b > 60 )); then
  echo "window bottom-right corner was not rounded: r=$br_r g=$br_g b=$br_b"
  exit 1
fi

# Content is still drawn: rounding that swallowed the inset surface would pass the probes above on its own.
read -r mid_r mid_g mid_b <<< "$(sample $((win_x + win_w / 2)) $((win_y + win_h / 2)))"
if (( mid_r < 200 )); then
  echo "main surface missing at the window centre: r=$mid_r g=$mid_g b=$mid_b"
  exit 1
fi

echo "inset main surface kept its interior corners square while the window rounded: interior red=$inner_r blue=$inner_b, corners green=$tl_g/$br_g"
