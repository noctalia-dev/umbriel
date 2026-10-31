#!/usr/bin/env bash
# harness: outputs=2
# Window physics displaces a dragged sheet across the shared output edge (each
# output draws its half). The deform keeps running after release, so the sheet
# still hangs over the boundary — its far half must keep drawing on the
# neighbour until the deform settles, or the window's own ring is cut off at
# the boundary the moment the move ends. Pinned twice: a drop on the home
# output, and a drop on the neighbour output whose release TRANSFERS workspace
# (the transfer's park + re-home are the release's resting re-homes too).
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
hnheld=0; hn16=0; hn64=0; gr16=0; gr64=0; hn320=0; hnsettled=0
bheld=0; b16=0; b64=0; bg16=0; bg64=0; bsettled=0

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "floating"

[colors]
backdrop = "#000000FF"
[colors.border]
focused = "#FF0000FF"
[appearance]
border_width = 8
outer_border_width = 0
corner_radius = 0
drag_opacity = 1.0
[appearance.shadow]
enabled = false
[animation]
duration_ms = 1
[animation.windows_in]
enabled = false
[animation.windows_out]
enabled = false
[animation.windows_drag]
physics = true
[[window_rule]]
match.title = "^ring-cut$"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
[effects]
border = "ring"
window = "window"
[effects.preset.ring]
kind = "border"
shader = "ring.glsl"
padding = 4
[effects.preset.window]
kind = "window"
shader = "window.glsl"
EOF
cat > "$UMBRIEL_RUNTIME_DIR/ring.glsl" <<'GLSL'
vec4 border(vec2 uv) { return vec4(1.0, 0.0, 0.0, 1.0); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/window.glsl" <<'GLSL'
vec4 window(vec2 uv) { return umbriel_sample(uv); }
GLSL
"$UMBRIEL" msg config-reload > /dev/null

FILL_COLOR=0xFF00FF00 "$UMBRIEL_UNMAP_CLIENT" ring-cut 400 300 > "$UMBRIEL_RUNTIME_DIR/ring-cut.log" 2>&1 &
for _ in $(seq 80); do
  [[ -n $("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "ring-cut")') ]] && break
  sleep 0.025
done
"$UMBRIEL" settle > /dev/null

home=$("$UMBRIEL" windows --json | jq -r '.[0].workspace' | cut -d: -f1)
if [[ $home == HEADLESS-1 ]]; then neighbour=HEADLESS-2; else neighbour=HEADLESS-1; fi
read -r wx wy ww wh < <(
  "$UMBRIEL" windows --json | jq -r '.[0] | "\(.x) \(.y) \(.w) \(.h)"'
)
# Park the window straddling the shared edge: 200px on each side of x=1280.
gx=$((wx + 40))
gy=$((wy + 40))
dx=$((1080 - wx))
pointer_hold 2560 720 move "$gx" "$gy" mod super press "$BTN_LEFT" \
  move $((gx + dx / 3)) "$gy" \
  move $((gx + 2 * dx / 3)) "$gy" \
  move $((gx + dx)) "$gy" -- release "$BTN_LEFT" mod none

stats() { # $1 = label: red/green counts near the shared edge on both outputs
  grim -o "$home" "$UMBRIEL_RUNTIME_DIR/$1-home.png"
  grim -o "$neighbour" "$UMBRIEL_RUNTIME_DIR/$1-neighbour.png"
  local hr hn gr gn
  hr=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/$1-home.png" count 'r > 0.8 && g < 0.2 && b < 0.2' "120x720+1160+0")
  hn=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/$1-neighbour.png" count 'r > 0.8 && g < 0.2 && b < 0.2' "120x720+0+0")
  gr=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/$1-home.png" count 'g > 0.8 && r < 0.2 && b < 0.2' "120x720+1160+0")
  gn=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/$1-neighbour.png" count 'g > 0.8 && r < 0.2 && b < 0.2' "120x720+0+0")
  echo "$1 home-edge red=$hr green=$gr | neighbour-edge red=$hn green=$gn"
  case $1 in
    held) hnheld=$hn ;;
    r16) hn16=$hn; gr16=$gr ;;
    r64) hn64=$hn; gr64=$gr ;;
    r320) hn320=$hn ;;
    settled) hnsettled=$hn ;;
    bheld) bheld=$hr ;;
    b16) b16=$hr; bg16=$gn ;;
    b64) b64=$hr; bg64=$gn ;;
    bsettled) bsettled=$hr ;;
  esac
}

