#!/usr/bin/env bash
# A second scrolling-layout close can interrupt an active vacancy reflow without starving or reversing the surviving
# column. Each close keeps its own windows_out shader clock, while the survivor starts windows_move immediately and
# keeps one monotonic movement toward its unchanged target instead of restarting or waiting for both durations.
set -euo pipefail

readonly SHOTS="$UMBRIEL_RUNTIME_DIR/tiled-repeated-close"
readonly OUT_MS=1370
readonly MOVE_MS=830
readonly SECOND_CLOSE_MS=450
readonly COLUMN_WIDTH=576
readonly MARKER_PIXELS=100
readonly STEP_MS=40
mkdir -p "$SHOTS"

# The close snapshot holds its captured column for its whole lifecycle, so it paints only a top band and leaves the
# rest transparent. The survivor growing underneath stays measurable while each close keeps its own phase colours.
cat > "$UMBRIEL_RUNTIME_DIR/repeated-close-phases.glsl" <<'GLSL'
vec4 animation(vec2 uv) {
    if (uv.y > 0.08) {
        return vec4(0.0);
    }
    vec4 source = umbriel_sample(uv);
    bool green = source.g > source.b;
    float progress = umbriel_clamped_progress;
    if (progress < 0.3) {
        return green ? vec4(0.12, 1.0, 0.0, 1.0) : vec4(0.12, 0.0, 1.0, 1.0);
    }
    if (progress < 0.7) {
        return green ? vec4(0.45, 1.0, 0.0, 1.0) : vec4(0.45, 0.0, 1.0, 1.0);
    }
    return green ? vec4(0.0, 1.0, 1.0, 1.0) : vec4(1.0, 0.0, 1.0, 1.0);
}
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/repeated-move-marker.glsl" <<'GLSL'
vec4 animation(vec2 uv) {
    vec4 source = umbriel_sample(uv);
    return vec4(source.r, max(source.g, source.a * 0.35), source.b, source.a);
}
GLSL

cat >> "$UMBRIEL_CONFIG" <<EOF

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[layout]
mode = "scrolling"
gap = 0

[layout.scrolling]
default_extent_fraction = 0.45
center_focused = "never"
center_underfull_strip = false

[animation.windows_in]
enabled = false

[animation.windows_out]
enabled = true
duration_ms = $OUT_MS
curve = "linear"
shader = "repeated-close-phases.glsl"

[animation.windows_move]
enabled = true
duration_ms = $MOVE_MS
curve = "linear"
shader = "repeated-move-marker.glsl"

[animation.workspaces]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

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

window_id() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | .id'
}

red_bounds() {
  "$UMBRIEL_PIXEL_PROBE" "$1" bbox 'r > 0.7 && b < 0.2'
}

# One probe emits the move marker and all three phases for both independently coloured close snapshots.
frame_markers() {
  "$UMBRIEL_PIXEL_PROBE" "$1" count \
    'r > 0.7 && g > 0.2 && b < 0.2' \
    'b > 0.7 && r > 0.05 && r < 0.25 && g < 0.2' \
    'b > 0.7 && r > 0.25 && r < 0.7 && g < 0.2' \
    'b > 0.7 && r > 0.7 && g < 0.2' \
    'g > 0.7 && r > 0.05 && r < 0.25 && b < 0.2' \
    'g > 0.7 && r > 0.25 && r < 0.7 && b < 0.2' \
    'g > 0.7 && b > 0.7 && r < 0.2'
}

# Writes the move and phase marker counts and the survivor bounds of one frame. Frames are analyzed in the background
# while capture continues.
analyze_frame() {
  local image=$1
  {
    frame_markers "$image"
    red_bounds "$image"
  } > "$image.txt"
}

close_window() {
  local id=$1
  "$UMBRIEL" msg "window-close:$id" > /dev/null
  for _ in $(seq 100); do
    "$UMBRIEL" windows --json | jq -e --arg id "$id" 'any(.[]; .id == $id)' > /dev/null || return 0
    sleep 0.02
  done
  echo "window $id never left the compositor window list"
  return 1
}

