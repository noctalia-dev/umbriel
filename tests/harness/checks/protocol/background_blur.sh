#!/usr/bin/env bash
# ext_background_effect_v1: the manager is advertised with the blur capability, a client's surface-local region blurs
# exactly that area, a later commit moves it, a NULL region clears it, and the [appearance.blur] keys still decide
# whether any of that is honored. The window paints nothing, so outside a requested region the frame must stay
# pixel-identical to the wallpaper and a blurred area is the only difference.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BASE="$UMBRIEL_RUNTIME_DIR/background-blur-base.png"
readonly FRAME="$UMBRIEL_RUNTIME_DIR/background-blur.png"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/background-blur-client.log"
# The wallpaper's square ends at (200, 200) and the window spans (100, 100) to (500, 400), so this crop goes through
# the square's corner: a crisp edge holds green and backdrop values only, while blur fills it with the mixture.
readonly FEATURE="60x60+170+170"

fail() {
  echo "$1"
  exit 1
}

# Pixels between the square's green (0, 1, 0) and the backdrop (0.33, 0.47, 0.67): a crisp edge holds neither r nor b,
# while the blur gradient spans both. Both channels stay clear of the two end colours, so antialiasing cannot fake it.
mixture() { "$UMBRIEL_PIXEL_PROBE" "$1" count 'r > 0.06 && r < 0.28 && b > 0.06 && b < 0.60' "$FEATURE"; }

# The frame must equal the wallpaper-only baseline: a transparent window with no blur adds nothing.
identical() {
  local largest differing
  read -r largest differing < <("$UMBRIEL_PIXEL_PROBE" "$BASE" diff "$1")
  if ((largest > 1 || differing > 0)); then
    fail "$2: the frame differs from the wallpaper by up to $largest in $differing pixels"
  fi
}

blurred() {
  local count
  count=$(mixture "$1")
  if ((count < 40)); then
    fail "$2: only $count mixed pixels at the square's corner; the blur is missing"
  fi
}

crisp() {
  local count
  count=$(mixture "$1")
  if ((count > 0)); then
    fail "$2: $count mixed pixels at the square's corner; the blur was not clipped away"
  fi
}

# Optimized blur samples the background beneath the window stack, so over the square it shows the backdrop instead of
# a green gradient; the square's own pixels are the signal there.
readonly INTERIOR="40x40+120+120"
green() { "$UMBRIEL_PIXEL_PROBE" "$1" count 'g > 0.9 && r < 0.1' "$INTERIOR"; }

# $1 blur rule on the window, $2 client_requests, $3 optimized, $4 enabled.
configure() {
  cat > "$UMBRIEL_CONFIG" <<EOF
[general]
xwayland = false
show_cheatsheet = false
autostart = []

[output.HEADLESS-1]
scale = 1.0

[animation]
enabled = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.title_bar]
mode = "off"

[appearance.shadow]
enabled = false

[appearance.blur]
enabled = ${4:-true}
client_requests = ${2:-true}
optimized = ${3:-false}
passes = 2
radius = 6
noise = 0.0
brightness = 1.0
contrast = 1.0
saturation = 1.0

[[window_rule]]
match.title = "^background-blur\$"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 100, y = 100, anchor = "top_left" }
blur = ${1:-false}
EOF
  "$UMBRIEL" msg config-reload > /dev/null
}

configure

"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 > "$UMBRIEL_RUNTIME_DIR/background.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/background.log" ready 1
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 bottom-layer > "$UMBRIEL_RUNTIME_DIR/square.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/square.log" ready 1
"$UMBRIEL" settle
grim "$BASE"

mkfifo "$UMBRIEL_RUNTIME_DIR/background-blur.fifo"
# One long-lived window keeps the same surface across rule changes; regions arrive over the pipe and every command
# echoes the applied region, so the check waits on the commit instead of on a delay.
FILL_COLOR=0x00000000 "$UMBRIEL_BACKGROUND_EFFECT_CLIENT" background-blur 400 300 \
  < "$UMBRIEL_RUNTIME_DIR/background-blur.fifo" > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!
