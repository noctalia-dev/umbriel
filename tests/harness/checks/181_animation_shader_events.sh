#!/usr/bin/env bash
# Each event must introduce a shader-only color during its transition and
# remove that color afterwards. Geometry settling alone cannot satisfy this.
set -euo pipefail
cat > "$UMBRIEL_RUNTIME_DIR/fixture-1.glsl" <<'GLSL'
vec4 animation(vec2 uv) { return vec4(1.0, 0.0, 0.0, 1.0); }
GLSL
readonly BASE="$UMBRIEL_RUNTIME_DIR/shader-events-base.toml"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/shader-events.png"
cp "$UMBRIEL_CONFIG" "$BASE"

configure() {
  cat "$BASE" > "$UMBRIEL_CONFIG"
  cat >> "$UMBRIEL_CONFIG" <<EOF

[animation]
duration_ms = 1200
curve = "linear"
[animation.windows_in]
enabled = false
[animation.$1]
enabled = true
shader = "fixture-1.glsl"
EOF
  "$UMBRIEL" msg config-reload > /dev/null
  sleep 1.4
}

red_pixels() {
  grim "$IMAGE"
  magick "$IMAGE" -resize 50% -fx '(r > 0.9 && g < 0.1 && b < 0.1) ? 1 : 0' \
    -format '%[fx:round(mean*w*h)]' info:
}
assert_no_red() {
  local count
  count=$(red_pixels)
  if (( count > 10 )); then
    echo "$1: stale shader color before transition or after completion ($count pixels)"
    exit 1
  fi
}
assert_transition() {
  local count
  sleep 0.15
  count=$(red_pixels)
  if (( count < 30 )); then
    echo "$1: missing shader-only intermediate color ($count pixels)"
    exit 1
  fi
  sleep 1.3
  assert_no_red "$1"
  echo "$1 shader transition verified"
}

configure windows_move
"$UMBRIEL_UNMAP_CLIENT" shader-events-a 700 700 > "$UMBRIEL_RUNTIME_DIR/a.log" 2>&1 &
"$UMBRIEL_UNMAP_CLIENT" shader-events-b 700 700 > "$UMBRIEL_RUNTIME_DIR/b.log" 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq length) == 2 ]] && break
  sleep 0.05
done
sleep 1.4
first=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "shader-events-a") | .id')
second=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "shader-events-b") | .id')
assert_no_red move
"$UMBRIEL" msg window-toggle-maximize > /dev/null
assert_transition resize

configure border
"$UMBRIEL" msg "window-focus:$second" > /dev/null
sleep 1.4
assert_no_red border
"$UMBRIEL" msg "window-focus:$first" > /dev/null
assert_transition border

configure dim_unfocused
assert_no_red dim_unfocused
"$UMBRIEL" msg "window-focus:$second" > /dev/null
assert_transition dim_unfocused

configure workspaces
assert_no_red workspaces
"$UMBRIEL" msg workspace-switch:2 > /dev/null
assert_transition workspaces
"$UMBRIEL" msg workspace-switch:1 > /dev/null
sleep 1.4

configure overview
assert_no_red overview
"$UMBRIEL" msg overview-open > /dev/null
assert_transition overview
"$UMBRIEL" msg overview-close > /dev/null
assert_transition overview-close

configure scratchpad
"$UMBRIEL" msg window-move-to-scratchpad:HEADLESS-1 > /dev/null
sleep 1.4
assert_no_red scratchpad
"$UMBRIEL" msg scratchpad-toggle:HEADLESS-1 > /dev/null
assert_transition scratchpad-show
"$UMBRIEL" msg scratchpad-toggle:HEADLESS-1 > /dev/null
assert_transition scratchpad-hide

configure layers
assert_no_red layers
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 40 > "$UMBRIEL_RUNTIME_DIR/layer.log" 2>&1 &
layer_pid=$!
assert_transition layer-open
kill -TERM "$layer_pid"
assert_transition layer-close
