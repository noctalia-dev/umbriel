#!/usr/bin/env bash
# A card admitted while the overview is open follows the same reveal as a normal-mode opener: fading in on windows_in
# while the neighbour reflows on windows_move. Closing a card during tiled reflow must discard
# the copied windows_move effect before windows_out runs. The move shader paints the live blue card red. The close
# shader then paints its snapshot green only when it samples the original blue client, or magenta when the stale
# move shader is still composed into the snapshot.
set -euo pipefail

readonly FIRST_LOG="$UMBRIEL_RUNTIME_DIR/overview-close-first.log"
readonly SECOND_LOG="$UMBRIEL_RUNTIME_DIR/overview-close-second.log"
readonly MOVING="$UMBRIEL_RUNTIME_DIR/overview-close-moving.png"
readonly DURING="$UMBRIEL_RUNTIME_DIR/overview-close-during.png"
readonly OPENED="$UMBRIEL_RUNTIME_DIR/overview-close-opened.png"
readonly AFTER="$UMBRIEL_RUNTIME_DIR/overview-close-after.png"

cat > "$UMBRIEL_RUNTIME_DIR/overview-move.glsl" <<'GLSL'
vec4 animation(vec2 uv) { return vec4(1.0, 0.0, 0.0, 1.0); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/overview-close.glsl" <<'GLSL'
vec4 animation(vec2 uv) {
    vec4 source = umbriel_sample(uv);
    return source.b > 0.5 ? vec4(0.0, 1.0, 0.0, 1.0) : vec4(1.0, 0.0, 1.0, 1.0);
}
GLSL

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "master"

[animation]
curve = "linear"

[animation.windows_in]
enabled = true
duration_ms = 600
style = "fade"

[animation.windows_out]
enabled = true
duration_ms = 1000
shader = "overview-close.glsl"

[animation.windows_move]
enabled = true
duration_ms = 1600
shader = "overview-move.glsl"

[colors]
backdrop = "#000000FF"

[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[overview]
zoom = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

color_pixels() {
  local image=$1 expression=$2
  "$UMBRIEL_PIXEL_PROBE" "$image" count "$expression"
}

FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" overview-close-first 1200 700 > "$FIRST_LOG" 2>&1 &
for _ in $(seq 80); do
  first=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "overview-close-first")')
  [[ -n $first ]] && break
  sleep 0.025
done
if [[ -z ${first:-} ]]; then
  echo "first overview close client never mapped"
  exit 1
fi
first_id=$(jq -r .id <<< "$first")
"$UMBRIEL" settle

"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle

# Animation time only moves by clock-advance from here: samples land 120 ms and 720 ms into the admission, and 120 ms
# into the close. Advancing 1600 ms finishes every timeline, including the reflow each change starts.
"$UMBRIEL" clock-freeze

# Mapping the second tile starts a long windows_move transition on the established first tile and its overview card.
FILL_COLOR=0xFF00FFFF "$UMBRIEL_UNMAP_CLIENT" overview-close-second 1200 700 > "$SECOND_LOG" 2>&1 &
for _ in $(seq 80); do
  if [[ $("$UMBRIEL" windows --json | jq length) -eq 2 ]]; then
    break
  fi
  sleep 0.025
done
if [[ $("$UMBRIEL" windows --json | jq length) -ne 2 ]]; then
  echo "second overview close client never mapped"
  exit 1
fi
"$UMBRIEL" clock-advance 120
grim "$MOVING"
moving_red=$(color_pixels "$MOVING" 'r > 0.8 && g < 0.1 && b < 0.1')
if ((moving_red < 4000)); then
  echo "setup did not put the first overview card under windows_move: red_pixels=$moving_red"
  exit 1
fi

# The opener fades in alongside the reflow its admission caused, exactly as it would outside the overview. Only the
# opener carries green and blue, so a partial value counts it whether it is over the red card or the black backdrop.
opening_dim=$(color_pixels "$MOVING" 'g > 0.08 && g < 0.7 && b > 0.08 && b < 0.7')
if ((opening_dim < 4000)); then
  echo "the overview opener did not fade in alongside the neighbour reflow: dim=$opening_dim"
  exit 1
fi

# windows_in ends 600 ms after the admission, while windows_move still runs until 1600 ms.
"$UMBRIEL" clock-advance 600
grim "$OPENED"
opened_cyan=$(color_pixels "$OPENED" 'g > 0.8 && b > 0.8 && r < 0.1')
if ((opened_cyan < 4000)); then
  echo "the overview opener never finished its windows_in: opaque=$opened_cyan"
  exit 1
fi
opened_red=$(color_pixels "$OPENED" 'r > 0.8 && g < 0.1 && b < 0.1')
if ((opened_red < 4000)); then
  echo "the overview opener's windows_in did not finish ahead of the neighbour reflow: red_pixels=$opened_red"
  exit 1
fi
"$UMBRIEL" clock-advance 1600
"$UMBRIEL" settle

"$UMBRIEL" msg "window-close:$first_id" > /dev/null
for _ in $(seq 80); do
  grep -q '^unmapped$' "$FIRST_LOG" && break
  sleep 0.01
done
if ! grep -q '^unmapped$' "$FIRST_LOG"; then
  echo "overview close client never unmapped: $(cat "$FIRST_LOG")"
  exit 1
fi
"$UMBRIEL" clock-advance 120
grim "$DURING"
during_green=$(color_pixels "$DURING" 'g > 0.8 && r < 0.1 && b < 0.1')
during_magenta=$(color_pixels "$DURING" 'r > 0.8 && g < 0.1 && b > 0.8')
if ((during_green < 4000)); then
  echo "overview card did not run windows_out from its original client buffer: green=$during_green magenta=$during_magenta"
  exit 1
fi
if ((during_magenta > 40)); then
  echo "overview close snapshot retained its stale windows_move shader: green=$during_green magenta=$during_magenta"
  exit 1
fi

"$UMBRIEL" clock-advance 1600
"$UMBRIEL" settle
grim "$AFTER"
after_green=$(color_pixels "$AFTER" 'g > 0.8 && r < 0.1 && b < 0.1')
after_magenta=$(color_pixels "$AFTER" 'r > 0.8 && g < 0.1 && b > 0.8')
if ((after_green > 40 || after_magenta > 40)); then
  echo "overview close snapshot remained after windows_out: green=$after_green magenta=$after_magenta"
  exit 1
fi

echo "an overview opener faded in alongside its reflow, and the close discarded windows_move and cleaned up"
