#!/usr/bin/env bash
# Focus-only animation shaders must update and detach from overview cards after wheel navigation settles.
# No persistent effects are selected: their instance ledger must not gate native animation-slot refreshes.
set -euo pipefail
cat > "$UMBRIEL_RUNTIME_DIR/focus.glsl" <<'GLSL'
vec4 animation(vec2 uv) {
  if (umbriel_clamped_progress > 0.02 && umbriel_clamped_progress < 0.98) return vec4(1.0, 0.0, 0.0, 1.0);
  return umbriel_sample(uv);
}
GLSL
cat >> "$UMBRIEL_CONFIG" <<'TOML'
[animation]
enabled = true
curve = "linear"
[animation.windows_in]
enabled = false
[animation.windows_move]
enabled = false
[animation.overview]
duration_ms = 100
curve = "linear"
[animation.border]
enabled = true
effect = "focus-marker"
duration_ms = 1200
curve = "linear"
[animation.dim_unfocused]
enabled = true
effect = "focus-marker"
dim = 0.4
duration_ms = 1200
curve = "linear"
[effects]
in_capture = true
[effects.preset.focus-marker]
kind = "animation"
shader = "focus.glsl"
[appearance]
border_width = 8
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = false
[colors]
backdrop = "#000000FF"
[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"
[overview]
zoom = 0.5
scroll_factor_horizontal = 1.0
TOML
"$UMBRIEL" msg config-reload >/dev/null
for index in 1 2; do
  FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" "overview-focus-$index" 700 450 >"$UMBRIEL_RUNTIME_DIR/client-$index.log" 2>&1 &
  for _ in $(seq 80); do
    [[ $("$UMBRIEL" windows --json | jq length) == "$index" ]] && break
    sleep 0.025
  done
done
[[ $("$UMBRIEL" windows --json | jq length) == 2 ]]
"$UMBRIEL" msg column-focus-first >/dev/null
"$UMBRIEL" settle >/dev/null
"$UMBRIEL" msg overview-open >/dev/null
"$UMBRIEL" settle >/dev/null
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 640 360
"$UMBRIEL" clock-freeze >/dev/null
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod shift notch 1 mod none
[[ $("$UMBRIEL" windows --json | jq -r '.[] | select(.focused) | .title') == overview-focus-2 ]]
"$UMBRIEL" clock-advance 300 >/dev/null
grim "$UMBRIEL_RUNTIME_DIR/focus-during.png"
red=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/focus-during.png" count 'r > 0.9 && g < 0.1 && b < 0.1')
(( red > 100 )) || { echo "focus shader never appeared on overview cards: red=$red"; exit 1; }
"$UMBRIEL" clock-advance 2000 >/dev/null
grim "$UMBRIEL_RUNTIME_DIR/focus-settled.png"
red=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/focus-settled.png" count 'r > 0.9 && g < 0.1 && b < 0.1')
(( red == 0 )) || { echo "overview retained stale focus shader after completion: red=$red"; exit 1; }
echo 'overview focus-only animation slots advance and clear after wheel navigation'
