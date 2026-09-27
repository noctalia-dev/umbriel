#!/usr/bin/env bash
# A border preset paints the focused window's ring and its padding, leaves the client hole alone, follows focus, can be
# switched off per window, freezes with the animation clock, spills light onto a neighbour, asks for frames only
# while its clock advances, and stays on an overview card's close snapshot.
set -euo pipefail

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/effect-border.png"
cat > "$UMBRIEL_RUNTIME_DIR/ring.glsl" <<'GLSL'
// Solid red over the whole drawn rectangle; the hole is cut out by the compositor. The green term is invisible at
// 8 bits but keeps umbriel_time an active uniform, so the program counts as time-reading.
vec4 border(vec2 uv) { return vec4(1.0, 0.001 * sin(umbriel_time), 0.0, 1.0); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/still.glsl" <<'GLSL'
vec4 border(vec2 uv) { return vec4(1.0, 0.0, 0.0, 1.0); }
GLSL
readonly BASE="$UMBRIEL_RUNTIME_DIR/effect-border-base.toml"
cp "$UMBRIEL_CONFIG" "$BASE"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
[appearance]
border_width = 4
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[colors]
backdrop = "#000000FF"
[effects]
border = "ring"
[effects.preset.ring]
kind = "border"
shader = "ring.glsl"
padding = 20
[[window_rule]]
match.title = "^effect-one$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
[[window_rule]]
match.title = "^effect-two$"
default_floating = true
default_position = { x = 700, y = 100, anchor = "top_left" }
[[window_rule]]
match.title = "^effect-plain$"
default_floating = true
default_position = { x = 100, y = 400, anchor = "top_left" }
border_effect = "off"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn() {
  FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" "$1" 300 200 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)')
    [[ -n $window ]] && break
    sleep 0.025
  done
  [[ -n $window ]]
}
red_at() { "$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r > 0.9 && g < 0.1 && b < 0.1' "$1"; }

spawn effect-one
"$UMBRIEL" settle > /dev/null
read -r x y w h id < <(jq -r '"\(.x) \(.y) \(.w) \(.h) \(.id)"' <<< "$window")
grim "$IMAGE"
# The ring (4 px) plus padding (20 px) paints red; sample a 2x2 patch inside the padding, 12 px above the client.
if (( $(red_at "2x2+$((x + w / 2))+$((y - 12))") < 4 )); then
  echo "border effect did not paint the padding above the focused window"
  exit 1
fi
# The client hole shows the blue client, not the effect.
if (( $("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'b > 0.9 && r < 0.1' "2x2+$((x + w / 2))+$((y + h / 2))") < 4 )); then
  echo "border effect leaked into the client hole"
  exit 1
fi

# Focus moves the effect: the first window loses it, the second gains it.
spawn effect-two
"$UMBRIEL" settle > /dev/null
read -r x2 y2 w2 _ id2 < <(jq -r '"\(.x) \(.y) \(.w) \(.h) \(.id)"' <<< "$window")
grim "$IMAGE"
if (( $(red_at "2x2+$((x2 + w2 / 2))+$((y2 - 12))") < 4 )); then
  echo "border effect did not follow focus to the second window"
  exit 1
fi
if (( $(red_at "2x2+$((x + w / 2))+$((y - 12))") > 0 )); then
  echo "border effect stayed on the unfocused window"
  exit 1
fi

# border_effect = "off" on a rule keeps the plain ring.
spawn effect-plain
"$UMBRIEL" settle > /dev/null
read -r x3 y3 w3 _ _ < <(jq -r '"\(.x) \(.y) \(.w) \(.h) \(.id)"' <<< "$window")
grim "$IMAGE"
if (( $(red_at "2x2+$((x3 + w3 / 2))+$((y3 - 12))") > 0 )); then
  echo "border_effect = off did not disable the default on the plain window"
  exit 1
fi
"$UMBRIEL" msg "window-focus:$id2" > /dev/null
"$UMBRIEL" settle > /dev/null

# Frames: a time-reading program requests effect-only frames while the clock advances, none once frozen.
frames() {
  "$UMBRIEL" effect-frames --json | jq -er '.outputs[0].effect_frames' || {
    echo "effect-frames reported no .outputs[0].effect_frames" >&2
    return 1
  }
}
before=$(frames)
sleep 0.3 # real time: effect-only frames arrive on the output's own timer
after=$(frames)
if (( after <= before )); then
  echo "an advancing time-reading border effect requested no effect-only frames"
  exit 1
fi
"$UMBRIEL" clock-freeze
"$UMBRIEL" settle > /dev/null
before=$(frames)
sleep 0.3 # real time: a frozen clock must produce no effect-only frames
after=$(frames)
if (( after != before )); then
  echo "a frozen clock still produced effect-only frames: $before -> $after"
  exit 1
fi
"$UMBRIEL" clock-resume
before=$(frames)
sleep 0.3 # real time: resuming the clock restarts effect-only frames
after=$(frames)
if (( after <= before )); then
  echo "resuming the clock did not restart effect-only frames"
  exit 1
fi

# animated = false and speed = 0 each stop frames while the clock runs.
for variant in 'animated = false' 'speed = 0'; do
  cat "$BASE" > "$UMBRIEL_CONFIG"
  cat >> "$UMBRIEL_CONFIG" <<EOF

[animation]
enabled = false
[appearance]
border_width = 4
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[effects]
border = "ring"
[effects.preset.ring]
kind = "border"
shader = "ring.glsl"
$variant
EOF
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" settle > /dev/null
  before=$(frames)
  sleep 0.3 # real time: a stopped clock must produce no effect-only frames
  after=$(frames)
  if (( after != before )); then
    echo "$variant still produced effect-only frames"
    exit 1
  fi
done

# Light: a preset with light spills red past its padding over a neighbouring window placed just below the ring.
near_y=$((y + h + 20))
cat "$BASE" > "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<EOF

[animation]
enabled = false
[appearance]
border_width = 4
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[colors]
backdrop = "#000000FF"
[effects]
border = "lit"
[effects.preset.lit]
kind = "border"
shader = "still.glsl"
padding = 10
[effects.preset.lit.light]
spread = 40
intensity = 4
threshold = 0.2
[[window_rule]]
match.title = "^effect-(one|two|plain)$"
default_floating = true
[[window_rule]]
match.title = "^effect-near$"
default_floating = true
default_position = { x = $x, y = $near_y, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
spawn effect-near
"$UMBRIEL" settle > /dev/null
read -r near_id near_top < <(jq -r '"\(.id) \(.y)"' <<< "$window")
"$UMBRIEL" msg "window-focus:$id" > /dev/null
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
# The lit ring box (4 px ring, 1 px raster margin, 10 px padding) ends 15 px below the client; the sample sits inside
# the neighbour's blue client, beyond it.
sample_y=$((near_top + 4))
if (( sample_y <= y + h + 15 )); then
  echo "the neighbour window overlaps the lit ring: its client starts at $near_top"
  exit 1
fi
read -r r _ b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$((x + w / 2))" "$sample_y")
if (( b < 200 )); then
  echo "the light sample is not over the neighbouring window: blue=$b"
  exit 1
fi
if (( r < 15 )); then
  echo "border light did not spill over the neighbouring window: red=$r"
  exit 1
fi
"$UMBRIEL" msg "window-close:$near_id" > /dev/null
"$UMBRIEL" settle > /dev/null

# An overview card closed while focused keeps the ring effect on its close snapshot: the red preset still paints the
# top of the card's ring on the first snapshot frame instead of the plain green ring.
cat "$BASE" > "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = true
[animation.windows_in]
enabled = false
[animation.windows_move]
enabled = false
[animation.windows_out]
enabled = true
style = "fade"
duration_ms = 5000
curve = "linear"
[appearance]
border_width = 4
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[colors]
backdrop = "#000000FF"
[colors.border]
focused = "#00FF00FF"
[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"
[overview]
zoom = 0.5
[effects]
border = "ring"
[effects.preset.ring]
kind = "border"
shader = "still.glsl"
padding = 20
[[window_rule]]
match.title = "^effect-(one|two|plain)$"
default_floating = true
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg "window-focus:$id" > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
read -r card_x card_y card_w _ < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox 'r > 0.9 && g < 0.1 && b < 0.1')
if (( card_w == 0 )); then
  echo "the focused overview card did not show the border effect"
  exit 1
fi
# A reload that doubles the padding widens the open card's ring by the scaled difference (20 px at zoom 0.5 = 10 px).
sed -i 's/^padding = 20$/padding = 40/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
padded_y=$card_y
read -r card_x card_y card_w _ < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox 'r > 0.9 && g < 0.1 && b < 0.1')
if (( card_y > padded_y - 8 )); then
  echo "the open overview card kept its ring padding across a reload: top $padded_y -> $card_y"
  exit 1
fi
readonly CARD_TOP="${card_w}x6+${card_x}+${card_y}"
card_red=$(red_at "$CARD_TOP")
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg "window-close:$id" > /dev/null
# Settle refuses while the frozen close animation runs, so poll the window list for the snapshot taking over.
for _ in $(seq 100); do
  [[ -z $("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "effect-one")') ]] && break
  sleep 0.02
done
if [[ -n $("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "effect-one")') ]]; then
  echo "the window never left the window list after window-close"
  exit 1
fi
"$UMBRIEL" clock-advance 1
grim "$IMAGE"
snapshot_red=$(red_at "$CARD_TOP")
if (( snapshot_red * 2 < card_red )); then
  echo "the overview card's close snapshot dropped the border effect: red $card_red -> $snapshot_red"
  exit 1
fi
"$UMBRIEL" clock-advance 5000
"$UMBRIEL" clock-resume
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" settle > /dev/null
echo "border effect padding, hole, focus, off override, frame gating, light, and overview close snapshots verified"
