#!/usr/bin/env bash
# A tiled close keeps its captured box for its whole windows_out while survivors begin windows_move at once. Both
# clocks keep their configured durations for every duration ordering.
set -euo pipefail

readonly SHOTS="$UMBRIEL_RUNTIME_DIR/tiled-close-duration-alignment"
readonly MARKER_PIXELS=1000
mkdir -p "$SHOTS"

cat > "$UMBRIEL_RUNTIME_DIR/duration-close-green.glsl" <<'GLSL'
vec4 animation(vec2 uv) {
    return vec4(0.0, 1.0, 0.0, 1.0);
}
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/duration-move-blue.glsl" <<'GLSL'
vec4 animation(vec2 uv) {
    return vec4(0.0, 0.0, 1.0, 1.0);
}
GLSL

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[layout]
mode = "master"
gap = 0

[layout.master]
default_width_fraction = 0.5

[animation.windows_in]
enabled = false

[animation.windows_out]
enabled = true
duration_ms = 1200 # close-duration
curve = "linear"
shader = "duration-close-green.glsl"

[animation.windows_move]
enabled = true
duration_ms = 600 # move-duration
curve = "linear"
shader = "duration-move-blue.glsl"

[animation.workspaces]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

# Animation time only moves by clock-advance. Frames are sampled every STEP_MS from the frozen instant the closer
# unmapped, so each sample time is exact.
readonly STEP_MS=50
"$UMBRIEL" clock-freeze

# Writes the green, blue, and red pixel counts and the green bounding box of one frame. Frames are analyzed in the
# background while capture continues.
analyze_frame() {
  local image=$1
  {
    "$UMBRIEL_PIXEL_PROBE" "$image" count \
      'g > 0.8 && r < 0.2 && b < 0.2' 'b > 0.8 && r < 0.2 && g < 0.2' 'r > 0.8 && g < 0.2 && b < 0.2'
    "$UMBRIEL_PIXEL_PROBE" "$image" bbox 'g > 0.8 && r < 0.2 && b < 0.2'
  } > "$image.txt"
}

spawn() {
  local title=$1 color=$2
  FILL_COLOR="$color" "$UMBRIEL_UNMAP_CLIENT" "$title" 1280 720 \
    > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 100); do
    window=$("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)')
    [[ -n $window ]] && return 0
    sleep 0.025
  done
  echo "timed out waiting for $title"
  return 1
}

set_durations() {
  local close_ms=$1 move_ms=$2
  sed -i \
    -e "s/^duration_ms = [0-9][0-9]* # close-duration$/duration_ms = $close_ms # close-duration/" \
    -e "s/^duration_ms = [0-9][0-9]* # move-duration$/duration_ms = $move_ms # move-duration/" \
    "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  sleep 0.15
}

assert_near() {
  local phase=$1 label=$2 actual=$3 expected=$4 tolerance=$5
  local difference=$((actual - expected))
  ((difference < 0)) && difference=$((-difference))
  if ((difference > tolerance)); then
    echo "$phase: $label was ${actual} ms, expected ${expected} ms within ${tolerance} ms"
    return 1
  fi
}

