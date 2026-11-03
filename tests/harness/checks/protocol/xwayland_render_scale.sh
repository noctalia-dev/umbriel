#!/usr/bin/env bash
# harness: xwayland=native
# X11 clients that draw a fixed number of pixels must stay at native output
# size unless forced X11 supersampling is explicitly enabled.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly EXPECT_SCALE=${UMBRIEL_X11_EXPECT_SCALE:-1.25}
readonly EXPECT_WIDTH=${UMBRIEL_X11_EXPECT_WIDTH:-480}
readonly EXPECT_HEIGHT=${UMBRIEL_X11_EXPECT_HEIGHT:-320}
readonly EXPECT_RED_WIDTH=${UMBRIEL_X11_EXPECT_RED_WIDTH:-500}
readonly EXPECT_RED_HEIGHT=${UMBRIEL_X11_EXPECT_RED_HEIGHT:-300}
readonly EXPECT_RED_PIXELS=${UMBRIEL_X11_EXPECT_RED_PIXELS:-120000}
readonly EXPECT_GDK_SCALE=${UMBRIEL_X11_EXPECT_GDK_SCALE:-1}
readonly EXPECT_XFT_DPI=${UMBRIEL_X11_EXPECT_XFT_DPI:-122880}
readonly EXPECT_OUTPUT_WIDTH=${UMBRIEL_X11_EXPECT_OUTPUT_WIDTH:-1280}
readonly EXPECT_OUTPUT_HEIGHT=${UMBRIEL_X11_EXPECT_OUTPUT_HEIGHT:-960}
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/x11-render-scale.png"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/x11-render-scale.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
[colors]
backdrop = "#00FF00FF"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.title_bar]
mode = "off"
[appearance.shadow]
enabled = false
[output."HEADLESS-1"]
mode = "1280x960"
scale = 1.25
render_scale = 2.5
[[window_rule]]
match.title = "^x11-render-scale$"
default_floating = true
default_position = { x = 100, y = 240, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$UMBRIEL_RUNTIME_DIR/x11-render-scale.control"
exec {client_fd}<>"$UMBRIEL_RUNTIME_DIR/x11-render-scale.control"
"$UMBRIEL_XWAYLAND_SHAPE_CLIENT" x11-render-scale managed unshaped <&"$client_fd" >"$CLIENT_LOG" 2>&1 &
client_pid=$!
await_lines "$CLIENT_LOG" 'done mapped bounding=' 1

expect_state() {
  local window= width height settings= outputs= red_width=0 red_height=0 red_pixels=0
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "x11-render-scale")')
    [[ -n $window ]] && break
    sleep 0.025
  done
  [[ -n $window ]] || { echo "X11 client did not map"; return 1; }
  read -r width height < <(jq -r '"\(.w) \(.h)"' <<< "$window")
  if ((width != EXPECT_WIDTH || height != EXPECT_HEIGHT)); then
    echo "X11 fixed-pixel window is ${width}x${height} logical; expected ${EXPECT_WIDTH}x${EXPECT_HEIGHT} at scale $EXPECT_SCALE"
    return 1
  fi

  for _ in $(seq 80); do
    settings=$("$UMBRIEL_XSETTINGS_CLIENT" 2>/dev/null) || true
    [[ $settings == *"Gdk/WindowScalingFactor=$EXPECT_GDK_SCALE "* && $settings == *"Xft/DPI=$EXPECT_XFT_DPI"* ]] && break
    sleep 0.025
  done
  if [[ $settings != *"Gdk/WindowScalingFactor=$EXPECT_GDK_SCALE "* || $settings != *"Xft/DPI=$EXPECT_XFT_DPI"* ]]; then
    echo "XSETTINGS does not match the X11 scale: $settings"
    return 1
  fi

  for _ in $(seq 80); do
    outputs=$("$UMBRIEL_XWAYLAND_OUTPUT_CLIENT") || true
    if jq -e --argjson w "$EXPECT_OUTPUT_WIDTH" --argjson h "$EXPECT_OUTPUT_HEIGHT" \
      'any(.outputs[]; .name == "HEADLESS-1" and .w == $w and .h == $h)' <<< "$outputs" > /dev/null; then
      break
    fi
    sleep 0.025
  done
  if ! jq -e --argjson w "$EXPECT_OUTPUT_WIDTH" --argjson h "$EXPECT_OUTPUT_HEIGHT" \
    'any(.outputs[]; .name == "HEADLESS-1" and .w == $w and .h == $h)' <<< "$outputs" > /dev/null; then
    echo "X11 output size differs: $outputs"
    return 1
  fi

  "$UMBRIEL" settle > /dev/null
  for _ in $(seq 80); do
    grim -o HEADLESS-1 "$IMAGE"
    read -r _ _ red_width red_height < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox 'r > 0.9 && g < 0.1 && b < 0.1')
    red_pixels=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r > 0.99 && g < 0.01 && b < 0.01')
    if ((red_width == EXPECT_RED_WIDTH && red_height == EXPECT_RED_HEIGHT && red_pixels == EXPECT_RED_PIXELS)); then
      break
    fi
    sleep 0.025
  done
  if ((red_width != EXPECT_RED_WIDTH || red_height != EXPECT_RED_HEIGHT || red_pixels != EXPECT_RED_PIXELS)); then
    echo "X11 rendered red region is ${red_width}x${red_height}, $red_pixels solid pixels; expected ${EXPECT_RED_WIDTH}x${EXPECT_RED_HEIGHT}, $EXPECT_RED_PIXELS"
    return 1
  fi
}

expect_state
if [[ $EXPECT_SCALE == 1.25 ]]; then
  for render_scale in 1.25 0.5; do
    sed -i "s/^render_scale = .*/render_scale = $render_scale/" "$UMBRIEL_CONFIG"
    "$UMBRIEL" msg config-reload > /dev/null
    expect_state
  done
  sed -i '/^\[general\]$/a force_x11_supersample = true' "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  expect_state
fi

printf 'quit\n' >&"$client_fd"
wait "$client_pid"
exec {client_fd}>&-
echo "X11 window, native output pixels and XSETTINGS follow scale $EXPECT_SCALE"