exec 3> "$UMBRIEL_RUNTIME_DIR/background-blur.fifo"
send() { printf '%s\n' "$1" >&3; }

await_lines "$CLIENT_LOG" 'bound' 1
await_lines "$CLIENT_LOG" 'mapped 400x300' 1
await_lines "$CLIENT_LOG" 'capabilities ' 1
grep -qx 'capabilities 1' "$CLIENT_LOG" \
  || fail "the manager did not advertise the blur capability: $(grep '^capabilities ' "$CLIENT_LOG")"

read -r wx wy ww wh < <(
  "$UMBRIEL" windows --json | jq -r '.[] | select(.title == "background-blur") | "\(.x) \(.y) \(.w) \(.h)"'
)
[[ $wx == 100 && $wy == 100 && $ww == 400 && $wh == 300 ]] \
  || fail "window is at $wx,$wy ${ww}x${wh}, not the 400x300 at 100,100 the region arithmetic assumes"
"$UMBRIEL" settle

# No request: the window stays invisible and no blur appears.
grim "$FRAME"
identical "$FRAME" "no request"

# A left-half region covers the square's corner.
send "r 0 0 200 300"
await_lines "$CLIENT_LOG" 'region 0 0 200 300' 1
"$UMBRIEL" settle
grim "$FRAME"
blurred "$FRAME" "left-half region"

# Moving the region away from the corner must move the blur with it.
send "r 200 0 400 300"
await_lines "$CLIENT_LOG" 'region 200 0 400 300' 1
"$UMBRIEL" settle
grim "$FRAME"
crisp "$FRAME" "right-half region"

# NULL clears the request entirely.
send "n"
await_lines "$CLIENT_LOG" 'region none' 1
"$UMBRIEL" settle
grim "$FRAME"
identical "$FRAME" "cleared region"

# A window rule still blurs the whole surface on its own, and a request narrows it to the region.
configure true
"$UMBRIEL" settle
grim "$FRAME"
blurred "$FRAME" "rule without a region"
send "r 200 0 400 300"
await_lines "$CLIENT_LOG" 'region 200 0 400 300' 2
"$UMBRIEL" settle
grim "$FRAME"
crisp "$FRAME" "rule restricted by a region"

# The master switch wins over both.
configure true true false false
"$UMBRIEL" settle
send "r 0 0 200 300"
await_lines "$CLIENT_LOG" 'region 0 0 200 300' 2
"$UMBRIEL" settle
grim "$FRAME"
identical "$FRAME" "blur disabled"
# A new client proves the global is still advertised while blur is off; the long-lived log cannot show that.
probe=$("$UMBRIEL_BACKGROUND_EFFECT_CLIENT" --probe 2>&1) \
  || fail "the manager disappeared when blur was disabled: $probe"
grep -qx 'capabilities 1' <<<"$probe" \
  || fail "a new client lost the blur capability when blur was disabled: $probe"
if grep -qx 'capabilities 0' "$CLIENT_LOG"; then
  fail "the manager sent an empty capability set"
fi

# The opt-out key keeps the global advertised but ignores the request, so the rule covers the whole surface again.
configure true false
"$UMBRIEL" settle
send "r 200 0 400 300"
await_lines "$CLIENT_LOG" 'region 200 0 400 300' 3
"$UMBRIEL" settle
grim "$FRAME"
blurred "$FRAME" "client requests disabled"

# Without a rule, ignored requests leave the surface alone.
configure false false
"$UMBRIEL" settle
send "r 0 0 400 300"
await_lines "$CLIENT_LOG" 'region 0 0 400 300' 1
"$UMBRIEL" settle
grim "$FRAME"
identical "$FRAME" "client requests disabled without a rule"

# Optimized blur has to honor the same region boundary.
configure false true true
"$UMBRIEL" settle
send "r 0 0 200 300"
await_lines "$CLIENT_LOG" 'region 0 0 200 300' 3
"$UMBRIEL" settle
grim "$FRAME"
green_count=$(green "$FRAME")
if ((green_count > 100)); then
  fail "optimized blur with a region: $green_count square pixels survived inside the requested area"
