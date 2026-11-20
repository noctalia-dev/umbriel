#!/usr/bin/env bash
# A visible scratchpad fades out inside the workspace preview it covered, over the overview's own opening. The pad is
# drawn as a card there, so at progress 0 it is pixel-identical to the same window on the workspace, its dim darkens the
# rest of that preview, and nothing of it is left outside the preview where the filmstrip would not cover it. The
# second opening runs without the pad and is the reference for all three claims.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly WINDOW_TITLE=overview-pad-fade-window
readonly PAD_TITLE=overview-pad-fade-pad
readonly OUT="$UMBRIEL_RUNTIME_DIR/pad-fade"

# Progress 0.5 of a linear 5000 ms opening leaves the preview at three quarters of the output: everything outside
# 160..1120 x 90..630 is outside the preview and must equal a frame where no pad ever existed. The pad sits flush with
# the output's right edge, so its scaled card ends on the preview's right edge and any shadow the clip failed to hold
# back lands just past it.
readonly OUTSIDE_POINTS=("40 40" "1130 300" "1130 440" "40 680")
# Inside the preview: the first point is over the workspace window's card, which the pad's dim covers too, and the
# second is bare preview desktop.
readonly PREVIEW_POINTS=("400 100" "880 600")

sample_rgb() {
  "$UMBRIEL_PIXEL_PROBE" "$1" mean "20x20+$(($2 - 10))+$(($3 - 10))"
}

assert_dimmed() {
  local label=$1 image=$2 x=$3 y=$4 reference=$5 delta=$6
  local red green blue grey_red grey_green grey_blue
  read -r red green blue <<< "$(sample_rgb "$image" "$x" "$y")"
  read -r grey_red grey_green grey_blue <<< "$(sample_rgb "$reference" "$x" "$y")"
  if ((grey_blue - blue < delta)); then
    echo "$label is not dimmed: $image reads $blue against $grey_blue in $reference"
    return 1
  fi
}

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
corner_radius = 8

[appearance.blur]
enabled = false

[overview]
zoom = 0.5
background_blur = false
shortcuts = false

[[window_rule]]
match.title = "^overview-pad-fade-pad$"
default_floating = true
default_floating_size_px = { width = 420, height = 260 }
default_position = { x = 860, y = 230, anchor = "top_left" }

[[window_rule]]
match.title = "^overview-pad-fade-window$"
default_floating = true
default_floating_size_px = { width = 360, height = 240 }
default_position = { x = 60, y = 60, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

FILL_COLOR=0xFF996633 "$CLIENT" "$WINDOW_TITLE" 360 240 > "$UMBRIEL_RUNTIME_DIR/pad-fade-window.log" 2>&1 &
FILL_COLOR=0xFF0000FF "$CLIENT" "$PAD_TITLE" 420 260 > "$UMBRIEL_RUNTIME_DIR/pad-fade-pad.log" 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -ge 2 ]] && break
  sleep 0.1
done

pad_id=$("$UMBRIEL" windows --json | jq -r --arg title "$PAD_TITLE" '.[] | select(.title == $title) | .id')
if [[ -z $pad_id ]]; then
  echo "pad client never mapped: $("$UMBRIEL" windows --json)"
  exit 1
fi
"$UMBRIEL" msg window-focus "$pad_id" > /dev/null
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
for _ in $(seq 60); do
  [[ -z $("$UMBRIEL" windows --json | jq -r --arg title "$PAD_TITLE" '.[] | select(.title == $title) | .workspace') ]] && break
  sleep 0.1
done
if [[ -n $("$UMBRIEL" windows --json | jq -r --arg title "$PAD_TITLE" '.[] | select(.title == $title) | .workspace') ]]; then
  echo "pad window never left its workspace: $("$UMBRIEL" windows --json)"
  exit 1
fi
# Focus the workspace window first: a shown pad owns its output, so any focus change behind it hides the pad. The pad
# is therefore shown last, and stays the one holding the seat.
window_id=$("$UMBRIEL" windows --json | jq -r --arg title "$WINDOW_TITLE" '.[] | select(.title == $title) | .id')
"$UMBRIEL" msg window-focus "$window_id" > /dev/null
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" settle > /dev/null
grim "$OUT-closed.png"

