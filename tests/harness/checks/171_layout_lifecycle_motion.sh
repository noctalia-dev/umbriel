#!/usr/bin/env bash
# Opening and closing a tile must keep it edge-locked to the tile whose layout geometry changes beside it. The blue
# lifecycle shader is translucent over an opaque red neighbour: any spatial overlap produces a purple sample.
# Scrolling translation, Dwindle split resizing, and Master area resizing are observed in the middle of the real
# render transition.
set -euo pipefail

readonly CLIENT="$UMBRIEL_UNMAP_CLIENT"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/lifecycle-motion.png"
readonly SHADER="$UMBRIEL_RUNTIME_DIR/lifecycle-blue.glsl"
readonly MOVE_SHADER="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../examples/shaders" && pwd)/squash.glsl"

cat > "$SHADER" <<'GLSL'
vec4 animation(vec2 uv) {
    float visible = umbriel_direction > 0.0
        ? umbriel_clamped_progress : 1.0 - umbriel_clamped_progress;
    float edge = mix(-0.02, 1.02, visible);
    float mask = 1.0 - smoothstep(edge - 0.02, edge + 0.02, uv.x);
    return vec4(0.0, 0.0, 0.5, 0.5) * mask;
}
GLSL

cat >> "$UMBRIEL_CONFIG" <<EOF

[colors]
background = "#000000FF"

[appearance]
border_width = 2
outer_border_width = 13
corner_radius = 0

[layout]
mode = "scrolling"
gap = 5

[layout.scrolling]
default_extent_fraction = 0.5

[animation]
duration_ms = 1200
curve = "window_flow"

[animation.beziers]
window_flow = [0.25, 0.46, 0.35, 1.0]

[animation.windows_in]
style = "fade"
shader = "lifecycle-blue.glsl"

[animation.windows_out]
style = "fade"
shader = "lifecycle-blue.glsl"

[animation.windows_move]
duration_ms = 1200
curve = "window_flow"
shader = "$MOVE_SHADER"
EOF
"$UMBRIEL" msg config-reload > /dev/null

wait_for_count() {
  local want=$1
  for _ in $(seq 80); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.025
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

spawn_red() {
  FILL_COLOR=0xFFFF0000 "$CLIENT" "$1" 1200 700 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
}

assert_no_overlap() {
  local label=$1 overlap
  grim "$SHOT"
  overlap=$(magick "$SHOT" \
    -fx 'r > 0.2 && r < 0.8 && g < 0.15 && b > 0.2 ? 1 : 0' \
    -format '%[fx:mean]' info:)
  if awk -v overlap="$overlap" 'BEGIN { exit !(overlap > 0.00001) }'; then
    echo "$label: translucent blue lifecycle content overlaps an opaque red tile ($overlap of the output)"
    return 1
  fi
}

assert_no_overlap_during() {
  local label=$1
  for frame in $(seq 1 5); do
    assert_no_overlap "$label frame $frame" || return 1
    sleep 0.04
  done
}

run_layout() {
  local mode=$1
  local first="motion-$mode-first"
  local second="motion-$mode-second"

  spawn_red "$first"
  wait_for_count 1
  sleep 1.35

  spawn_red "$second"
  wait_for_count 2
  assert_no_overlap_during "$mode open overlap"

  sleep 0.7
  local third="motion-$mode-third"
  spawn_red "$third"
  wait_for_count 3
  assert_no_overlap_during "$mode three-window open overlap"

  sleep 0.7
  local third_id
  third_id=$("$UMBRIEL" windows --json | jq -r --arg title "$third" '.[] | select(.title == $title) | .id')
  "$UMBRIEL" msg "window-close:$third_id" > /dev/null
  wait_for_count 2
  assert_no_overlap_during "$mode three-window close overlap"

  sleep 0.7
  local second_id
  second_id=$("$UMBRIEL" windows --json | jq -r --arg title "$second" '.[] | select(.title == $title) | .id')
  "$UMBRIEL" msg "window-close:$second_id" > /dev/null
  wait_for_count 1
  assert_no_overlap_during "$mode close overlap"

  sleep 0.7
  local first_id
  first_id=$("$UMBRIEL" windows --json | jq -r --arg title "$first" '.[] | select(.title == $title) | .id')
  "$UMBRIEL" msg "window-close:$first_id" > /dev/null
  wait_for_count 0
  sleep 1.3
}

run_dwindle_subtree() {
  local titles=(dwindle-tree-first dwindle-tree-second dwindle-tree-third dwindle-tree-fourth)
  local control_fifo="$UMBRIEL_RUNTIME_DIR/dwindle-tree-control"
  local control_log="$UMBRIEL_RUNTIME_DIR/${titles[0]}.log"
  local control_fd
  mkfifo "$control_fifo"
  exec {control_fd}<>"$control_fifo"
  local count=0
  for title in "${titles[@]}"; do
    if ((count == 0)); then
      env FILL_COLOR=0xFFFF0000 MAXIMIZE_ON_STDIN=1 \
        "$CLIENT" "$title" 1200 700 <&"$control_fd" > "$control_log" 2>&1 &
    else
      spawn_red "$title"
    fi
    count=$((count + 1))
    wait_for_count "$count"
    if ((count == 4)); then
      # A redundant client restore requests an unanimated arrange while the fourth tile is opening. Its layout target
      # is unchanged, so this pass must not snap the edge-locked position while the paired size animation continues.
      printf r >&"$control_fd"
      for _ in $(seq 1 40); do
        grep -q '^unmaximize-requested$' "$control_log" && break
        sleep 0.01
      done
      if ! grep -q '^unmaximize-requested$' "$control_log"; then
        echo "dwindle control client did not issue its redundant restore: $(< "$control_log")"
        return 1
      fi
      assert_no_overlap_during "dwindle nested-subtree open overlap"
    fi
    sleep 1.25
  done

  local first_id
  first_id=$("$UMBRIEL" windows --json | jq -r --arg title "${titles[0]}" '.[] | select(.title == $title) | .id')
  "$UMBRIEL" msg "window-close:$first_id" > /dev/null
  wait_for_count 3
  assert_no_overlap_during "dwindle nested-subtree close overlap"
  sleep 1.25
  exec {control_fd}>&-
}

run_layout scrolling
sed -i 's/mode = "scrolling"/mode = "dwindle"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
run_layout dwindle
sed -i 's/mode = "dwindle"/mode = "master"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
run_layout master
sed -i 's/mode = "master"/mode = "dwindle"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
run_dwindle_subtree

echo "scrolling, Dwindle, and Master lifecycle motion kept adjacent tiles disjoint"
