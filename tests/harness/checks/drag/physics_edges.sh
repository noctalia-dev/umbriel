#!/usr/bin/env bash
# A border whose rigid position is outside the output must survive when drag physics pulls it back on screen,
# including nested border/window effects on a rotated, fractionally scaled output.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/drag-edge.png"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#000000FF"
[colors.border]
focused = "#FF0000FF"
[appearance]
border_width = 8
outer_border_width = 0
corner_radius = 0
drag_opacity = 1.0
[appearance.shadow]
enabled = false
[animation]
duration_ms = 1
[animation.windows_in]
enabled = false
[animation.windows_out]
enabled = false
[animation.windows_drag]
physics = true
[[window_rule]]
match.title = "^edge$"
default_floating = true
default_position = { x = 150, y = 150, anchor = "top_left" }
EOF
readonly BASE="$UMBRIEL_RUNTIME_DIR/edge-base.toml"
cp "$UMBRIEL_CONFIG" "$BASE"
cat > "$UMBRIEL_RUNTIME_DIR/ring.glsl" <<'GLSL'
vec4 border(vec2 uv) { return vec4(1.0, 0.0, 0.0, 1.0); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/window.glsl" <<'GLSL'
vec4 window(vec2 uv) { return umbriel_sample(uv); }
GLSL
for mode in normal rotated; do
  cp "$BASE" "$UMBRIEL_CONFIG"
  case "$mode" in
    normal) output_w=1280; output_h=720; scale=1; transform=normal ;;
    rotated) output_w=576; output_h=1024; scale=1.25; transform=90
      sed -i 's/x = 150/x = 80/' "$UMBRIEL_CONFIG"
      cat >> "$UMBRIEL_CONFIG" <<'EOF'
[effects]
border = "ring"
window = "window"
[effects.preset.ring]
kind = "border"
shader = "ring.glsl"
padding = 4
[effects.preset.window]
kind = "window"
shader = "window.glsl"
EOF
      ;;
  esac
  cat >> "$UMBRIEL_CONFIG" <<EOF
[output."HEADLESS-1"]
scale = $scale
transform = "$transform"
EOF
  "$UMBRIEL" msg config-reload > /dev/null

  for edge in left right top bottom; do
    FILL_COLOR=0xFF00FF00 "$UMBRIEL_UNMAP_CLIENT" edge 400 300 > "$UMBRIEL_RUNTIME_DIR/$edge.log" 2>&1 &
    for _ in $(seq 80); do
      window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "edge")')
      [[ -n $window ]] && break
      sleep 0.025
    done
    [[ -n $window ]]
    "$UMBRIEL" settle > /dev/null
    read -r x y w h id < <(jq -r '"\(.x) \(.y) \(.w) \(.h) \(.id)"' <<< "$window")
    gx=$((x + 40)); gy=$((y + 40)); dx=0; dy=0
    case "$edge" in
      left) gx=$((x + w - 40)); dx=$((-20 - x)); strip="100x100+0+$((y + h / 2))" ;;
      right) dx=$((output_w - w + 20 - x)); strip="100x100+$((output_w - 100))+$((y + h / 2))" ;;
      top) gy=$((y + h - 40)); dy=$((-20 - y)); strip="100x100+$((x + w / 2))+0" ;;
      bottom) dy=$((output_h - h + 20 - y)); strip="100x100+$((x + w / 2))+$((output_h - 100))" ;;
    esac
    "$UMBRIEL" clock-freeze
    pointer_hold "$output_w" "$output_h" move "$gx" "$gy" mod super press 272 \
      move $((gx + dx / 3)) $((gy + dy / 3)) \
      move $((gx + 2 * dx / 3)) $((gy + 2 * dy / 3)) \
      move $((gx + dx)) $((gy + dy)) -- release 272 mod none
    "$UMBRIEL" clock-advance 16 > /dev/null
    grim -s 1 "$IMAGE"
    red=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r > 0.8 && g < 0.2 && b < 0.2' "$strip")
    if ((red < 100)); then
      echo "$mode $edge border was clipped before deformation: $red red pixels"
      exit 1
    fi
    pointer_release
    "$UMBRIEL" msg "window-close:$id" > /dev/null
    "$UMBRIEL" clock-resume
    for _ in $(seq 80); do
      [[ $("$UMBRIEL" windows --json | jq length) == 0 ]] && break
      sleep 0.025
    done
  done
done
echo "drag physics kept all four borders at output edges, including fractional scale and rotation"