"$UMBRIEL" clock-freeze
"$UMBRIEL" msg overview-open > /dev/null
grim "$OUT-p00.png"
"$UMBRIEL" clock-advance 2500
grim "$OUT-p50.png"
"$UMBRIEL" clock-advance 2000
grim "$OUT-p90.png"

# The opening settles with the pad dismissed: the window stays in its pad and off screen.
"$UMBRIEL" clock-advance 500
grim "$OUT-settled.png"
if ! "$UMBRIEL" windows --json | jq -e --arg title "$PAD_TITLE" \
    'any(.[] | select(.title == $title); .visible == false and .scratchpad == "default" and .workspace == "")' > /dev/null; then
  echo "the settled overview did not keep the pad stored and hidden: $("$UMBRIEL" windows --json)"
  exit 1
fi

# The pad's dim belonged to the opening. Stop the closing zoom halfway: it rests at the same three-quarter preview as
# the reference below, and a dim the settle left behind would climb back with the closing progress.
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" clock-advance 2500
grim "$OUT-closing.png"
"$UMBRIEL" clock-advance 2500
"$UMBRIEL" settle > /dev/null

# A second opening, now without a pad, is the reference for every comparison below.
"$UMBRIEL" msg overview-open > /dev/null
grim "$OUT-ref00.png"
"$UMBRIEL" clock-advance 2500
grim "$OUT-ref50.png"
"$UMBRIEL" clock-advance 2000
grim "$OUT-ref90.png"
"$UMBRIEL" clock-advance 500
grim "$OUT-refsettled.png"

# At progress 0 the preview is the output and the pad's card must reproduce the live pad exactly, so the swap from the
# desktop to the filmstrip changes no pixel. The pad sits at 860,230 and is 420x260.
read -r largest differing < <(
  "$UMBRIEL_PIXEL_PROBE" "$OUT-p00.png" diff "$OUT-closed.png" "418x258+861+231"
)
if ((largest > 8 || differing > 200)); then
  echo "pad card does not reproduce the live pad at progress 0: largest difference $largest over $differing pixels"
  exit 1
fi
# The card's shadow is a scaled reconstruction of the live one, so it is compared over the band the shadow reaches with
# a tolerance that still catches a shadow faded ahead of the card or missing altogether.
read -r largest differing < <(
  "$UMBRIEL_PIXEL_PROBE" "$OUT-p00.png" diff "$OUT-closed.png" "430x290+850+215"
)
if ((largest > 20)); then
  echo "pad card shadow does not follow the card at progress 0: largest difference $largest"
  exit 1
fi

# The pad's dim darkens the rest of its preview, so the pad's own opening frame is darker there than the reference.
for point in "${PREVIEW_POINTS[@]}"; do
  read -r x y <<< "$point"
  assert_dimmed "preview desktop at $point" "$OUT-p00.png" "$x" "$y" "$OUT-ref00.png" 10
done
# The dim rides the same clock: at half progress a quarter of it is left, so the preview keeps three quarters of its
# colour where the reference has all of it.
read -r dim_red dim_green dim_blue <<< "$(sample_rgb "$OUT-p50.png" 880 600)"
read -r dimref_red dimref_green dimref_blue <<< "$(sample_rgb "$OUT-ref50.png" 880 600)"
if ((dim_blue * 4 < dimref_blue * 3 - 6 || dim_blue * 4 > dimref_blue * 3 + 6)); then
  echo "the pad's dim did not follow the opening: $dim_blue against $dimref_blue without it"
  exit 1
fi
# The border is one image with the content: at half alpha the ring reads as its own colour blended half into the
# preview, not as the opaque ring the pad drew at rest. The ring runs 856..859 on the desktop and 802..805 at half
# progress; the outermost pixel of each is compared.
read -r ring_red ring_green ring_blue <<< "$("$UMBRIEL_PIXEL_PROBE" "$OUT-p00.png" pixel 856 360)"
read -r fade_red fade_green fade_blue <<< "$("$UMBRIEL_PIXEL_PROBE" "$OUT-p50.png" pixel 802 360)"
read -r edge_red edge_green edge_blue <<< "$("$UMBRIEL_PIXEL_PROBE" "$OUT-p50.png" pixel 760 360)"
ring_mid=$(((ring_blue + edge_blue) / 2))
if ((fade_blue < ring_mid - 5 || fade_blue > ring_mid + 5)); then
  echo "the pad card's border did not fade with its content: $fade_blue against $ring_mid (ring $ring_blue, preview $edge_blue)"
  exit 1