run_case() {
  local phase=$1 workspace=$2 close_ms=$3 move_ms=$4
  local survivor_title="duration-$phase-survivor"
  local closer_title="duration-$phase-close"
  local max_duration=$close_ms
  ((move_ms > max_duration)) && max_duration=$move_ms

  set_durations "$close_ms" "$move_ms"
  "$UMBRIEL" msg "workspace-switch:$workspace" > /dev/null
  "$UMBRIEL" clock-advance 100

  spawn "$survivor_title" 0xFFFF0000
  spawn "$closer_title" 0xFF000000
  local closer_id
  closer_id=$(jq -r .id <<< "$window")

  # Opening the second tile also reflows the survivor. Let that independent movement finish before measuring close.
  "$UMBRIEL" clock-advance "$((move_ms + 200))"

  "$UMBRIEL" msg "window-close:$closer_id" > /dev/null
  for _ in $(seq 100); do
    "$UMBRIEL" windows --json | jq -e --arg title "$closer_title" 'any(.[]; .title == $title)' > /dev/null || break
    sleep 0.02
  done

  local elapsed=1 count=0
  local -a sample_times=() analyzers=()
  "$UMBRIEL" clock-advance 1
  while ((elapsed <= max_duration + 300)); do
    grim "$SHOTS/$phase-$count.png"
    analyze_frame "$SHOTS/$phase-$count.png" &
    analyzers+=($!)
    sample_times[$count]=$elapsed
    count=$((count + 1))
    "$UMBRIEL" clock-advance "$STEP_MS"
    elapsed=$((elapsed + STEP_MS))
  done
  local analyzer
  for analyzer in "${analyzers[@]}"; do
    wait "$analyzer"
  done

  if ! grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/$closer_title.log"; then
    echo "$phase: closing client did not unmap"
    return 1
  fi
  if "$UMBRIEL" windows --json | jq -e --arg title "$closer_title" \
      'any(.[]; .title == $title)' > /dev/null; then
    echo "$phase: compositor retained the closing client"
    return 1
  fi

  local i green blue red
  local close_first=-1 close_last=-1 move_first=-1 move_last=-1
  local final_green=0 final_blue=0 final_red=0
  local bounds_x=0 bounds_y=0 bounds_w=0 bounds_h=0 bx by bw bh
  for ((i = 0; i < count; i++)); do
    { read -r green blue red; read -r bx by bw bh; } < "$SHOTS/$phase-$i.png.txt"
    final_green=$green
    final_blue=$blue
    final_red=$red
    if ((green >= MARKER_PIXELS)); then
      if ((close_first < 0)); then
        close_first=$i
        bounds_x=$bx
        bounds_y=$by
        bounds_w=$bw
        bounds_h=$bh
      elif ((bx - bounds_x > 2 || bounds_x - bx > 2
             || by - bounds_y > 2 || bounds_y - by > 2
             || bw - bounds_w > 2 || bounds_w - bw > 2
             || bh - bounds_h > 2 || bounds_h - bh > 2)); then
        echo "$phase: close snapshot changed geometry at frame $i: $bx $by $bw $bh, expected $bounds_x $bounds_y $bounds_w $bounds_h"
        return 1
      fi
      close_last=$i
    fi
    if ((blue >= MARKER_PIXELS)); then
      ((move_first < 0)) && move_first=$i
      move_last=$i
    fi
  done

  if ((close_first < 0 || move_first < 0)); then
    echo "$phase: animation marker missing: close=$close_first move=$move_first"
    return 1
  fi

  local tolerance=$((2 * STEP_MS + 60))
  ((tolerance < 140)) && tolerance=140
  local close_span=$((sample_times[close_last] - sample_times[close_first]))
  local move_span=$((sample_times[move_last] - sample_times[move_first]))
  local start_offset=$((sample_times[move_first] - sample_times[close_first]))
  local end_offset=$((sample_times[move_last] - sample_times[close_last]))
  local first_time=${sample_times[close_first]}
  ((sample_times[move_first] < first_time)) && first_time=${sample_times[move_first]}
  local last_time=${sample_times[close_last]}
  ((sample_times[move_last] > last_time)) && last_time=${sample_times[move_last]}
  local total_span=$((last_time - first_time))
  local expected_start=0
  local expected_end=$((move_ms - close_ms))

  assert_near "$phase" "windows_out span" "$close_span" "$close_ms" "$tolerance"
  assert_near "$phase" "windows_move span" "$move_span" "$move_ms" "$tolerance"
  assert_near "$phase" "windows_move start offset" "$start_offset" "$expected_start" "$tolerance"
  assert_near "$phase" "windows_move end offset" "$end_offset" "$expected_end" "$tolerance"
  assert_near "$phase" "total transition span" "$total_span" "$max_duration" "$tolerance"

  if ((final_green >= MARKER_PIXELS || final_blue >= MARKER_PIXELS || final_red < 800000)); then
    echo "$phase: final frame did not contain one settled full-width survivor: red=$final_red green=$final_green blue=$final_blue"
    return 1
  fi

  printf '%s: close=%d ms move=%d ms start=%d ms end=%d ms total=%d ms tolerance=%d ms\n' \
    "$phase" "$close_span" "$move_span" "$start_offset" "$end_offset" "$total_span" "$tolerance"
}

run_case long-close 1 1200 600
run_case equal 2 900 900
run_case long-move 3 600 1200

echo "close snapshot held its captured box while survivors moved immediately, for every duration ordering"
