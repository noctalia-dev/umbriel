#!/usr/bin/env bash
# Previous-result feedback must carry each rendered result into the next frame,
# begin from the current target, and remain isolated between simultaneous targets.
set -euo pipefail

readonly SHADER="${UMBRIEL_CONFIG%/*}/feedback.glsl"
readonly PASSTHROUGH="${UMBRIEL_CONFIG%/*}/feedback-passthrough.glsl"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/feedback.png"

cat > "$SHADER" <<'GLSL'
vec4 animation(vec2 uv) {
    vec4 source = umbriel_sample(uv);
    vec4 previous = umbriel_sample_previous(uv);
    if (uv.y < 0.2) {
        return previous;
    }
    if (uv.x >= 0.5) {
        return source;
    }
    float base = umbriel_size.x < 500.0 ? 0.05 : 0.55;
    if (umbriel_linear_progress < 0.2) {
        return vec4(base, 0.0, 0.0, 1.0);
    }
    if (umbriel_linear_progress < 0.4) {
        float red = abs(previous.r - base) < 0.03 ? base + 0.1 : previous.r;
        return vec4(red, 0.0, 0.0, 1.0);
    }
    if (umbriel_linear_progress < 0.6) {
        float red = abs(previous.r - (base + 0.1)) < 0.03 ? base + 0.2 : previous.r;
        return vec4(red, 0.0, 0.0, 1.0);
    }
    return vec4(previous.r, 0.0, 0.0, 1.0);
}
GLSL

cat > "$PASSTHROUGH" <<'GLSL'
vec4 animation(vec2 uv) { return umbriel_sample(uv); }
GLSL

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false

[animation]
enabled = true
duration_ms = 7000
curve = "linear"
[animation.windows_in]
style = "none"
shader = "feedback.glsl"
[animation.windows_out]
duration_ms = 1200
style = "fade"
shader = "feedback-passthrough.glsl"
[animation.windows_move]
enabled = false

[[window_rule]]
match.title = "^feedback-small$"
default_floating = true
default_position = { x = 80, y = 80, anchor = "top_left" }

[[window_rule]]
match.title = "^feedback-large$"
default_floating = true
default_position = { x = 700, y = 80, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

# Animation time only moves by clock-advance. Opening frames are drawn at 700, 2100, 3500, and 4550 ms of the 7000 ms
# timeline, one in each feedback phase; the closing sample lands 150 ms into its 1200 ms timeline.
"$UMBRIEL" clock-freeze
"$UMBRIEL_UNMAP_CLIENT" feedback-small 320 360 > "$UMBRIEL_RUNTIME_DIR/feedback-small.log" 2>&1 &
"$UMBRIEL_UNMAP_CLIENT" feedback-large 650 360 > "$UMBRIEL_RUNTIME_DIR/feedback-large.log" 2>&1 &

for _ in $(seq 80); do
  windows=$("$UMBRIEL" windows --json)
  small=$(jq -c '.[] | select(.title == "feedback-small")' <<< "$windows")
  large=$(jq -c '.[] | select(.title == "feedback-large")' <<< "$windows")
  [[ -n $small && -n $large ]] && break
  sleep 0.025
done
if [[ -z ${small:-} || -z ${large:-} ]]; then
  echo "feedback clients did not both map: ${windows:-}"
  exit 1
fi

"$UMBRIEL" clock-advance 700
"$UMBRIEL" clock-advance 1400
"$UMBRIEL" clock-advance 1400
"$UMBRIEL" clock-advance 1050
grim "$IMAGE"

sample() {
  local window=$1 x_fraction=$2 y_fraction=$3
  local x y width height sample_x sample_y
  read -r x y width height <<< "$(jq -r '[.x, .y, .w, .h] | @tsv' <<< "$window")"
  sample_x=$(awk -v x="$x" -v width="$width" -v fraction="$x_fraction" 'BEGIN { printf "%d", x + width * fraction }')
  sample_y=$(awk -v y="$y" -v height="$height" -v fraction="$y_fraction" 'BEGIN { printf "%d", y + height * fraction }')
  magick "$IMAGE" -crop "6x6+$((sample_x - 3))+$((sample_y - 3))" \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

read -r small_red small_green small_blue <<< "$(sample "$small" 0.25 0.55)"
if ! (( small_red > 52 && small_red < 76 && small_green < 8 && small_blue < 8 )); then
  echo "small target did not retain its two feedback transitions: $small_red $small_green $small_blue"
  exit 1
fi

read -r large_red large_green large_blue <<< "$(sample "$large" 0.25 0.55)"
if ! (( large_red > 180 && large_red < 204 && large_green < 8 && large_blue < 8 )); then
  echo "large target did not retain its independent feedback transitions: $large_red $large_green $large_blue"
  exit 1
fi

read -r source_red source_green source_blue <<< "$(sample "$small" 0.75 0.55)"
if ! (( source_red > 70 && source_red < 100 && source_green > 100 && source_green < 140 && source_blue > 150 && source_blue < 190 )); then
  echo "current-source control changed unexpectedly: $source_red $source_green $source_blue"
  exit 1
fi

read -r initial_red initial_green initial_blue <<< "$(sample "$small" 0.75 0.1)"
if ! (( initial_red > 70 && initial_red < 100 && initial_green > 100 && initial_green < 140 && initial_blue > 150 && initial_blue < 190 )); then
  echo "previous-sample initialization did not retain the current target: $initial_red $initial_green $initial_blue"
  exit 1
fi

small_id=$(jq -r .id <<< "$small")
"$UMBRIEL" msg "window-close:$small_id" > /dev/null
"$UMBRIEL" clock-advance 150
grim "$IMAGE"
read -r closing_red closing_green closing_blue <<< "$(sample "$small" 0.25 0.55)"
if ! (( closing_red > 52 && closing_red < 76 && closing_green < 8 && closing_blue < 8 )); then
  echo "closing snapshot did not inherit target feedback: $closing_red $closing_green $closing_blue"
  exit 1
fi

echo "target-local shader feedback, initialization, isolation, and snapshot transfer verified"