assert_phase_timeline() {
  local label=$1 request_ms=$2 early_name=$3 middle_name=$4 late_name=$5
  local -n early_ref=$early_name middle_ref=$middle_name late_ref=$late_name
  local first_early=-1 first_middle=-1 first_late=-1 last_visible=-1 i
  for ((i = 0; i < frame_count; i++)); do
    ((sample_times[i] < request_ms)) && continue
    if ((early_ref[i] >= MARKER_PIXELS)); then
      ((first_early < 0)) && first_early=$i
      last_visible=$i
    fi
    if ((middle_ref[i] >= MARKER_PIXELS)); then
      ((first_middle < 0)) && first_middle=$i
      last_visible=$i
    fi
    if ((late_ref[i] >= MARKER_PIXELS)); then
      ((first_late < 0)) && first_late=$i
      last_visible=$i
    fi
  done
  if ((first_early < 0 || first_middle < 0 || first_late < 0)); then
    echo "$label: windows_out skipped a shader phase: early=$first_early middle=$first_middle late=$first_late"
    return 1
  fi
  if ((first_middle <= first_early || first_late <= first_middle)); then
    echo "$label: windows_out shader phases were out of order: early=$first_early middle=$first_middle late=$first_late"
    return 1
  fi

  local tolerance=$((2 * max_gap_ms + 120))
  local middle_elapsed=$((sample_times[first_middle] - request_ms))
  local late_elapsed=$((sample_times[first_late] - request_ms))
  local last_elapsed=$((sample_times[last_visible] - request_ms))
  local middle_expected=$((OUT_MS * 3 / 10))
  local late_expected=$((OUT_MS * 7 / 10))
  if ((middle_elapsed < middle_expected - tolerance || middle_elapsed > middle_expected + tolerance)); then
    echo "$label: windows_out middle phase started at ${middle_elapsed} ms, expected ${middle_expected} ms within ${tolerance} ms"
    return 1
  fi
  if ((late_elapsed < late_expected - tolerance || late_elapsed > late_expected + tolerance)); then
    echo "$label: windows_out late phase started at ${late_elapsed} ms, expected ${late_expected} ms within ${tolerance} ms"
    return 1
  fi
  if ((last_elapsed < OUT_MS - tolerance || last_elapsed > OUT_MS + tolerance)); then
    echo "$label: windows_out ended at ${last_elapsed} ms, expected ${OUT_MS} ms within ${tolerance} ms"
    return 1
  fi
  printf '%s: phases=%d/%d/%d ms end=%d ms\n' \
    "$label" "$((sample_times[first_early] - request_ms))" "$middle_elapsed" "$late_elapsed" "$last_elapsed"
}

# Animation time only moves by clock-advance. Advancing past MOVE_MS finishes each opening reflow.
"$UMBRIEL" clock-freeze
spawn repeated-survivor 0xFFFF0000
"$UMBRIEL" clock-advance "$((MOVE_MS + 100))"
spawn repeated-second-close 0xFF00FF00
"$UMBRIEL" clock-advance "$((MOVE_MS + 100))"
spawn repeated-first-close 0xFF0000FF
"$UMBRIEL" clock-advance "$((MOVE_MS + 100))"

readonly SECOND_ID=$(window_id repeated-second-close)
readonly FIRST_ID=$(window_id repeated-first-close)
if [[ -z $SECOND_ID || -z $FIRST_ID ]]; then
  echo "repeated close setup did not expose both closing windows"
  exit 1
fi

grim "$SHOTS/before.png"
read -r initial_x initial_y initial_width initial_height <<< "$(red_bounds "$SHOTS/before.png")"
if ((initial_x != 0 || initial_y != 0 || initial_width < 100 || initial_width >= COLUMN_WIDTH || initial_height != 720)); then
  echo "scrolling setup did not leave a partially visible survivor: +$initial_x +$initial_y $initial_width $initial_height"
  exit 1
fi

