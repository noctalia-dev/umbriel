#!/usr/bin/env bash
# A shader seed must expose four stable channels and refresh when the same view
# begins another transition.
set -euo pipefail

readonly SHADER="${UMBRIEL_CONFIG%/*}/seed.glsl"
readonly PASSTHROUGH="${UMBRIEL_CONFIG%/*}/seed-passthrough.glsl"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/seed.png"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/seed-client.log"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/seed-control"

cat > "$SHADER" <<'GLSL'
vec4 animation(vec2 uv) {
    float value = uv.x < 0.25 ? umbriel_random_seed.x
        : uv.x < 0.5 ? umbriel_random_seed.y
        : uv.x < 0.75 ? umbriel_random_seed.z
        : umbriel_random_seed.w;
    return vec4(value, value, value, 1.0);
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
duration_ms = 1800
curve = "linear"
[animation.windows_in]
style = "none"
shader = "seed.glsl"
[animation.windows_out]
duration_ms = 900
style = "fade"
shader = "seed-passthrough.glsl"
[animation.windows_move]
enabled = false

[[window_rule]]
match.title = "^shader-seed$"
default_floating = true
default_position = { x = 300, y = 180, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

# Animation time only moves by clock-advance: samples land 150 ms and 500 ms into the 1800 ms opening timeline and
# 150 ms into the 900 ms closing one.
"$UMBRIEL" clock-freeze

mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
REMAP_ON_STDIN=1 "$UMBRIEL_UNMAP_CLIENT" shader-seed 800 420 \
  <&"$control_fd" > "$CLIENT_LOG" 2>&1 &

wait_for_map() {
  local count=$1
  for _ in $(seq 80); do
    window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "shader-seed")')
    [[ $(grep -c '^mapped$' "$CLIENT_LOG" || true) -eq $count && -n $window ]] && return 0
    sleep 0.025
  done
  echo "shader seed client did not reach map $count: $(< "$CLIENT_LOG")"
  exit 1
}

sample_seed() {
  local x y width height sample_x sample_y values=() fraction
  read -r x y width height <<< "$(jq -r '[.x, .y, .w, .h] | @tsv' <<< "$window")"
  sample_y=$((y + height / 2))
  grim "$IMAGE"
  for fraction in 0.125 0.375 0.625 0.875; do
    sample_x=$(awk -v x="$x" -v width="$width" -v fraction="$fraction" 'BEGIN { printf "%d", x + width * fraction }')
    values+=("$(magick "$IMAGE" -crop "6x6+$((sample_x - 3))+$((sample_y - 3))" \
      -format '%[fx:round(255*mean.r)]' info:)")
  done
  echo "${values[*]}"
}

wait_for_map 1
"$UMBRIEL" clock-advance 150
first=$(sample_seed)
"$UMBRIEL" clock-advance 350
first_later=$(sample_seed)
if [[ $first != "$first_later" ]]; then
  echo "shader seed changed during one transition: $first then $first_later"
  exit 1
fi

window_id=$(jq -r .id <<< "$window")
"$UMBRIEL" msg "window-close:$window_id" > /dev/null
"$UMBRIEL" clock-advance 150
closing=$(sample_seed)
if [[ $closing != "$first" ]]; then
  echo "closing snapshot did not preserve the opening seed: $first then $closing"
  exit 1
fi
for _ in $(seq 80); do
  [[ $(grep -c '^unmapped$' "$CLIENT_LOG" || true) -eq 1 ]] && break
  sleep 0.025
done
if [[ $(grep -c '^unmapped$' "$CLIENT_LOG" || true) -ne 1 ]]; then
  echo "shader seed client did not unmap: $(< "$CLIENT_LOG")"
  exit 1
fi

"$UMBRIEL" clock-advance 1800
"$UMBRIEL" settle
printf r >&"$control_fd"
wait_for_map 2
"$UMBRIEL" clock-advance 150
second=$(sample_seed)
"$UMBRIEL" clock-advance 350
second_later=$(sample_seed)
if [[ $second != "$second_later" ]]; then
  echo "shader seed changed during the second transition: $second then $second_later"
  exit 1
fi
if [[ $first == "$second" ]]; then
  echo "shader seed was reused by the next transition: $first"
  exit 1
fi

echo "four-channel shader seed stability and renewal verified"
