#!/usr/bin/env bash
# A feedback animation enclosing a window effect keeps separate histories per composition role: with in_capture = false
# every captured frame excludes the window effect, the first captured frame starts from the capture's own input rather
# than the display's history, and the display's feedback matches a run without any capture in flight at the same clock
# steps.
set -euo pipefail
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/effect-capture-feedback.png"
cat > "$UMBRIEL_RUNTIME_DIR/accumulate.glsl" <<'GLSL'
// Adds 0.01 red once per animation instant: green holds the progress the history last saw, so repeated draws of one
// instant add nothing. Blue keeps the largest input blue this history has ever composited.
vec4 animation(vec2 uv) {
  vec4 p = umbriel_sample_previous(uv);
  float step = abs(p.g - umbriel_linear_progress) > 2.0 / 255.0 ? 0.01 : 0.0;
  return vec4(min(p.r + step, 1.0), umbriel_linear_progress, max(p.b, umbriel_sample(uv).b), 1.0);
}
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/green.glsl" <<'GLSL'
vec4 window(vec2 uv) { return vec4(0.0, 1.0, 0.0, 1.0); }
GLSL
readonly BASE="$UMBRIEL_RUNTIME_DIR/feedback-base.toml"
cp "$UMBRIEL_CONFIG" "$BASE"
write_config() {
  cat "$BASE" > "$UMBRIEL_CONFIG"
  cat >> "$UMBRIEL_CONFIG" <<EOF

[colors]
backdrop = "#000000FF"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[animation]
duration_ms = 4000
curve = "linear"
[animation.windows_in]
style = "none"
effect = "accumulate"
[effects]
window = "green"
in_capture = $1
[effects.preset.accumulate]
kind = "animation"
shader = "accumulate.glsl"
[effects.preset.green]
kind = "window"
shader = "green.glsl"
[[window_rule]]
match.title = "^feedback$"
default_floating = true
default_position = { x = 200, y = 150, anchor = "top_left" }
EOF
  "$UMBRIEL" msg config-reload > /dev/null
}
spawn() {
  FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" feedback 300 200 > "$UMBRIEL_RUNTIME_DIR/feedback.log" 2>&1 &
  client_pid=$!
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "feedback")')
    [[ -n $window ]] && break
    sleep 0.025
  done
  [[ -n $window ]] || { echo "the feedback client never mapped"; exit 1; }
  read -r x y w h id < <(jq -r '"\(.x) \(.y) \(.w) \(.h) \(.id)"' <<< "$window")
}
# unmap-client keeps its toplevel alive across a compositor close request, so a respawn under the same title must kill
# the process instead.
retire() {
  kill "$client_pid" 2>/dev/null || true
  wait "$client_pid" 2>/dev/null || true
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "feedback")')
    [[ -z $window ]] && break
    sleep 0.025
  done
  [[ -z $window ]] || { echo "the retired client's toplevel did not disappear"; exit 1; }
}
# The client is blue. The window effect paints it green; the enclosing accumulate program keeps red (instants
# accumulated) and blue (the most blue its history has seen). So: a history that ever composited an unfiltered frame
# shows blue > 0 (client seen), one that only saw filtered frames shows blue = 0 (green window seen), and red counts
# the animation instants the history has drawn.
centre_red() { "$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$((x + w / 2))" "$((y + h / 2))" | cut -d' ' -f1; }
centre_blue() { "$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$((x + w / 2))" "$((y + h / 2))" | cut -d' ' -f3; }
# Reads the display's accumulated red through a reload asserting in_capture = true, plus one clock-advance and one
# grim. That reload is a no-op in the reference run (already true) and flips the policy back in the capture run
# (redamaging and scheduling an extra frame there); either way it never touches display history, so the two
# readings below remain comparable.
read_display_red() {
  sed -i 's/^in_capture = false$/in_capture = true/' "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" clock-advance 1 > /dev/null
  grim "$IMAGE"
  centre_red
}

# Reference run with effects included in captures: the same clock steps and the same three grim calls as the capture
# run below (grim itself requests a frame; read_display_red adds one more) so the two runs' display histories stay
# comparable, even though the capture run's own read_display_red does extra work of its own (see above).
write_config true
"$UMBRIEL" clock-freeze
spawn
for _ in $(seq 4); do "$UMBRIEL" clock-advance 100 > /dev/null; done
grim "$IMAGE"
"$UMBRIEL" clock-advance 100 > /dev/null
grim "$IMAGE"
for _ in $(seq 2); do "$UMBRIEL" clock-advance 100 > /dev/null; done
grim "$IMAGE"
reference=$(read_display_red)
retire
"$UMBRIEL" clock-advance 8000 > /dev/null
"$UMBRIEL" settle > /dev/null

# Capture run with effects excluded from captures: the same steps; every grim is a pending capture that stops between
# the calls and restarts.
write_config false
spawn
for _ in $(seq 4); do "$UMBRIEL" clock-advance 100 > /dev/null; done
grim "$IMAGE"
first_capture_blue=$(centre_blue)
first_capture_red=$(centre_red)
"$UMBRIEL" clock-advance 100 > /dev/null
grim "$IMAGE"
second_capture_blue=$(centre_blue)
for _ in $(seq 2); do "$UMBRIEL" clock-advance 100 > /dev/null; done
grim "$IMAGE"
third_capture_blue=$(centre_blue)
if (( first_capture_blue < 200 || second_capture_blue < 200 || third_capture_blue < 200 )); then
  echo "captured frames included the window effect (client blue hidden): $first_capture_blue $second_capture_blue $third_capture_blue"
  exit 1
fi
# One instant adds about 2.55 red; by the first capture the display has drawn several.
if (( first_capture_red > 4 )); then
  echo "the first captured frame read the display's history: red $first_capture_red"
  exit 1
fi
# The display kept accumulating through its own history; the capture role kept its own.
display_red=$(read_display_red)
display_blue=$(centre_blue)
if (( display_blue > 15 )); then
  echo "the display's feedback history composited an unfiltered capture frame: blue $display_blue"
  exit 1
fi
# Encoding jitter plus one instant.
if (( display_red < reference - 4 || display_red > reference + 4 )); then
  echo "display feedback diverged from the capture-free run: $display_red vs $reference"
  exit 1
fi
echo "capture-role feedback isolation verified: display $display_red vs $reference, first capture $first_capture_red"