fi
send "r 200 0 400 300"
await_lines "$CLIENT_LOG" 'region 200 0 400 300' 4
"$UMBRIEL" settle
grim "$FRAME"
green_count=$(green "$FRAME")
if ((green_count < 1500)); then
  fail "optimized blur with the region moved away: only $green_count square pixels left outside the request"
fi

# The protocol defers removal to the next wl_surface.commit: destroying the object alone keeps the blur on screen.
send "r 0 0 200 300"
await_lines "$CLIENT_LOG" 'region 0 0 200 300' 4
"$UMBRIEL" settle
grim "$FRAME"
green_count=$(green "$FRAME")
if ((green_count > 100)); then
  fail "destroy setup: $green_count square pixels survived before the object was destroyed"
fi
send "d"
await_lines "$CLIENT_LOG" 'destroyed' 1
# A reload that touches window chrome re-derives every blur without the client committing; the region must still be the
# committed one.
printf '\n[colors]\naccent_primary = "#7AA3FF"\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
grim "$FRAME"
green_count=$(green "$FRAME")
if ((green_count > 100)); then
  fail "destroying the object removed the blur before the next commit: $green_count square pixels came back"
fi
send "p"
await_lines "$CLIENT_LOG" 'committed' 1
"$UMBRIEL" settle
grim "$FRAME"
green_count=$(green "$FRAME")
if ((green_count < 1500)); then
  fail "the commit after destroy kept the blur: only $green_count square pixels came back"
fi

# Animated leg: a client-only blur has no window rule, so the rule's own blur fade must not zero it while the window
# opens, while it loses focus, and after the focus dim settles.
cat > "$UMBRIEL_CONFIG" <<CONFIG
[general]
xwayland = false
show_cheatsheet = false
autostart = []

[output.HEADLESS-1]
scale = 1.0

[animation]
enabled = true
duration_ms = 300
curve = "linear"

[animation.windows_in]
style = "fade"

[animation.windows_move]
enabled = false

[animation.dim_unfocused]
enabled = true

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.title_bar]
mode = "off"

[appearance.shadow]
enabled = false

[appearance.blur]
enabled = true
client_requests = true
optimized = false
passes = 2
radius = 6
noise = 0.0
brightness = 1.0
contrast = 1.0
saturation = 1.0

[[window_rule]]
match.title = "^background-blur-anim\$"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 100, y = 100, anchor = "top_left" }
blur = false
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
mkfifo "$UMBRIEL_RUNTIME_DIR/background-blur-anim.fifo"
FILL_COLOR=0x00000000 "$UMBRIEL_BACKGROUND_EFFECT_CLIENT" background-blur-anim 400 300 \
  < "$UMBRIEL_RUNTIME_DIR/background-blur-anim.fifo" > "$UMBRIEL_RUNTIME_DIR/background-blur-anim.log" 2>&1 &
ANIM_PID=$!
exec 4> "$UMBRIEL_RUNTIME_DIR/background-blur-anim.fifo"
send_anim() { printf '%s\n' "$1" >&4; }
await_lines "$UMBRIEL_RUNTIME_DIR/background-blur-anim.log" 'mapped 400x300' 1
send_anim "r 0 0 200 300"
await_lines "$UMBRIEL_RUNTIME_DIR/background-blur-anim.log" 'region 0 0 200 300' 1
"$UMBRIEL" settle
grim "$FRAME"
blurred "$FRAME" "animated open finished"

# Focus moves to a new window: the dim transition animates the blurred window without any commit from it.
FILL_COLOR=0xFF445566 "$UMBRIEL_UNMAP_CLIENT" background-blur-focus 200 200 \
  > "$UMBRIEL_RUNTIME_DIR/background-blur-focus.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/background-blur-focus.log" mapped 1
"$UMBRIEL" settle
grim "$FRAME"
blurred "$FRAME" "after the unfocused dim settled"

send_anim "q"
exec 4>&-
wait "$ANIM_PID" || true

send "q"
exec 3>&-
wait "$CLIENT_PID" || true
echo "ext_background_effect_v1 blurs exactly the requested region and [appearance.blur] still gates it"
