#!/usr/bin/env bash
# Established tiled peers share one windows_move transition and remain disjoint through interrupted geometry changes.
# Lifecycle actors are deliberately outside this contract, so opens and closes settle before sampling. Every window is
# translucent red over black with a green border ring. Two content layers raise red above 0.56, while a border crossing
# content mixes red with green. The windows_move shader writes the source alpha into blue, which proves the sampled
# transition really ran. The maximized window is drawn at a lower alpha than its peers, because its resize crossfade
# carries the windows_move shader too: only blue at the peers' alpha proves the peers themselves moved.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly SHOTS="$UMBRIEL_RUNTIME_DIR/layout-motion"
mkdir -p "$SHOTS"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "scrolling"

# Half-width columns make maximizing reflow visible neighbours.
[layout.scrolling]
default_extent_fraction = 0.5

[animation]
duration_ms = 1500
curve = "linear"

[animation.windows_in]
style = "popin"

[animation.windows_move]
shader = "layout-motion.glsl"

[appearance]
border_width = 2
outer_border_width = 8
corner_radius = 0

[colors.border]
focused = "#00FF00FF"
unfocused = "#00FF00FF"
outer = "#00FF00FF"

[appearance.shadow]
enabled = false
EOF
cat > "$UMBRIEL_RUNTIME_DIR/layout-motion.glsl" <<'GLSL'
vec4 animation(vec2 uv) {
    vec4 source = umbriel_sample(uv);
    return vec4(source.r, source.g, source.a, source.a);
}
GLSL
"$UMBRIEL" msg config-reload > /dev/null
# Animation time only moves by clock-advance.
"$UMBRIEL" clock-freeze

spawn() {
  local fill=0x80800000
  [[ $1 == *-a ]] && fill=0x60600000
  FILL_COLOR=$fill "$CLIENT" "$1" 1200 700 > "$SHOTS/$1.log" 2>&1 &
}

wait_for_windows() {
  local want=$1
  for _ in $(seq 80); do
    if [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]]; then
      return 0
    fi
    sleep 0.05
  done
  echo "timed out waiting for $want window(s), saw: $("$UMBRIEL" windows --json)"
  return 1
}

window_id() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | .id'
}

# Runs every animation to its end. A crossfade starts only once its client commits the resized buffer, which happens in
# real time, so advance until a settle probe succeeds.
finish() {
  for _ in $(seq 20); do
    "$UMBRIEL" clock-advance 2000 > /dev/null
    if timeout 0.3 "$UMBRIEL" settle > /dev/null 2>&1; then
      return 0
    fi
  done
  echo "animations never finished: $("$UMBRIEL" windows --json)"
  return 1
}

# Fraction of the output covered by pixels only two red layers, or a ring over red content, can produce.
overlap_pixels() {
  "$UMBRIEL_PIXEL_PROBE" "$1" count '(r > 0.56 && g < 0.1) || (r > 0.1 && g > 0.2)'
}

# Peers are drawn at alpha 0.5 and the maximized window at 0.375, so the marker blue tells them apart.
move_marker_pixels() {
  "$UMBRIEL_PIXEL_PROBE" "$1" count 'b > 0.44 && r > 0.2'
}

# Fourteen frames 100 ms of animation time apart, from the trigger on, so the samples span the whole 1500 ms motion.
sample() {
  local phase=$1
  local i
  local saw_marker=0
  for i in $(seq 14); do
    grim "$SHOTS/$phase-$i.png"
    "$UMBRIEL" clock-advance 100 > /dev/null
  done
  for i in $(seq 14); do
    local overlap
    overlap=$(overlap_pixels "$SHOTS/$phase-$i.png")
    if ((overlap > 9)); then
      echo "$phase: frame $i shows overlapping tiles ($overlap pixels): $SHOTS/$phase-$i.png"
      exit 1
    fi
    if (( $(move_marker_pixels "$SHOTS/$phase-$i.png") > 1000 )); then
      saw_marker=1
    fi
  done
  if ((!saw_marker)); then
    echo "$phase: the established peers never carried the windows_move shader during the transition"
    exit 1
  fi
}

close_all() {
  local id
  for id in $("$UMBRIEL" windows --json | jq -r '.[].id'); do
    "$UMBRIEL" msg "window-close:$id" > /dev/null
  done
}

run_mode() {
  local mode=$1
  local extent=$2
  local tag="$mode-$extent"
  sed -i -e "s/^mode = \"[a-z]*\"$/mode = \"$mode\"/" -e "s/^default_extent_fraction = .*$/default_extent_fraction = $extent/" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  finish

  spawn "motion-$tag-a"
  wait_for_windows 1
  finish

  spawn "motion-$tag-b"
  wait_for_windows 2
  finish

  spawn "motion-$tag-c"
  wait_for_windows 3
  finish

  "$UMBRIEL" msg "window-focus:$(window_id "motion-$tag-a")" > /dev/null
  "$UMBRIEL" msg window-toggle-maximize > /dev/null
  "$UMBRIEL" clock-advance 300 > /dev/null
  "$UMBRIEL" msg window-toggle-maximize > /dev/null
  sample "$tag-maximize-interrupt"
  finish

  close_all
  wait_for_windows 0
  finish
}

run_mode scrolling 0.5
run_mode dwindle 0.5
run_mode master 0.5

echo "established tiled peers used windows_move and remained disjoint in scrolling, dwindle, and master"