# Sample times are milliseconds after the first close unmapped, and the second close lands exactly SECOND_CLOSE_MS
# later.
first_request_ms=0
close_window "$FIRST_ID"
second_request_ms=0
frame_count=0
elapsed_ms=1
deadline_ms=$((SECOND_CLOSE_MS + OUT_MS + 400))
declare -a sample_times=() analyzers=()
"$UMBRIEL" clock-advance 1
while ((elapsed_ms <= deadline_ms)); do
  if ((second_request_ms == 0 && elapsed_ms >= SECOND_CLOSE_MS)); then
    close_window "$SECOND_ID"
    second_request_ms=$elapsed_ms
    deadline_ms=$((second_request_ms + OUT_MS + 400))
    "$UMBRIEL" clock-advance 1
    elapsed_ms=$((elapsed_ms + 1))
  fi
  grim "$SHOTS/frame-$frame_count.png"
  analyze_frame "$SHOTS/frame-$frame_count.png" &
  analyzers+=($!)
  sample_times[$frame_count]=$elapsed_ms
  frame_count=$((frame_count + 1))
  step_ms=$STEP_MS
  if ((second_request_ms == 0 && elapsed_ms < SECOND_CLOSE_MS && elapsed_ms + step_ms > SECOND_CLOSE_MS)); then
    step_ms=$((SECOND_CLOSE_MS - elapsed_ms))
  fi
  "$UMBRIEL" clock-advance "$step_ms"
  elapsed_ms=$((elapsed_ms + step_ms))
done
for analyzer in "${analyzers[@]}"; do
  wait "$analyzer"
done

if ((second_request_ms == 0)); then
  echo "second close was never requested"
  exit 1
fi
for title in repeated-first-close repeated-second-close; do
  if ! grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/$title.log"; then
    echo "$title did not unmap"
    exit 1
  fi
  if "$UMBRIEL" windows --json | jq -e --arg title "$title" 'any(.[]; .title == $title)' > /dev/null; then
    echo "$title remained in the compositor window list"
    exit 1
  fi
done

declare -a move_pixels=() blue_early=() blue_middle=() blue_late=()
declare -a green_early=() green_middle=() green_late=()
declare -a red_x=() red_y=() red_width=() red_height=()
max_gap_ms=$STEP_MS
for ((i = 0; i < frame_count; i++)); do
  {
    read -r move_pixels[$i] blue_early[$i] blue_middle[$i] blue_late[$i] \
      green_early[$i] green_middle[$i] green_late[$i]
    read -r red_x[$i] red_y[$i] red_width[$i] red_height[$i]
  } < "$SHOTS/frame-$i.png.txt"
done

assert_phase_timeline first-close "$first_request_ms" blue_early blue_middle blue_late
assert_phase_timeline second-close "$second_request_ms" green_early green_middle green_late

first_move_first=-1
first_move_last=-1
post_second_marker_first=-1
post_second_marker_last=-1
for ((i = 0; i < frame_count; i++)); do
  if ((move_pixels[i] < MARKER_PIXELS)); then
    continue
  fi
  if ((sample_times[i] < second_request_ms)); then
    ((first_move_first < 0)) && first_move_first=$i
    first_move_last=$i
  elif ((sample_times[i] >= second_request_ms + 100)); then
    ((post_second_marker_first < 0)) && post_second_marker_first=$i
    post_second_marker_last=$i
  fi
done
if ((first_move_first < 0 || first_move_last < 0)); then
  echo "first close never began windows_move before it was interrupted"
  exit 1
fi
if ((second_request_ms - sample_times[first_move_last] > 2 * max_gap_ms + 100)); then
  echo "second close did not interrupt an active windows_move: last marker was $((second_request_ms - sample_times[first_move_last])) ms earlier"
  exit 1
fi
if ((post_second_marker_first < 0 || post_second_marker_last < post_second_marker_first)); then
  echo "windows_move shader disappeared after the second close"
  exit 1
fi

timing_tolerance=$((2 * max_gap_ms + 120))
first_move_elapsed=$((sample_times[first_move_first] - first_request_ms))
if ((first_move_elapsed > timing_tolerance)); then
  echo "first windows_move began at ${first_move_elapsed} ms, expected it to start immediately within ${timing_tolerance} ms"
  exit 1
fi

pre_second_width=$initial_width
for ((i = 0; i < frame_count; i++)); do
  ((sample_times[i] >= second_request_ms)) && break
  pre_second_width=${red_width[$i]}
done
if ((pre_second_width < initial_width + 30)); then
  echo "second close did not interrupt observable survivor motion: initial=$initial_width before-second=$pre_second_width"
  exit 1
fi

