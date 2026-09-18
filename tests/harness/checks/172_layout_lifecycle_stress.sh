#!/usr/bin/env bash
# Slow tiled reflows must remain disjoint when maps and closes arrive before earlier transitions settle. Distinct
# translucent client, opening, and closing colors expose every pairwise presentation overlap.
set -euo pipefail

readonly CLIENT="$UMBRIEL_UNMAP_CLIENT"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/lifecycle-stress.png"
readonly OPEN_SHADER="$UMBRIEL_RUNTIME_DIR/lifecycle-stress-blue.glsl"
readonly CLOSE_SHADER="$UMBRIEL_RUNTIME_DIR/lifecycle-stress-green.glsl"
readonly MOVE_SHADER="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../examples/shaders" && pwd)/squash.glsl"

cat > "$OPEN_SHADER" <<'GLSL'
vec4 animation(vec2 uv) {
    float visible = umbriel_direction > 0.0
        ? umbriel_clamped_progress : 1.0 - umbriel_clamped_progress;
    float edge = mix(-0.02, 1.02, visible);
    float mask = 1.0 - smoothstep(edge - 0.02, edge + 0.02, uv.x);
    return vec4(0.0, 0.0, 0.5, 0.5) * mask;
}
GLSL

cat > "$CLOSE_SHADER" <<'GLSL'
vec4 animation(vec2 uv) {
    float visible = umbriel_direction > 0.0
        ? umbriel_clamped_progress : 1.0 - umbriel_clamped_progress;
    float edge = mix(-0.02, 1.02, visible);
    float mask = 1.0 - smoothstep(edge - 0.02, edge + 0.02, uv.x);
    return vec4(0.0, 0.5, 0.0, 0.5) * mask;
}
GLSL

cat >> "$UMBRIEL_CONFIG" <<EOF

[colors]
background = "#000000FF"

[colors.border]
focused = "#000000FF"
unfocused = "#000000FF"
outer = "#000000FF"

[appearance]
border_width = 2
outer_border_width = 13
corner_radius = 0

[layout]
mode = "dwindle"
gap = 5

[animation]
duration_ms = 3000
curve = "window_flow"

[animation.beziers]
window_flow = [0.25, 0.46, 0.35, 1.0]

[animation.windows_in]
duration_ms = 3000
curve = "easeoutcubic"
style = "fade"
shader = "lifecycle-stress-blue.glsl"

[animation.windows_out]
duration_ms = 3000
curve = "easeoutcubic"
style = "fade"
shader = "lifecycle-stress-green.glsl"

[animation.windows_move]
duration_ms = 3000
curve = "window_flow"
shader = "$MOVE_SHADER"
EOF
"$UMBRIEL" msg config-reload > /dev/null

wait_for_count() {
  local want=$1
  for _ in $(seq 1 120); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.025
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

spawn_red() {
  local title=$1
  FILL_COLOR=0x80800000 "$CLIENT" "$title" 1200 700 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
}

close_title() {
  local title=$1 id
  id=$("$UMBRIEL" windows --json | jq -r --arg title "$title" '.[] | select(.title == $title) | .id')
  if [[ -z $id ]]; then
    echo "could not find stress window '$title'"
    return 1
  fi
  "$UMBRIEL" msg "window-close:$id" > /dev/null
}

assert_no_overlap() {
  local label=$1 overlap
  grim "$SHOT"
  overlap=$(magick "$SHOT" \
    -fx '(r > 0.65 && g < 0.1 && b < 0.1) || (r < 0.1 && g > 0.65 && b < 0.1) || (r < 0.1 && g < 0.1 && b > 0.65) || (r > 0.2 && g > 0.2 && b < 0.15) || (r > 0.2 && g < 0.15 && b > 0.2) || (r < 0.15 && g > 0.15 && b > 0.15) ? 1 : 0' \
    -format '%[fx:mean]' info:)
  if awk -v overlap="$overlap" 'BEGIN { exit !(overlap > 0.00001) }'; then
    echo "$label: lifecycle presentations overlap ($overlap of the output)"
    return 1
  fi
}

run_burst() {
  local mode=$1
  local second="$mode-stress-second"
  local third="$mode-stress-third"
  local fourth="$mode-stress-fourth"
  local fifth="$mode-stress-fifth"

  spawn_red "$second"
  wait_for_count 2
  sleep 0.2
  spawn_red "$third"
  wait_for_count 3
  sleep 0.2
  close_title "$second"
  wait_for_count 2
  sleep 0.2
  spawn_red "$fourth"
  wait_for_count 3
  sleep 0.2
  close_title "$third"
  wait_for_count 2
  sleep 0.2
  spawn_red "$fifth"
  wait_for_count 3
  sleep 0.2
  close_title "$fourth"
  wait_for_count 2
}

run_layout_stress() {
  local mode=$1
  spawn_red "$mode-stress-first"
  wait_for_count 1
  sleep 3.2

  run_burst "$mode" &
  local burst_pid=$!
  for frame in $(seq 1 18); do
    assert_no_overlap "$mode lifecycle stress frame $frame" || {
      wait "$burst_pid" || true
      return 1
    }
    sleep 0.04
  done
  wait "$burst_pid"

  local id
  while read -r id; do
    "$UMBRIEL" msg "window-close:$id" > /dev/null
  done < <("$UMBRIEL" windows --json | jq -r '.[].id')
  wait_for_count 0
  sleep 3.2
}

run_layout_stress dwindle
sed -i 's/mode = "dwindle"/mode = "master"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
run_layout_stress master

echo "slow Dwindle and Master lifecycle bursts kept every opening and closing presentation disjoint"
