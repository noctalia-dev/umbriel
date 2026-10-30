#!/usr/bin/env bash
# harness: outputs=1
# appearance.downsample picks how the scene samples a minified source. A client
# alternating per-texel columns, shrunk 3:1, makes the methods distinguishable
# by pixel: box averages the fragment's whole texel footprint and preserves the
# pattern's 2:1 coverage ratio per output pixel, while bilinear point-samples
# and invents an even 50/50 mix. The method is a scene policy, so flipping it
# on a reload must re-render.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/downsample-box-client.log"
readonly WINDOW_TITLE="downsample-box"

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
downsample = "box"
[appearance.blur]
enabled = false
[appearance.shadow]
enabled = false
[output.HEADLESS-1]
scale = 1.0
render_scale = 3.0

[[window_rule]]
match.title = "^downsample-box$"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 40, y = 40, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" downsample-box 400 300 > "$CLIENT_LOG" 2>&1 &
await_lines "$CLIENT_LOG" 'mapped ' 1
presented=$(grep '^mapped ' "$CLIENT_LOG" | tail -1)
if [[ $presented != *"buffer 1200x900"* || $presented != *"scale 360/120"* ]]; then
  echo "the client did not render at render_scale 3.0 ($presented)"
  exit 1
fi

WINDOW_ID=""
for _ in $(seq 80); do
  WINDOW_ID=$("$UMBRIEL" windows --json | jq -r --arg title "$WINDOW_TITLE" '.[] | select(.title == $title) | .id')
  [[ -n $WINDOW_ID ]] && break
  sleep 0.025
done
if [[ -z $WINDOW_ID ]]; then
  echo "no window id for '$WINDOW_TITLE'"
  exit 1
fi

IMAGE="$UMBRIEL_RUNTIME_DIR/downsample-box.png"
# Cap leg captures at output-raster resolution instead of logical.
GRIM=(grim -o HEADLESS-1)

pixel() {
  local px=$1 py=$2 red green blue
  read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$px" "$py")
  echo "$red $green $blue"
}

probe_matches() {
  local px=$1 py=$2 er=$3 eg=$4 eb=$5 red green blue
  read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$px" "$py")
  [[ $red =~ ^[0-9]+$ ]] || return 1
  ((red >= er - 2 && red <= er + 2 && green >= eg - 2 && green <= eg + 2 && blue >= eb - 2 && blue <= eb + 2))
}

# The window occupies output x 40..439, three buffer texels per output pixel.
# Output px 41 covers texels 3..5 (green, blue, green) and px 42 texels 6..8
# (blue, green, blue). Poll for the pair, then fail with what rendered: each
# argument list is g/b for px 41 then px 42 (red is 0).
expect_pair() {
  local description=$1 g1=$2 b1=$3 g2=$4 b2=$5
  for _ in $(seq 30); do
    "${GRIM[@]}" "$IMAGE"
    if probe_matches 41 190 0 "$g1" "$b1" && probe_matches 42 190 0 "$g2" "$b2"; then
      return 0
    fi
    sleep 0.1
  done
  echo "$description: got $(pixel 41 190) and $(pixel 42 190), expected 0 $g1 $b1 and 0 $g2 $b2"
  exit 1
}

# A toplevel capture draws the same buffer through the window's mirror scene,
# which carries its own downsample policy. The capture is the window at 1:1
# (400x300), so its px 41 and 42 hold the same texel groups as the output
# probes and the same values apply.
expect_toplevel_pair() {
  local description=$1 g1=$2 b1=$3 g2=$4 b2=$5
  for _ in $(seq 30); do
    grim -T "$WINDOW_ID" "$IMAGE"
    if probe_matches 41 190 0 "$g1" "$b1" && probe_matches 42 190 0 "$g2" "$b2"; then
      return 0
    fi
    sleep 0.1
  done
  echo "$description: got $(pixel 41 190) and $(pixel 42 190), expected 0 $g1 $b1 and 0 $g2 $b2"
  exit 1
}

# Box: output px 41 covers two green texels and one blue (0,170,85), px 42 the
# mirror (0,85,170) in the sRGB screenshot.
expect_pair "box downsample" 170 85 85 170
expect_toplevel_pair "box downsample in window captures" 170 85 85 170

sed -i 's/downsample = "box"/downsample = "bilinear"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
# Bilinear: one point sample per output pixel lands on a texel center and
# reports that texel pure, dropping the other two (aliasing).
expect_pair "bilinear downsample" 0 255 255 0 0
expect_toplevel_pair "bilinear downsample in window captures" 0 255 255 0 0

sed -i 's/downsample = "bilinear"/downsample = "box"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
expect_pair "box restored on reload" 170 85 85 170
expect_toplevel_pair "capture downsample restored on reload" 170 85 85 170

echo "downsample method controls minified sampling and reload re-renders"

# Cap: at 5:1 the four-tap grid must step ratio/K texels so it still spans the
# whole footprint (a one-texel step would average only 4 of the 5 texels and
# report an even mix). render_scale 4.0 on the scale-0.8 output draws the
# client's 1600-texel buffer into the window's 320-pixel raster (5:1), and the
# window's logical (40,40) lands at raster (32,32). Capture at raster
# resolution so probes read framebuffer pixels: px 41 and 42 cover 5-texel
# footprints whose 1.25-texel taps average 5:3 and 3:5 green mixes,
# (0,159,96) and (0,96,159); the one-texel-step mutation reaches only 4 of
# the 5 texels and reports the even mix (0,127,127) at every pixel.
mapped_before=$(grep -c '^mapped ' "$CLIENT_LOG")
sed -i 's/scale = 1.0/scale = 0.8/; s/render_scale = 3.0/render_scale = 4.0/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
await_lines "$CLIENT_LOG" 'mapped ' "$((mapped_before + 1))" 0.05
presented=$(grep '^mapped ' "$CLIENT_LOG" | tail -1)
if [[ $presented != *"buffer 1600x1200"* ]]; then
  echo "the client did not render at render_scale 4.0 ($presented)"
  exit 1
fi
"$UMBRIEL" settle > /dev/null
IMAGE="$UMBRIEL_RUNTIME_DIR/downsample-box-cap.png"
GRIM=(grim -s 0.8 -o HEADLESS-1)
"${GRIM[@]}" "$IMAGE"
# The 5:1 pin assumes the raster rect (32,32,320,240): assert its black edges
# instead of trusting the scale math.
if [[ $(pixel 31 190) != "0 0 0" || $(pixel 352 190) != "0 0 0" || $(pixel 41 31) != "0 0 0" ||
  $(pixel 41 272) != "0 0 0" || $(pixel 32 190) == "0 0 0" || $(pixel 351 190) == "0 0 0" ||
  $(pixel 41 32) == "0 0 0" || $(pixel 41 271) == "0 0 0" ]]; then
  echo "window raster rect is not (32,32,320,240): x31..32 $(pixel 31 190)/$(pixel 32 190), x351..352 $(pixel 351 190)/$(pixel 352 190), y31..32 $(pixel 41 31)/$(pixel 41 32), y271..272 $(pixel 41 271)/$(pixel 41 272)"
  exit 1
fi
expect_pair "capped box spans the footprint" 159 96 96 159

echo "capped box grid spans the full minification footprint"
