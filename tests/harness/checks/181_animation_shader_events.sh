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
duration_ms = 300
curve = "linear"
[animation.windows_in]
enabled = false
[animation.$1]
enabled = true
shader = "fixture-1.glsl"
EOF
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" clock-advance 1000
}

red_pixels() {
  grim "$IMAGE"
  "$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r > 0.9 && g < 0.1 && b < 0.1'
}
assert_no_red() {
  local count
  count=$(red_pixels)
  if (( count > 40 )); then
    echo "$1: stale shader color before transition or after completion ($count pixels)"
    exit 1
  fi
}
assert_transition() {
  local count
  "$UMBRIEL" clock-advance 100
  count=$(red_pixels)
  if (( count < 120 )); then
    echo "$1: missing shader-only intermediate color ($count pixels)"
    exit 1
  fi
  "$UMBRIEL" clock-advance 1000
  assert_no_red "$1"
  echo "$1 shader transition verified"
}

# Animation time only moves by clock-advance: each 300 ms transition is sampled 100 ms after its trigger, and
# advancing 1000 ms finishes it.
"$UMBRIEL" clock-freeze
# Layer mapping is client-driven, so the transition starts only once the compositor has seen it.
wait_for_mapped_layers() {
  for _ in $(seq 100); do
    [[ $("$UMBRIEL" layers --json | jq '[.[] | select(.mapped)] | length') == "$1" ]] && return 0
    sleep 0.025
  done
  echo "timed out waiting for $1 mapped layer surfaces"
  exit 1
}

configure windows_move
"$UMBRIEL_UNMAP_CLIENT" shader-events-a 700 700 > "$UMBRIEL_RUNTIME_DIR/a.log" 2>&1 &
"$UMBRIEL_UNMAP_CLIENT" shader-events-b 700 700 > "$UMBRIEL_RUNTIME_DIR/b.log" 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq length) == 2 ]] && break
  sleep 0.05
done
"$UMBRIEL" clock-advance 1000
first=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "shader-events-a") | .id')
second=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "shader-events-b") | .id')
assert_no_red move
"$UMBRIEL" msg window-toggle-maximize > /dev/null
assert_transition resize

configure border
"$UMBRIEL" msg "window-focus:$second" > /dev/null
"$UMBRIEL" clock-advance 1000
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
"$UMBRIEL" clock-advance 1000

configure overview
assert_no_red overview
"$UMBRIEL" msg overview-open > /dev/null
assert_transition overview
"$UMBRIEL" msg overview-close > /dev/null
assert_transition overview-close

configure scratchpad
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
"$UMBRIEL" clock-advance 1000
assert_no_red scratchpad
"$UMBRIEL" msg scratchpad-toggle > /dev/null
assert_transition scratchpad-show
"$UMBRIEL" msg scratchpad-toggle > /dev/null
assert_transition scratchpad-hide

configure layers
assert_no_red layers
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 40 > "$UMBRIEL_RUNTIME_DIR/layer.log" 2>&1 &
layer_pid=$!
wait_for_mapped_layers 1
assert_transition layer-open
kill -TERM "$layer_pid"
wait_for_mapped_layers 0
assert_transition layer-close
