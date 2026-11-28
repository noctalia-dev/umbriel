#!/usr/bin/env bash
# A scratchpad that tiles its windows leaves the overview as one card image of that layout. Pad members are arranged
# by the pad's own workspace now, not by a manager-owned float, so the card must reproduce the tiles it replaced at
# progress 0, keep them visible mid-fade, and leave nothing behind. The pad covers the second workspace, so a card that
# fell back to a workspace index of its own would sit in the wrong preview and fail the first claim. The second opening
# runs without the pad and is the reference.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly OUT="$UMBRIEL_RUNTIME_DIR/pad-layout"
readonly OUTPUT_WIDTH=1280
readonly OUTPUT_HEIGHT=720

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = true

[animation.windows_in]
enabled = false

[animation.windows_out]
enabled = false

[animation.overview]
enabled = true
duration_ms = 5000
curve = "linear"

[animation.scratchpad]
enabled = true
dim = 0.5
blur = false

[colors]
backdrop = "#204060FF"

[colors.overview]
background_tint = "#00000000"
workspace_background = "#101820FF"

[appearance]
border_width = 2
outer_border_width = 2

[appearance.blur]
enabled = false

[overview]
zoom = 0.5
background_blur = false
shortcuts = false

[output."HEADLESS-1"]
workspaces = 2

[[scratchpad]]
name = "tiles"

[[window_rule]]
match.title = "^overview-pad-layout-"
default_scratchpad = "tiles"
EOF
"$UMBRIEL" msg config-reload > /dev/null

for index in a b c; do
  case $index in
    a) colour=0xFFFF0000 ;;
    b) colour=0xFF00FF00 ;;
    c) colour=0xFF0000FF ;;
  esac
  FILL_COLOR=$colour "$CLIENT" "overview-pad-layout-$index" 400 300 \
    > "$UMBRIEL_RUNTIME_DIR/pad-layout-$index.log" 2>&1 &
done
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -ge 3 ]] && break
  sleep 0.1
done

# The pad covers the output's second workspace.
"$UMBRIEL" msg workspace-switch 2 > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg scratchpad-toggle tiles > /dev/null
"$UMBRIEL" settle > /dev/null
# Layout actions act on the focused window's workspace, so focus a member and then tile the pad.
pad_id=$("$UMBRIEL" windows --json | jq -r --arg title overview-pad-layout-a '.[] | select(.title == $title) | .id')
"$UMBRIEL" msg window-focus "$pad_id" > /dev/null
"$UMBRIEL" msg workspace-set-layout dwindle > /dev/null
"$UMBRIEL" settle > /dev/null

grim "$OUT-closed.png"
# The tiles are measured on screen, not from the IPC geometry: a tiled pad fills the output with its three colours.
readonly RED_PREDICATE='r > 0.8 && g < 0.2 && b < 0.2'
readonly GREEN_PREDICATE='r < 0.2 && g > 0.8 && b < 0.2'
readonly BLUE_PREDICATE='r < 0.2 && g < 0.2 && b > 0.8'
tiles=()
for predicate in "$RED_PREDICATE" "$GREEN_PREDICATE" "$BLUE_PREDICATE"; do
  read -r x y w h < <("$UMBRIEL_PIXEL_PROBE" "$OUT-closed.png" bbox "$predicate")
  if ((w <= 0 || h <= 0)); then
    echo "one of the pad's windows is not on screen: $("$UMBRIEL" windows --json)"
    exit 1
  fi
  tiles+=("$x $y $w $h")
done
readonly OUTPUT_AREA=$((OUTPUT_WIDTH * OUTPUT_HEIGHT))
covered=0
for tile in "${tiles[@]}"; do
  read -r x y w h <<< "$tile"
  covered=$((covered + w * h))
done
if ((covered < OUTPUT_AREA * 6 / 10)); then
  echo "the pad did not tile its windows over the output: ${tiles[*]}"
  exit 1
fi
if ! jq -e '[.[] | select(.title | startswith("overview-pad-layout-")) | select(.floating == false and .visible)] | length == 3' \
    <<< "$("$UMBRIEL" windows --json)" > /dev/null; then
  echo "the pad's windows are not tiled members on screen: $("$UMBRIEL" windows --json)"
  exit 1
