#!/usr/bin/env bash
# A window's output-wide dim follows stacking, passes input through, reloads and clears with the owner.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
readonly FRAME="$UMBRIEL_RUNTIME_DIR/dim.png"
capture_config
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[colors]
backdrop = "#FFFFFFFF"
[[window_rule]]
match.title = "^(under|dim-one|dim-two)$"
default_floating = true
default_floating_size_px = { width = 200, height = 180 }
[[window_rule]]
match.title = "^under$"
default_position = { x = 100, y = 200, anchor = "top_left" }
[[window_rule]]
match.title = "^dim-one$"
default_position = { x = 500, y = 200, anchor = "top_left" }
dim_behind = 0.5
[[window_rule]]
match.title = "^dim-two$"
default_position = { x = 800, y = 200, anchor = "top_left" }
dim_behind = 0.5
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
shot() { "$UMBRIEL" settle; grim "$FRAME"; }
assert_pixel() { assert_region_rgb "$FRAME" "10x10+$1+$2" "$3" "$4" "$5" "$6"; }
FILL_COLOR=0xFF00FF00 FOLLOW_CONFIGURES=1 "$UMBRIEL_UNMAP_CLIENT" under 200 180 > "$UMBRIEL_RUNTIME_DIR/under.log" 2>&1 &
await_window under
FILL_COLOR=0xFF0000FF FOLLOW_CONFIGURES=1 "$UMBRIEL_UNMAP_CLIENT" dim-one 200 180 > "$UMBRIEL_RUNTIME_DIR/one.log" 2>&1 &
one=$!
await_window dim-one
shot
assert_pixel 1200 650 128 128 128 'background dims'
assert_pixel 150 250 0 128 0 'lower window dims'
assert_pixel 550 250 0 0 255 'owner stays bright'

# A pointer click through the dim must reach and raise the lower window.
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 150 250 click 272
shot
window_by_title under | jq -e '.active' > /dev/null
assert_pixel 150 250 0 255 0 'raised window stays above dim'

FILL_COLOR=0xFFFF0000 FOLLOW_CONFIGURES=1 "$UMBRIEL_UNMAP_CLIENT" dim-two 200 180 > "$UMBRIEL_RUNTIME_DIR/two.log" 2>&1 &
two=$!
await_window dim-two
shot
assert_pixel 1200 650 64 64 64 'two backdrops accumulate'
assert_pixel 550 250 0 0 128 'upper dim affects lower owner'
assert_pixel 850 250 255 0 0 'upper owner stays bright'

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[[window_rule]]
match.title = "^dim-two$"
dim_behind = 0.0
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
shot
assert_pixel 1200 650 128 128 128 'zero override clears upper dim on reload'
one_id=$(window_by_title dim-one | jq -r .id)
"$UMBRIEL" msg window-focus "$one_id" > /dev/null
"$UMBRIEL" msg window-move-to-workspace-silent 2 > /dev/null
shot
assert_pixel 1200 650 255 255 255 'hidden workspace does not dim'
"$UMBRIEL" msg workspace-switch 2 > /dev/null
shot
assert_pixel 1200 650 128 128 128 'visible workspace restores dim'
"$UMBRIEL" msg workspace-switch 1 > /dev/null
shot
assert_pixel 1200 650 255 255 255 'leaving workspace clears dim'
kill "$one" "$two"
for _ in $(seq 100); do
  [[ $("$UMBRIEL" windows --json | jq length) == 1 ]] && break
  sleep 0.025
done
shot
assert_pixel 1200 650 255 255 255 'closing owners restores brightness'
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[[window_rule]]
match.title = "^under$"
dim_behind = 0.5
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
shot
assert_pixel 1200 650 128 128 128 'reload enables dim on an existing window'
sed -i '/^dim_behind = /d' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
shot
assert_pixel 1200 650 255 255 255 'removing all dim rules clears the existing backdrop'
# Sample the backdrop halfway through the owner's opening fade.
sed -i '/^\[animation\]$/ { n; s/enabled = false/enabled = true/; }' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[[window_rule]]
match.title = "^dim-one$"
dim_behind = 0.5
[animation.windows_in]
style = "fade"
duration_ms = 1000
curve = "linear"
[animation.windows_out]
enabled = false
[animation.windows_move]
enabled = false
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" clock-freeze > /dev/null
FILL_COLOR=0xFF0000FF FOLLOW_CONFIGURES=1 "$UMBRIEL_UNMAP_CLIENT" dim-one 200 180 > "$UMBRIEL_RUNTIME_DIR/fade.log" 2>&1 &
fade=$!
await_window dim-one
"$UMBRIEL" clock-advance 500 > /dev/null
grim "$FRAME"
assert_pixel 1200 650 191 191 191 'dim follows half-open fade'
"$UMBRIEL" clock-advance 500 > /dev/null
shot
assert_pixel 1200 650 128 128 128 'full dim after opening fade'
kill "$fade"
for _ in $(seq 100); do
  [[ $("$UMBRIEL" windows --json | jq length) == 1 ]] && break
  sleep 0.025
done
shot
assert_pixel 1200 650 255 255 255 'animated owner clears dim on close'
echo 'window dim follows stacking, reloads, passes pointer input and clears on close'
