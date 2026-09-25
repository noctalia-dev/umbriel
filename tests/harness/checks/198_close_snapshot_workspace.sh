#!/usr/bin/env bash
# Tiled and floating close snapshots retain their source workspace ownership. They travel with an outgoing workspace,
# stay clipped to its viewport, and become hidden when that workspace finishes leaving the output.
set -euo pipefail

readonly SHOTS="$UMBRIEL_RUNTIME_DIR/close-snapshot-workspace"
mkdir -p "$SHOTS"

cat > "$UMBRIEL_RUNTIME_DIR/workspace-close-stripe.glsl" <<'GLSL'
vec4 animation(vec2 uv) {
    if (uv.x >= 0.70 && uv.x <= 0.78 && uv.y >= 0.55) {
        return vec4(0.0, 1.0, 0.0, 1.0);
    }
    return vec4(0.0);
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
gap = 0

[output."HEADLESS-1"]
workspaces = 3
workspace_axis = "horizontal"

[animation.windows_in]
enabled = false

[animation.windows_out]
enabled = true
duration_ms = 4000
curve = "linear"
shader = "workspace-close-stripe.glsl"

[animation.windows_move]
enabled = false

[animation.workspaces]
enabled = true
duration_ms = 1200
curve = "linear"

[[window_rule]]
match.title = "^ownership-(tiled|floating)-anchor$"
default_floating = true
default_floating_size_px = { width = 120, height = 120 }
default_position = { x = 1050, y = 80, anchor = "top_left" }

[[window_rule]]
match.title = "^ownership-floating-close$"
default_floating = true
default_floating_size_px = { width = 600, height = 360 }
default_position = { x = 420, y = 300, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn() {
  local title=$1 color=$2 width=${3:-1280} height=${4:-720}
  FILL_COLOR="$color" "$UMBRIEL_UNMAP_CLIENT" "$title" "$width" "$height" \
    > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 100); do
    window=$("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)')
    [[ -n $window ]] && return 0
    sleep 0.025
  done
  echo "timed out waiting for $title"
  return 1
}

wait_unmapped() {
  local title=$1
  for _ in $(seq 100); do
    if grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/$title.log" \
        && ! "$UMBRIEL" windows --json | jq -e --arg title "$title" 'any(.[]; .title == $title)' > /dev/null; then
      return 0
    fi
    sleep 0.025
  done
  echo "timed out waiting for $title to unmap"
  return 1
}

red_pixels() {
  "$UMBRIEL_PIXEL_PROBE" "$1" count 'r > 0.7 && g < 0.2 && b < 0.2'
}

green_pixels() {
  "$UMBRIEL_PIXEL_PROBE" "$1" count 'r < 0.2 && g > 0.7 && b < 0.2'
}

red_bounds() {
  "$UMBRIEL_PIXEL_PROBE" "$1" bbox 'r > 0.7 && g < 0.2 && b < 0.2'
}

green_bounds() {
  "$UMBRIEL_PIXEL_PROBE" "$1" bbox 'r < 0.2 && g > 0.7 && b < 0.2'
}

absolute() {
  local value=$1
  if ((value < 0)); then
    echo $((-value))
  else
    echo "$value"
  fi
}

verify_workspace_slide() {
  local phase=$1 closing_title=$2 anchor_title=$3 destination=$4
  local closing anchor id image label
  closing=$("$UMBRIEL" windows --json | jq -c --arg title "$closing_title" '.[] | select(.title == $title)')
  anchor=$("$UMBRIEL" windows --json | jq -c --arg title "$anchor_title" '.[] | select(.title == $title)')
  if [[ -z $closing || -z $anchor ]]; then
    echo "$phase: source workspace did not contain both the closer and its live anchor"
    return 1
  fi

  id=$(jq -r .id <<< "$closing")
  "$UMBRIEL" msg "window-close:$id" > /dev/null
  wait_unmapped "$closing_title"
  "$UMBRIEL" clock-advance 50

  image="$SHOTS/$phase-baseline.png"
  grim "$image"
  if (( $(red_pixels "$image") < 500 || $(green_pixels "$image") < 500 )); then
    echo "$phase: close snapshot and source anchor were not both visible before the workspace switch"
    return 1
  fi

  local base_rx base_ry base_rw base_rh base_gx base_gy base_gw base_gh
  read -r base_rx base_ry base_rw base_rh < <(red_bounds "$image")
  read -r base_gx base_gy base_gw base_gh < <(green_bounds "$image")
  local base_delta=$((2 * base_gx + base_gw - 2 * base_rx - base_rw))
  local previous_rx=$base_rx previous_gx=$base_gx

  "$UMBRIEL" msg "workspace-switch:$destination" > /dev/null
  "$UMBRIEL" clock-advance 250
  grim "$SHOTS/$phase-early.png"
  "$UMBRIEL" clock-advance 350
  grim "$SHOTS/$phase-middle.png"

  for label in early middle; do
    image="$SHOTS/$phase-$label.png"
    if (( $(red_pixels "$image") < 500 || $(green_pixels "$image") < 500 )); then
      echo "$phase: close snapshot or source anchor vanished during the $label workspace-slide sample"
      return 1
    fi

    local rx ry rw rh gx gy gw gh current_delta delta_error red_y_error green_y_error
    read -r rx ry rw rh < <(red_bounds "$image")
    read -r gx gy gw gh < <(green_bounds "$image")
    current_delta=$((2 * gx + gw - 2 * rx - rw))
    delta_error=$(absolute $((current_delta - base_delta)))
    red_y_error=$(absolute $((ry - base_ry)))
    green_y_error=$(absolute $((gy - base_gy)))
    if ((delta_error > 8 || red_y_error > 3 || green_y_error > 3)); then
      echo "$phase: close snapshot did not travel rigidly with its workspace anchor at $label: delta_error=$delta_error red_y_error=$red_y_error green_y_error=$green_y_error"
      return 1
    fi
    if ((previous_rx - rx < 80 || previous_gx - gx < 80)); then
      echo "$phase: close snapshot and anchor did not both move left at $label: red=$previous_rx->$rx green=$previous_gx->$gx"
      return 1
    fi
    previous_rx=$rx
    previous_gx=$gx
  done

  # The slide has ended, but the four-second close timeline has not. Workspace ownership must hide both objects.
  "$UMBRIEL" clock-advance 750
  image="$SHOTS/$phase-settled.png"
  grim "$image"
  local final_red final_green
  final_red=$(red_pixels "$image")
  final_green=$(green_pixels "$image")
  if ((final_red >= 50 || final_green >= 50)); then
    echo "$phase: inactive source workspace leaked after the slide: red=$final_red green=$final_green"
    return 1
  fi
}

spawn ownership-tiled-close 0xFF0000FF
"$UMBRIEL" settle
spawn ownership-tiled-anchor 0xFFFF0000
"$UMBRIEL" settle
# Animation time only moves by clock-advance: slide samples land 250 ms and 600 ms into the 1200 ms workspace slide,
# and the settled sample at 1350 ms, still inside the 4000 ms close timeline.
"$UMBRIEL" clock-freeze
verify_workspace_slide tiled ownership-tiled-close ownership-tiled-anchor 2
"$UMBRIEL" clock-advance 4000

spawn ownership-floating-close 0xFF0000FF 600 360
"$UMBRIEL" settle
spawn ownership-floating-anchor 0xFFFF0000 120 120
"$UMBRIEL" settle
verify_workspace_slide floating ownership-floating-close ownership-floating-anchor 3

echo "tiled and floating close snapshots stayed owned by their source workspaces throughout horizontal slides"