fi

# The card fades on the opening's clock, not on a clock of its own. Halfway through a linear 5000 ms opening its blue
# is exactly halfway between its own and the preview behind it; a fade that runs faster or slower reads elsewhere.
read -r pad_red pad_green pad_blue <<< "$(sample_rgb "$OUT-p50.png" 1070 360)"
read -r under_red under_green under_blue <<< "$(sample_rgb "$OUT-p50.png" 760 360)"
read -r start_red start_green start_blue <<< "$(sample_rgb "$OUT-p00.png" 1070 360)"
mid=$(((start_blue + under_blue) / 2))
if ((pad_blue < mid - 6 || pad_blue > mid + 6)); then
  echo "pad card did not fade on the opening's clock: $pad_blue against $mid (full $start_blue, preview $under_blue)"
  exit 1
fi
# The tail of the fade is still running at 90%: a fade that ended early leaves nothing of the card here.
read -r tail_red tail_green tail_blue <<< "$(sample_rgb "$OUT-p90.png" 876 360)"
read -r under90_red under90_green under90_blue <<< "$(sample_rgb "$OUT-p90.png" 400 500)"
tail=$((tail_blue - under90_blue))
if ((tail < 12 || tail > 40)); then
  echo "pad card did not follow the opening to its end: $tail over the preview at 90%, from $under90_blue"
  exit 1
fi

# The card must scale with its preview, not sit at the desktop's size. The pad covers 860..1280 x 230..490 on the
# desktop; halfway through it is three quarters of that, so these points are inside the live pad but outside its card,
# and must read as the preview's desktop.
read -r card_red card_green card_blue <<< "$(sample_rgb "$OUT-p50.png" 1070 360)"
for y in 240 480; do
  read -r edge_red edge_green edge_blue <<< "$(sample_rgb "$OUT-p50.png" 1070 "$y")"
  if ((card_blue - edge_blue < 40)); then
    echo "the pad card did not scale with its preview at y=$y: $edge_blue against $card_blue inside the card"
    exit 1
  fi
done

# Nothing of the pad may reach outside its preview: there the frame must equal the reference exactly.
for point in "${OUTSIDE_POINTS[@]}"; do
  read -r x y <<< "$point"
  read -r largest differing < <("$UMBRIEL_PIXEL_PROBE" "$OUT-p50.png" diff "$OUT-ref50.png" "20x20+$((x - 10))+$((y - 10))")
  if ((largest > 2)); then
    echo "pad content leaked outside its preview at $point: largest difference $largest"
    exit 1
  fi
done

# Both halfway frames rest at the same three-quarter preview, so they must agree: the closing one still carries the pad
# state from the first opening, and any dim left behind would have darkened it again. A pad card the settle failed to
# drop would fade back in at 900,360, inside the footprint the card covered before the settle.
"$UMBRIEL" clock-resume > /dev/null
for point in "${PREVIEW_POINTS[@]}" "900 360"; do
  read -r x y <<< "$point"
  read -r largest differing < <("$UMBRIEL_PIXEL_PROBE" "$OUT-closing.png" diff "$OUT-ref50.png" "20x20+$((x - 10))+$((y - 10))")
  if ((largest > 2)); then
    echo "the pad returned during the closing zoom at $point: largest difference $largest"
    exit 1
  fi
done

# The settled opening holds no trace of the pad: its card and its dim are gone, so the preview matches the opening
# that never had one. At progress 1 the preview is 320..960 x 180..540 and the card would have sat at 750..960 x
# 295..425.
for region in "210x130+750+295" "40x40+890+500"; do
  read -r largest differing < <("$UMBRIEL_PIXEL_PROBE" "$OUT-settled.png" diff "$OUT-refsettled.png" "$region")
  if ((largest > 2)); then
    echo "the settled overview still showed the pad at $region: largest difference $largest"
    exit 1
  fi
done

echo "the scratchpad faded inside its workspace preview and left nothing outside it"