fi

# Mid-fade previews are three quarters of the output, so everything outside 160..1120 x 90..630 is off the preview.# Mid-fade previews are three quarters of the output, so everything outside 160..1120 x 90..630 is off the preview.
# The pad fills the output, so its scaled card ends on the preview's right edge and anything the clip failed to hold
# back, such as the shadow of its outermost tile, lands just past it.
readonly OUTSIDE_POINTS=("40 40" "1240 40" "40 680" "1240 680" "1130 200" "1130 500")

sample_rgb() {
  "$UMBRIEL_PIXEL_PROBE" "$1" mean "20x20+$(($2 - 10))+$(($3 - 10))"
}

# The card's point for a live output-relative point at `zoom`, in preview coordinates.
scaled_point() {
  local x=$1 y=$2 zoom=$3
  printf '%s %s\n' \
    "$(awk -v x="$x" -v z="$zoom" -v w="$OUTPUT_WIDTH" 'BEGIN { printf "%d", (w - w * z) / 2 + x * z }')" \
    "$(awk -v y="$y" -v z="$zoom" -v h="$OUTPUT_HEIGHT" 'BEGIN { printf "%d", (h - h * z) / 2 + y * z }')"
}

"$UMBRIEL" clock-freeze
"$UMBRIEL" msg overview-open > /dev/null
grim "$OUT-p00.png"
"$UMBRIEL" clock-advance 2500
grim "$OUT-p50.png"
"$UMBRIEL" clock-advance 2500
grim "$OUT-settled.png"

# The settled opening keeps the pad stored in its scratchpad and off screen.
if ! jq -e '[.[] | select(.title | startswith("overview-pad-layout-"))
    | select(.scratchpad == "tiles" and .visible == false)] | length == 3' \
    <<< "$("$UMBRIEL" windows --json)" > /dev/null; then
  echo "the settled overview did not keep the pad stored and hidden: $("$UMBRIEL" windows --json)"
  exit 1
fi

"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" clock-advance 2500
"$UMBRIEL" clock-advance 2500
"$UMBRIEL" clock-resume > /dev/null
"$UMBRIEL" settle > /dev/null

# The same two-workspace opening without the pad is the reference at each progress.
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" clock-advance 2500
grim "$OUT-ref50.png"

# A window that joins the pad while the overview is open has no card of its own: the pad stays dismissed, so the frame
# must match an opening that never saw it. Its own workspace is the pad's, and a card built from it would land in the
# strip.
FILL_COLOR=0xFFFF00FF "$CLIENT" overview-pad-layout-late 400 300 > "$UMBRIEL_RUNTIME_DIR/pad-layout-late.log" 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq '[.[] | select(.title == "overview-pad-layout-late")] | length') -ge 1 ]] && break
  sleep 0.1
done
if ! jq -e '[.[] | select(.title == "overview-pad-layout-late") | select(.scratchpad == "tiles" and .visible == false)] | length == 1' \
    <<< "$("$UMBRIEL" windows --json)" > /dev/null; then
  echo "the late window did not join the dismissed pad: $("$UMBRIEL" windows --json)"
  exit 1
fi
"$UMBRIEL" clock-advance 2500
grim "$OUT-refset.png"

# A third opening, with nothing new, is what the late window's opening must look like.
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" clock-advance 2500
"$UMBRIEL" clock-advance 2500
"$UMBRIEL" clock-resume > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" clock-advance 5000
grim "$OUT-clean.png"
read -r largest differing < <("$UMBRIEL_PIXEL_PROBE" "$OUT-refset.png" diff "$OUT-clean.png")
if ((largest > 2)); then
  echo "a window that joined the dismissed pad drew a card of its own: largest difference $largest"
  exit 1
fi

# At progress 0 the preview is the output: every tile must reproduce the live one, which only holds if the cards were
# built in the row the pad covered.
while read -r x y w h; do
  read -r largest differing < <(
    "$UMBRIEL_PIXEL_PROBE" "$OUT-p00.png" diff "$OUT-closed.png" "$((w - 4))x$((h - 4))+$((x + 2))+$((y + 2))"
  )
  if ((largest > 8)); then
    echo "the tiled pad card does not reproduce the live pad at $x,$y: largest difference $largest"
    exit 1
  fi
