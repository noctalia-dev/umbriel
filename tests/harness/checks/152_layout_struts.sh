#!/usr/bin/env bash
# harness: outputs=1
# Layout struts reserve logical space for normal tiled windows after a
# real layer-shell exclusive zone. Floating windows, maximize-to-edges, and
# fullscreen keep their own broader areas while the scrolling strip reserves the
# strut band they bleed into, and reload removes struts from already mapped
# workspaces.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly LAYER_CLIENT="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly BASE_CONFIG="$UMBRIEL_RUNTIME_DIR/layout-struts-base.toml"
readonly PANEL_LOG="$UMBRIEL_RUNTIME_DIR/layout-struts-panel.log"
readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/layout-struts.png"

cp "$UMBRIEL_CONFIG" "$BASE_CONFIG"

write_config() {
  cat "$BASE_CONFIG" > "$UMBRIEL_CONFIG"
  cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[layout]
mode = "dwindle"
gap = 0

[output.HEADLESS-1]
workspaces = ["base", "override", "scroll-h", "scroll-v"]

[[workspace]]
name = "scroll-h"
layout.mode = "scrolling"
layout.gap = 8
layout.scrolling.direction = "horizontal"
layout.scrolling.default_width_fraction = 0.5
layout.scrolling.center_underfull_strip = false

[[workspace]]
name = "scroll-v"
layout.mode = "scrolling"
layout.gap = 8
layout.scrolling.direction = "vertical"
layout.scrolling.default_width_fraction = 0.5
layout.scrolling.center_underfull_strip = false

[[window_rule]]
match.title = "^strut-float$"
default_floating = true
default_size = [200, 100]
default_position = { x = 0, y = 0, anchor = "top_left" }
EOF
  if [[ $1 == with-struts ]]; then
    cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout.struts]
left = 17
right = 23
top = 31
bottom = 29

[[workspace]]
name = "override"

[workspace.layout.struts]
left = 50
top = 5

[[workspace]]
name = "override"
output = "HEADLESS-1"

[workspace.layout.struts]
right = 60
EOF
  fi
}