"$UMBRIEL" clock-freeze
"$UMBRIEL" clock-advance 16 > /dev/null
stats held
pointer_release
"$UMBRIEL" clock-advance 16 > /dev/null
stats r16
"$UMBRIEL" clock-advance 48 > /dev/null
stats r64
"$UMBRIEL" clock-advance 256 > /dev/null
stats r320
"$UMBRIEL" clock-advance 768 > /dev/null
stats settled
"$UMBRIEL" clock-resume

# The scenario must have engaged: while held, the ring's far half draws on the
# neighbour, and the sheet is still measurable at r16. Without these floors the
# assertions below pass vacuously.
if ((hnheld < 100)); then
  echo "scenario never engaged: no ring on the neighbour while held ($hnheld)"
  exit 1
fi
if ((gr16 < 10000)) || ((gr64 < 10000)); then
  echo "unmeasurable: home samples too small (gr16=$gr16 gr64=$gr64), a cut would pass silently"
  exit 1
fi

# While the released sheet is still displaced across the shared edge (its home
# half still drawn), the neighbour must keep drawing the far half. Re-homing the
# view under the home output's clipped root the moment the move ends cuts the
# deformed sheet's ring off at the boundary. Pinned at both displaced samples:
# a re-home anywhere before the settle must fail here.
if ((gr16 > 10000)) && ((hn16 < 100)); then
  echo "the release cut the deformed sheet at the shared edge (neighbour red: held=$hnheld r16=$hn16 r64=$hn64)"
  exit 1
fi
if ((gr64 > 10000)) && ((hn64 < 100)); then
  echo "the release cut the deformed sheet mid-settle (neighbour red: r64=$hn64)"
  exit 1
fi

# And the home clip must come back once the deform settled: a view left under
# the unclipped drag tree forever would keep drawing past the edge.
if ((hnsettled >= 100)); then
  echo "the home clip never applied after settle (neighbour red: r320=$hn320 settled=$hnsettled)"
  exit 1
fi
echo "a released sheet keeps drawing across the shared edge until the deform settles"

# Second release — the drop lands on the NEIGHBOUR output, so it TRANSFERS
# workspace: removeView parks the view under the server tree and addView
# re-homes it onto the destination layer. Both are the release's resting
# re-home and must wait for the settle just the same; a transfer that lands
# immediately cuts the trailing sheet on the home side of the edge.
pointer_hold 2560 720 move 1120 "$gy" mod super press "$BTN_LEFT" \
  move 1180 "$gy" \
  move 1250 "$gy" \
  move 1320 "$gy" -- release "$BTN_LEFT" mod none
"$UMBRIEL" clock-freeze
"$UMBRIEL" clock-advance 16 > /dev/null
stats bheld
pointer_release
"$UMBRIEL" clock-advance 16 > /dev/null
stats b16
"$UMBRIEL" clock-advance 48 > /dev/null
stats b64
"$UMBRIEL" clock-advance 1024 > /dev/null
stats bsettled
"$UMBRIEL" clock-resume

if ((bheld < 100)); then
  echo "transfer scenario never engaged: no ring on the home side while held ($bheld)"
  exit 1
fi
if ((bg16 < 10000)) || ((bg64 < 10000)); then
  echo "unmeasurable: transfer displacement samples too small (bg16=$bg16 bg64=$bg64)"
  exit 1
fi
if ((bg16 > 10000)) && ((b16 < 100)); then
  echo "the transfer cut the deformed sheet at the shared edge (home red: held=$bheld r16=$b16 r64=$b64)"
  exit 1
fi
if ((bg64 > 10000)) && ((b64 < 100)); then
  echo "the transfer cut the deformed sheet mid-settle (home red: r64=$b64)"
  exit 1
fi
if ((bsettled >= 100)); then
  echo "the destination clip never applied after the transfer settle (home red: $bsettled)"
  exit 1
fi
echo "a transferred sheet keeps drawing across the shared edge until the deform settles"