done < <(printf '%s\n' "${tiles[@]}")

# A tile lends its shadow to the layer under every tile, so a gap beside it reads darker than the bare backdrop. The
# card draws that shadow under itself; a pad card that dropped it leaves the gap at the backdrop.
for tile in "${tiles[@]}"; do
  read -r x y w h <<< "$tile"
  right=$((x + w))
  for other in "${tiles[@]}"; do
    read -r next_x next_y next_w next_h <<< "$other"
    if ((next_x <= right)); then
      continue
    fi
    if ((next_y >= y + h || y >= next_y + next_h)); then
      continue
    fi
    gap=$((next_x - right))
    if ((gap < 8 || gap > 40)); then
      continue
    fi
    strip_x=$((right + 4))
    strip_y=$((y + h / 4))
    strip_w=$((gap - 8))
    strip_h=$((h / 2))
    read -r card_red card_green card_blue <<< "$(sample_rgb "$OUT-p00.png" $((strip_x + strip_w / 2)) $((strip_y + strip_h / 2)))"
    read -r live_red live_green live_blue <<< "$(sample_rgb "$OUT-closed.png" $((strip_x + strip_w / 2)) $((strip_y + strip_h / 2)))"
    read -r largest differing < <(
      "$UMBRIEL_PIXEL_PROBE" "$OUT-p00.png" diff "$OUT-closed.png" "${strip_w}x${strip_h}+$strip_x+$strip_y"
    )
    if ((largest > 6 || card_blue - live_blue > 3)); then
      echo "the pad card dropped the shadow of the tile at $x,$y: largest difference $largest, card $card_red $card_green $card_blue against live $live_red $live_green $live_blue"
      exit 1
    fi
  done
done

# Halfway through, each tile is still on screen where its preview puts it, and reads nothing like the pad-free opening.
while read -r x y w h; do
  read -r point_x point_y <<< "$(scaled_point $((x + w / 2)) $((y + h / 2)) 0.75)"
  read -r pad_red pad_green pad_blue <<< "$(sample_rgb "$OUT-p50.png" "$point_x" "$point_y")"
  read -r ref_red ref_green ref_blue <<< "$(sample_rgb "$OUT-ref50.png" "$point_x" "$point_y")"
  if ((pad_red - ref_red < 40 && pad_green - ref_green < 40 && pad_blue - ref_blue < 40)); then
    echo "the tiled pad card left its preview before the opening ended at $point_x,$point_y: $pad_red $pad_green $pad_blue against $ref_red $ref_green $ref_blue"
    exit 1
  fi
done < <(printf '%s\n' "${tiles[@]}")

# Nothing of the pad may reach outside the preview it belongs to.
for point in "${OUTSIDE_POINTS[@]}"; do
  read -r x y <<< "$point"
  read -r largest differing < <(
    "$UMBRIEL_PIXEL_PROBE" "$OUT-p50.png" diff "$OUT-ref50.png" "20x20+$((x - 10))+$((y - 10))"
  )
  if ((largest > 2)); then
    echo "pad content leaked outside its preview at $point: largest difference $largest"
    exit 1
  fi
done

# Once the opening settles, the pad leaves no trace: its preview reads like the opening that never had one.
while read -r x y w h; do
  read -r point_x point_y <<< "$(scaled_point $((x + w / 2)) $((y + h / 2)) 0.5)"
  read -r largest differing < <(
    "$UMBRIEL_PIXEL_PROBE" "$OUT-settled.png" diff "$OUT-refset.png" "20x20+$((point_x - 10))+$((point_y - 10))"
  )
  if ((largest > 2)); then
    echo "the settled overview still showed the tiled pad at $point_x,$point_y: largest difference $largest"
    exit 1
  fi
done < <(printf '%s\n' "${tiles[@]}")

echo "a tiled scratchpad faded out as one card image inside the preview it covered"