field_of() {
  "$UMBRIEL" windows --json \
    | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

box_of() {
  "$UMBRIEL" windows --json \
    | jq -r --arg title "$1" '.[] | select(.title == $title) | "\(.w)x\(.h)+\(.x)+\(.y)"'
}

assert_box() {
  local title=$1 expected="$2x$3+$4+$5" actual= previous=
  for _ in $(seq 80); do
    actual=$(box_of "$title")
    if [[ $actual == "$expected" && $actual == "$previous" ]]; then
      return 0
    fi
    previous=$actual
    sleep 0.1
  done
  echo "expected '$title' box $expected, got ${actual:-missing}"
  return 1
}

assert_size() {
  local title=$1 expected="$2x$3" actual= previous=
  for _ in $(seq 80); do
    actual="$(field_of "$title" w)x$(field_of "$title" h)"
    if [[ $actual == "$expected" && $actual == "$previous" ]]; then
      return 0
    fi
    previous=$actual
    sleep 0.1
  done
  echo "expected '$title' size $expected, got ${actual:-missing}"
  return 1
}

pixel_color() {
  magick "$1" -alpha off -format "%[hex:p{$2,$3}]" info:
}

assert_client_pixel() {
  local image=$1 x=$2 y=$3 color
  color=$(pixel_color "$image" "$x" "$y")
  if [[ $color != 0000FF* && $color != 00FF00* ]]; then
    echo "expected client content at $x,$y, got #$color"
    return 1
  fi
}

assert_not_client_pixel() {
  local image=$1 x=$2 y=$3 color
  color=$(pixel_color "$image" "$x" "$y")
  if [[ $color == 0000FF* || $color == 00FF00* ]]; then
    echo "expected non-client content at $x,$y, got #$color"
    return 1
  fi
}

capture_maximized_to_edges() {
  local title=$1
  assert_size "$title" 1280 680
  grim -o HEADLESS-1 "$SCREENSHOT"
  assert_not_client_pixel "$SCREENSHOT" 0 39
  assert_client_pixel "$SCREENSHOT" 0 40
  assert_client_pixel "$SCREENSHOT" 1279 40
  assert_client_pixel "$SCREENSHOT" 0 719
  assert_client_pixel "$SCREENSHOT" 1279 719
}

spawn_client() {
  local title=$1 width=${2:-400} height=${3:-300}
  "$CLIENT" "$title" "$width" "$height" > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
}

focus_window() {
  local id=
  for _ in $(seq 80); do
    id=$(field_of "$1" id)
    if [[ -n $id ]]; then
      "$UMBRIEL" msg "window-focus-warp:$id" > /dev/null
      return 0
    fi
    sleep 0.1
  done
  echo "window '$1' did not map"
  return 1
}

write_config with-struts
"$UMBRIEL" msg config-reload > /dev/null

"$LAYER_CLIENT" HEADLESS-1 40 > "$PANEL_LOG" 2>&1 &
for _ in $(seq 80); do
  grep -q '^ready$' "$PANEL_LOG" && break
  sleep 0.1
done
if ! grep -q '^ready$' "$PANEL_LOG"; then
  echo "exclusive-zone panel did not map: $(< "$PANEL_LOG")"
  exit 1
fi

spawn_client strut-base
focus_window strut-base
assert_box strut-base 1240 620 17 71
grim -o HEADLESS-1 "$SCREENSHOT"
assert_client_pixel "$SCREENSHOT" 17 71
assert_client_pixel "$SCREENSHOT" 1256 71
assert_client_pixel "$SCREENSHOT" 17 690
assert_client_pixel "$SCREENSHOT" 1256 690
assert_not_client_pixel "$SCREENSHOT" 16 71
assert_not_client_pixel "$SCREENSHOT" 17 70
assert_not_client_pixel "$SCREENSHOT" 1257 71
assert_not_client_pixel "$SCREENSHOT" 17 691

"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
capture_maximized_to_edges strut-base
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
assert_box strut-base 1240 620 17 71

"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
assert_size strut-base 1280 720
grim -o HEADLESS-1 "$SCREENSHOT"
assert_client_pixel "$SCREENSHOT" 0 0
assert_client_pixel "$SCREENSHOT" 1279 0
assert_client_pixel "$SCREENSHOT" 0 719
assert_client_pixel "$SCREENSHOT" 1279 719
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
assert_box strut-base 1240 620 17 71

spawn_client strut-float 200 100
focus_window strut-float
assert_box strut-float 200 100 0 40

"$UMBRIEL" msg workspace-switch:override > /dev/null
spawn_client strut-override
focus_window strut-override
assert_box strut-override 1170 646 50 45

"$UMBRIEL" msg workspace-switch:scroll-h > /dev/null
spawn_client strut-scroll-h
focus_window strut-scroll-h
assert_box strut-scroll-h 608 604 25 79
"$UMBRIEL" msg window-toggle-maximize > /dev/null
assert_box strut-scroll-h 1224 604 25 79
"$UMBRIEL" msg window-toggle-maximize > /dev/null
assert_box strut-scroll-h 608 604 25 79
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
capture_maximized_to_edges strut-scroll-h
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
assert_box strut-scroll-h 608 604 25 79

# A maximize-to-edges window is presented against the usable area, past the
# struts the strip is inset by, so the strip has to reserve that band: the
# neighboring column keeps one gap of clearance instead of ending up underneath
# the window.
spawn_client strut-scroll-neighbor
focus_window strut-scroll-neighbor
assert_box strut-scroll-neighbor 608 604 641 79
focus_window strut-scroll-h
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
capture_maximized_to_edges strut-scroll-h
assert_box strut-scroll-neighbor 608 604 1288 79
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
assert_box strut-scroll-h 608 604 25 79
assert_box strut-scroll-neighbor 608 604 641 79

"$UMBRIEL" msg workspace-switch:scroll-v > /dev/null
spawn_client strut-scroll-v
focus_window strut-scroll-v
assert_box strut-scroll-v 1224 298 25 79
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
capture_maximized_to_edges strut-scroll-v
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
assert_box strut-scroll-v 1224 298 25 79

write_config without-struts
"$UMBRIEL" msg config-reload > /dev/null
assert_box strut-scroll-v 1264 328 8 48

"$UMBRIEL" msg workspace-switch:override > /dev/null
assert_box strut-override 1280 680 0 40

"$UMBRIEL" msg workspace-switch:base > /dev/null
assert_box strut-base 1280 680 0 40
assert_box strut-float 200 100 0 40

echo "layout struts covered layer zones, workspace overrides, tiled states, scrolling directions, and reload"