previous_width=$initial_width
for ((i = 0; i < frame_count; i++)); do
  if ((red_x[i] != 0 || red_y[i] != 0 || red_height[i] != 720)); then
    echo "survivor left its scrolling lane at frame $i: +${red_x[$i]} +${red_y[$i]} ${red_width[$i]} ${red_height[$i]}"
    exit 1
  fi
  if ((red_width[i] + 4 < previous_width)); then
    echo "repeated close reversed survivor progress at frame $i: previous=$previous_width current=${red_width[$i]}"
    exit 1
  fi
  ((red_width[i] > previous_width)) && previous_width=${red_width[$i]}
done

second_anchor_frame=-1
for ((i = 0; i < frame_count; i++)); do
  if ((sample_times[i] >= second_request_ms + 20)); then
    second_anchor_frame=$i
    break
  fi
done
if ((second_anchor_frame < 0)); then
  echo "no survivor sample followed the second close"
  exit 1
fi
second_anchor_width=${red_width[$second_anchor_frame]}
rebased_move_first=-1
for ((i = second_anchor_frame + 1; i < frame_count; i++)); do
  if ((red_width[i] > second_anchor_width + 4)); then
    rebased_move_first=$i
    break
  fi
done
if ((rebased_move_first < 0)); then
  echo "survivor never resumed geometry motion after the second close: anchor=$second_anchor_width"
  exit 1
fi
rebased_move_elapsed=$((sample_times[rebased_move_first] - second_request_ms))
if ((rebased_move_elapsed > timing_tolerance)); then
  echo "rebased survivor geometry began at ${rebased_move_elapsed} ms, expected it to start immediately within ${timing_tolerance} ms"
  exit 1
fi

final_frame=-1
last_growth_ms=${sample_times[rebased_move_first]}
last_growth_width=${red_width[$rebased_move_first]}
stall_limit_ms=$((3 * max_gap_ms + 100))
for ((i = rebased_move_first; i < frame_count; i++)); do
  if ((red_width[i] >= COLUMN_WIDTH - 2)); then
    final_frame=$i
    break
  fi
  if ((red_width[i] > last_growth_width + 2)); then
    last_growth_width=${red_width[$i]}
    last_growth_ms=${sample_times[$i]}
  elif ((sample_times[i] - last_growth_ms > stall_limit_ms)); then
    echo "rebased survivor starved during windows_move at frame $i: width=${red_width[$i]} stalled=$((sample_times[i] - last_growth_ms)) ms"
    exit 1
  fi
done
if ((final_frame < 0)); then
  echo "survivor never reached its final scrolling geometry: last width=${red_width[$((frame_count - 1))]}"
  exit 1
fi
final_elapsed=$((sample_times[final_frame] - second_request_ms))
if ((final_elapsed > OUT_MS + timing_tolerance)); then
  echo "repeated close summed windows_out and windows_move delays: survivor settled after ${final_elapsed} ms"
  exit 1
fi
move_span=$((sample_times[final_frame] - first_request_ms))
if ((move_span < MOVE_MS - timing_tolerance || move_span > MOVE_MS + timing_tolerance)); then
  echo "survivor geometry ran for ${move_span} ms after the first close, expected ${MOVE_MS} ms within ${timing_tolerance} ms"
  exit 1
fi
marker_during_reflow=0
for ((i = rebased_move_first; i <= final_frame; i++)); do
  if ((move_pixels[i] >= MARKER_PIXELS)); then
    marker_during_reflow=1
    break
  fi
done
if ((marker_during_reflow == 0)); then
  echo "windows_move shader did not overlap rebased survivor geometry"
  exit 1
fi
last_frame=$((frame_count - 1))
if ((red_x[last_frame] != 0 || red_y[last_frame] != 0 \
    || red_width[last_frame] != COLUMN_WIDTH || red_height[last_frame] != 720)); then
  echo "survivor missed final scrolling geometry: +${red_x[$last_frame]} +${red_y[$last_frame]} ${red_width[$last_frame]} ${red_height[$last_frame]}"
  exit 1
fi

printf 'repeated scrolling close: first marker=%d ms, motion resumed=%d ms, settled=%d ms after the second close, survivor=%d..%d px\n' \
  "$first_move_elapsed" "$rebased_move_elapsed" "$final_elapsed" "$initial_width" "$COLUMN_WIDTH"
echo "interrupted scrolling closes preserved both shaders and one monotonic survivor reflow on a single move clock"
