#!/usr/bin/env bash
# A window preset rewrites a translucent window's pixels in place: at rest it sees the desktop through the window,
# inside an opening capture it shades window content over the live desktop, backdrop sampling returns after the
# capture ends, it survives the close snapshot including on an overview card, window_effect overrides or disables
# the default per rule regardless of which window holds focus, and a border preset's overlay tints only the focused
# bordered window and follows focus. Runs with in_capture = true so grim observes the same composition as the
# display.
set -euo pipefail
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/effect-window.png"
readonly BASE="$UMBRIEL_RUNTIME_DIR/effect-window-base.toml"
cp "$UMBRIEL_CONFIG" "$BASE"

# Swap red and blue of whatever is under the window.
cat > "$UMBRIEL_RUNTIME_DIR/swap.glsl" <<'GLSL'
vec4 window(vec2 uv) { return umbriel_sample(uv).bgra; }
GLSL
# Zeroes red and blue, keeping only the sampled alpha in green: a marker that stays pure green wherever it applies.
cat > "$UMBRIEL_RUNTIME_DIR/green.glsl" <<'GLSL'
vec4 window(vec2 uv) { vec4 c = umbriel_sample(uv); return vec4(0.0, c.a, 0.0, c.a); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/hold.glsl" <<'GLSL'
// An opening animation that keeps the window at rest so its capture encloses the window effect.
vec4 animation(vec2 uv) { return umbriel_sample(uv); }
GLSL
# Forces the blue channel to its maximum, leaving red and green as sampled: a marker for the overlay slot.
cat > "$UMBRIEL_RUNTIME_DIR/overlaymark.glsl" <<'GLSL'
vec4 window(vec2 uv) { vec4 c = umbriel_sample(uv); return vec4(c.r, c.g, 1.0, c.a); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/ring2.glsl" <<'GLSL'
vec4 border(vec2 uv) { return vec4(1.0, 1.0, 0.0, 1.0); }
GLSL

spawn() {
  FILL_COLOR=${2:-0x80800000} "$UMBRIEL_UNMAP_CLIENT" "$1" 300 200 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)')
    [[ -n $window ]] && break
    sleep 0.025
  done
  [[ -n $window ]]
  read -r x y w h id < <(jq -r '"\(.x) \(.y) \(.w) \(.h) \(.id)"' <<< "$window")
}
centre() { grim "$IMAGE"; "$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$((x + w / 2))" "$((y + h / 2))"; }

### Section A: the swap preset's sampling contract, and window_effect overriding the default per rule.
cat "$BASE" > "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#0000FFFF"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[animation]
duration_ms = 2000
curve = "linear"
[animation.windows_in]
style = "none"
effect = "hold"
[animation.windows_out]
enabled = true
style = "fade"
duration_ms = 2000
[effects]
window = "swap"
in_capture = true
[effects.preset.swap]
kind = "window"
shader = "swap.glsl"
[effects.preset.green]
kind = "window"
shader = "green.glsl"
[effects.preset.hold]
kind = "animation"
shader = "hold.glsl"
[[window_rule]]
match.title = "^window-(rest|override)$"
default_floating = true
[[window_rule]]
match.title = "^window-override$"
window_effect = "green"
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$UMBRIEL" clock-freeze
spawn window-rest 0x40400000
# 25% red: over the blue backdrop the desktop composite is (0.25, 0, 0.75), swapped (0.75, 0, 0.25), while the
# window's own pixels are (0.25, 0, 0) -> swapped (0, 0, 0.25), so the two contracts differ in the red channel.
# Inside the opening capture the program shades only window content: (0.25,0,0) -> swapped (0,0,0.25), then
# composited over blue: red ~0, blue ~255.
"$UMBRIEL" clock-advance 500
read -r r g b < <(centre)
if (( r > 20 || b < 200 )); then
  echo "inside the opening capture the effect did not shade window content alone: $r $g $b"
  exit 1
fi
# After the capture ends the program reads the desktop through the window: (0.25,0,0.75) swapped is (0.75,0,0.25).
"$UMBRIEL" clock-advance 3000
"$UMBRIEL" settle > /dev/null
read -r r g b < <(centre)
if (( r < 170 || r > 210 || b < 45 || b > 85 )); then
  echo "at rest the effect did not sample the desktop backdrop through the window: $r $g $b"
  exit 1
fi
rest_x=$x rest_y=$y rest_w=$w rest_h=$h rest_id=$id

# Close: a fading snapshot is itself a running animation, so the effect keeps shading window content alone (the
# same contract as the opening capture) rather than the backdrop-inclusive at-rest blend.
"$UMBRIEL" msg "window-close:$rest_id" > /dev/null
"$UMBRIEL" clock-advance 200
x=$rest_x y=$rest_y w=$rest_w h=$rest_h
read -r r g b < <(centre)
if (( r > 20 || b < 200 )); then
  echo "the close snapshot dropped the window effect: $r $g $b"
  exit 1
fi
"$UMBRIEL" clock-advance 3000
"$UMBRIEL" settle > /dev/null

# window_effect on a rule replaces the default.
spawn window-override
"$UMBRIEL" clock-advance 3000
"$UMBRIEL" settle > /dev/null
read -r r g b < <(centre)
if (( g < 100 || r > 20 )); then
  echo "window_effect did not override the default preset: $r $g $b"
  exit 1
fi
"$UMBRIEL" clock-resume
"$UMBRIEL" settle > /dev/null

### Section B: a rule's window_effect = "off" disables the default, and losing focus does not lift the effect.
cat "$BASE" > "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
[colors]
backdrop = "#0000FFFF"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[effects]
window = "green"
in_capture = true
[effects.preset.green]
kind = "window"
shader = "green.glsl"
[[window_rule]]
match.title = "^window-default$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
[[window_rule]]
match.title = "^window-off$"
default_floating = true
default_position = { x = 500, y = 100, anchor = "top_left" }
window_effect = "off"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn window-default
"$UMBRIEL" settle > /dev/null
read -r r g b < <(centre)
if (( g < 200 || r > 20 || b > 20 )); then
  echo "the default window preset did not tint the focused window green: $r $g $b"
  exit 1
fi
default_x=$x default_y=$y default_w=$w default_h=$h

# window-off takes focus away from window-default; the effect must stay on window-default regardless.
spawn window-off
"$UMBRIEL" settle > /dev/null
read -r r g b < <(centre)
if (( r < 100 || r > 160 || b < 100 || b > 160 || g > 40 )); then
  echo "window_effect = off left the plain window tinted: $r $g $b"
  exit 1
fi
x=$default_x y=$default_y w=$default_w h=$default_h
read -r r g b < <(centre)
if (( g < 200 || r > 20 || b > 20 )); then
  echo "losing focus lifted the window effect from an unfocused window: $r $g $b"
  exit 1
fi

### Section C: a border preset's overlay tints only the focused, bordered window and follows focus.
cat "$BASE" > "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
[colors]
backdrop = "#000000FF"
[appearance]
border_width = 3
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[effects]
border = "ring2"
in_capture = true
[effects.preset.ring2]
kind = "border"
shader = "ring2.glsl"
padding = 10
overlay = "overlaymark"
[effects.preset.overlaymark]
kind = "window"
shader = "overlaymark.glsl"
[[window_rule]]
match.title = "^overlay-one$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
[[window_rule]]
match.title = "^overlay-two$"
default_floating = true
default_position = { x = 700, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn overlay-one 0xFF00FF00
"$UMBRIEL" settle > /dev/null
read -r _ _ b < <(centre)
if (( b < 200 )); then
  echo "the overlay did not tint the focused bordered window: b=$b"
  exit 1
fi
one_x=$x one_y=$y one_w=$w one_h=$h

spawn overlay-two 0xFF00FF00
"$UMBRIEL" settle > /dev/null
read -r _ _ b < <(centre)
if (( b < 200 )); then
  echo "the overlay did not follow focus to the second window: b=$b"
  exit 1
fi
x=$one_x y=$one_y w=$one_w h=$one_h
read -r _ _ b < <(centre)
if (( b > 200 )); then
  echo "the overlay stayed on the window that lost focus: b=$b"
  exit 1
fi

# Only the card under test should appear in the next section's overview.
for title in window-override window-default window-off overlay-one overlay-two; do
  close_id=$("$UMBRIEL" windows --json | jq -r --arg title "$title" '.[] | select(.title == $title) | .id')
  [[ -n $close_id ]] && "$UMBRIEL" msg "window-close:$close_id" > /dev/null
done
for _ in $(seq 100); do
  [[ -z $("$UMBRIEL" windows --json | jq -c '.[] | select(.title != null)') ]] && break
  sleep 0.02
done

### Section D: overview cards carry the window effect, including a card closed while the overview is open.
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
[colors]
backdrop = "#000000FF"
[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"
[overview]
zoom = 0.5
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[effects]
window = "green"
in_capture = true
[effects.preset.green]
kind = "window"
shader = "green.glsl"
[[window_rule]]
match.title = "^overview-card$"
default_floating = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn overview-card 0xFFFF0000
card_id=$id
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
green_open=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'g > 0.7 && r < 0.2 && b < 0.2')
if (( green_open < 200 )); then
  echo "the overview card did not show the window effect: $green_open green pixels"
  exit 1
fi
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg "window-close:$card_id" > /dev/null
# Settle refuses while the frozen close animation runs, so poll the window list for the snapshot taking over.
for _ in $(seq 100); do
  [[ -z $("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "overview-card")') ]] && break
  sleep 0.02
done
if [[ -n $("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "overview-card")') ]]; then
  echo "the window never left the window list after window-close"
  exit 1
fi
"$UMBRIEL" clock-advance 1
grim "$IMAGE"
green_closed=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'g > 0.7 && r < 0.2 && b < 0.2')
if (( green_closed * 2 < green_open )); then
  echo "the overview card's close snapshot dropped the window effect: $green_open -> $green_closed"
  exit 1
fi
"$UMBRIEL" clock-advance 5000
"$UMBRIEL" clock-resume
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" settle > /dev/null

# A card's window effect covers every mirrored client surface, as it covers the live view's surface tree. With
# OFFSET_GEOMETRY the red root sits above a blue child that the surface walk visits first; by default the blue child
# covers the red root from above. Either card turns green with none of its root's red or its child's blue left.
card_surfaces() {
  local title=$1 client
  OFFSET_GEOMETRY=$2 "$UMBRIEL_SUBSURFACE_CLIENT" "$title" > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  client=$!
  for _ in $(seq 60); do
    grep -q '^mapped$' "$UMBRIEL_RUNTIME_DIR/$title.log" && break
    sleep 0.05
  done
  grep -q '^mapped$' "$UMBRIEL_RUNTIME_DIR/$title.log" || {
    echo "$title never mapped: $(cat "$UMBRIEL_RUNTIME_DIR/$title.log")"
    exit 1
  }
  "$UMBRIEL" settle > /dev/null
  "$UMBRIEL" msg overview-open > /dev/null
  "$UMBRIEL" settle > /dev/null
  grim "$IMAGE"
  card_green=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'g > 0.7 && r < 0.2 && b < 0.2')
  card_red=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r > 0.7 && g < 0.2 && b < 0.2')
  card_blue=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'b > 0.7 && r < 0.2 && g < 0.2')
  "$UMBRIEL" msg overview-close > /dev/null
  "$UMBRIEL" settle > /dev/null
  "$UMBRIEL" clock-freeze
  kill "$client"
  for _ in $(seq 100); do
    [[ -z $("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)') ]] && break
    sleep 0.02
  done
  [[ -z $("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)') ]] || {
    echo "$title never left the window list after its client exited"
    exit 1
  }
  "$UMBRIEL" clock-advance 5000
  "$UMBRIEL" clock-resume
  "$UMBRIEL" settle > /dev/null
}
card_surfaces card-below 40
if (( card_green < 10000 || card_red > 20 || card_blue > 20 )); then
  echo "a card whose first surface lies below its root missed the window effect: green $card_green," \
    "red $card_red, blue $card_blue"
  exit 1
fi
card_surfaces card-above 0
if (( card_green < 10000 || card_red > 20 || card_blue > 20 )); then
  echo "a card whose child covers its root missed the window effect: green $card_green, red $card_red," \
    "blue $card_blue"
  exit 1
fi

echo "in-place window effect at rest, inside a capture, per rule, on overlay focus, and on overview cards verified"
