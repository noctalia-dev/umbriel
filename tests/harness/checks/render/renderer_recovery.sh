#!/usr/bin/env bash
# Renderer loss is reported from inside the renderer's mutable lost signal. The test command emits it twice in one
# dispatch: recovery must let both emissions finish, coalesce them, then replace the renderer and draw another frame.
set -euo pipefail

readonly LOG_MARK=$(($(wc -l < "$UMBRIEL_LOG") + 1))

if [[ $(grep -c "Compiling animation shader: animation.builtin_fade" "$UMBRIEL_LOG") -ne 1 ]]; then
  echo "the built-in fade did not compile exactly once at startup"
  exit 1
fi

"$UMBRIEL" renderer-recover > /dev/null

recovered=false
for _ in $(seq 100); do
  if tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | grep -q "renderer recreated"; then
    recovered=true
    break
  fi
  sleep 0.02
done

if [[ $recovered != true ]]; then
  echo "renderer recovery did not complete"
  tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | tail -n 20
  exit 1
fi

if [[ $(grep -c "Compiling animation shader: animation.builtin_fade" "$UMBRIEL_LOG") -ne 2 ]]; then
  echo "the built-in fade did not recompile exactly once after the first recovery"
  exit 1
fi

"$UMBRIEL" settle > /dev/null
"$UMBRIEL" windows > /dev/null

# A preset bound before the loss must render through the new renderer's program,
# and a window without any selector must still open through the built-in fade.
cat > "$UMBRIEL_RUNTIME_DIR/rebind.glsl" <<'GLSL'
vec4 animation(vec2 uv) { return vec4(0.0, 1.0, 0.0, 1.0); }
GLSL
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 2000
curve = "linear"
[animation.windows_in]
style = "fade"
[effects.preset.rebind]
kind = "animation"
shader = "rebind.glsl"
[animation.windows_move]
effect = "rebind"
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" renderer-recover > /dev/null
recovered_again=false
for _ in $(seq 100); do
  if [[ $(tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | grep -c "renderer recreated") -ge 2 ]]; then
    recovered_again=true
    break
  fi
  sleep 0.02
done

if [[ $recovered_again != true ]]; then
  echo "second renderer recovery did not complete"
  tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | tail -n 20
  exit 1
fi

"$UMBRIEL" settle > /dev/null
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/recovery.png"
"$UMBRIEL" clock-freeze
"$UMBRIEL_UNMAP_CLIENT" recovery-fade 600 400 > "$UMBRIEL_RUNTIME_DIR/recovery-fade.log" 2>&1 &
for _ in $(seq 80); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "recovery-fade")')
  [[ -n $window ]] && break
  sleep 0.025
done
[[ -n $window ]]
x=$(jq -r '.x + (.w / 2 | floor)' <<< "$window")
y=$(jq -r '.y + (.h / 2 | floor)' <<< "$window")
# Halfway through the built-in fade the client's blue shows at roughly half strength over the black backdrop.
"$UMBRIEL" clock-advance 1000
grim "$IMAGE"
read -r _ _ blue < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$x" "$y")
if (( blue < 40 || blue > 140 )); then
  echo "the built-in opening fade did not run after recovery: blue=$blue"
  exit 1
fi
"$UMBRIEL" clock-advance 3000
# Moving the window binds the rebind preset: the moved window renders solid green through the recompiled program.
id=$(jq -r .id <<< "$window")
"$UMBRIEL" msg "window-focus:$id" > /dev/null
"$UMBRIEL" msg window-toggle-floating > /dev/null
"$UMBRIEL" clock-advance 500
grim "$IMAGE"
green=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'g > 0.9 && r < 0.1 && b < 0.1')
if (( green < 1000 )); then
  echo "the animation preset did not rebind after renderer recovery: $green green pixels"
  exit 1
fi
"$UMBRIEL" clock-advance 3000
"$UMBRIEL" settle > /dev/null

if [[ $(tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | grep -c "GPU context lost, recreating renderer") -ne 2 ]]; then
  echo "renderer loss did not start exactly two recoveries"
  tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | tail -n 20
  exit 1
fi

echo "renderer loss unwound, recreated the renderer, drew another frame, and rebound effects"

# recovery-runtime: suppressed IPC-only preset/pool roots (including the
# border overlay) survive renderer replacement without changing a cached pick.
cat > "$UMBRIEL_RUNTIME_DIR/recovery-border.glsl" <<'GLSL'
vec4 border(vec2 uv) { return vec4(0.0, 1.0, 0.0, 1.0); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/recovery-overlay.glsl" <<'GLSL'
vec4 window(vec2 uv) { return vec4(1.0, 0.0, 0.0, 1.0); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/recovery-screen.glsl" <<'GLSL'
vec4 screen(vec2 uv) { return vec4(0.0, 0.0, 1.0, 1.0); }
GLSL
cat >> "$UMBRIEL_CONFIG" <<'EOF_CONFIG'
[effects]
in_capture = true
[effects.preset.recovery_border_z]
kind = "border"
shader = "recovery-border.glsl"
overlay = "recovery_overlay"
[effects.preset.recovery_border_a]
kind = "border"
shader = "recovery-border.glsl"
[effects.preset.recovery_overlay]
kind = "window"
shader = "recovery-overlay.glsl"
[effects.preset.recovery_screen]
kind = "screen"
shader = "recovery-screen.glsl"
[effects.pool.recovery_pool]
kind = "border"
choose = ["recovery_border_z", "recovery_border_a"]
selection = "round_robin"
EOF_CONFIG
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg "effect-border-set:recovery_pool/$id" > /dev/null
"$UMBRIEL" msg "effect-border-toggle:$id" > /dev/null
"$UMBRIEL" msg effect-screen-set:recovery_screen > /dev/null
"$UMBRIEL" msg effect-screen-toggle > /dev/null
recovery_slot=$("$UMBRIEL" windows --json | jq -c --arg id "$id" '.[] | select(.id == $id) | .border_effect')
"$UMBRIEL" renderer-recover > /dev/null
for _ in $(seq 100); do
  [[ $(tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | grep -c "renderer recreated") -ge 3 ]] && break
  sleep 0.02
done
"$UMBRIEL" settle > /dev/null
if [[ $(tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | grep -c "renderer recreated") -ne 3 ]]; then
  echo "recovery-runtime: third recovery did not finish"
  exit 1
fi
recovered_slot=$("$UMBRIEL" windows --json | jq -c --arg id "$id" '.[] | select(.id == $id) | .border_effect')
if [[ $recovery_slot != "$recovered_slot" ]] || ! jq -e '.name == "recovery_border_z" and .pool == "recovery_pool" and .source == "runtime" and .suppressed' <<< "$recovered_slot" > /dev/null; then
  echo "recovery-runtime-slot: recovery changed a suppressed selection: $recovery_slot -> $recovered_slot"
  exit 1
fi
"$UMBRIEL" effects --json | jq -e '[.presets[] | select(.name | startswith("recovery_")) | .state] == ["compiled", "compiled", "compiled", "compiled"]' > /dev/null || { echo "recovery-runtime-prepared"; exit 1; }
"$UMBRIEL" msg "effect-border-toggle:$id" > /dev/null
"$UMBRIEL" settle > /dev/null
recovery_window=$("$UMBRIEL" windows --json | jq -c --arg id "$id" '.[] | select(.id == $id)')
x=$(jq -r '.x + (.w / 2 | floor)' <<< "$recovery_window")
y=$(jq -r '.y + (.h / 2 | floor)' <<< "$recovery_window")
grim "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$x" "$y")
if (( r < 240 || g > 15 || b > 15 )); then
  echo "recovery-overlay-pixel: unsuppressed overlay did not render after recovery: $r $g $b"
  exit 1
fi
"$UMBRIEL" msg effect-screen-toggle > /dev/null
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 20 20)
if (( b < 240 || r > 15 || g > 15 )); then
  echo "recovery-screen-pixel: unsuppressed IPC preset did not render after recovery: $r $g $b"
  exit 1
fi
"$UMBRIEL" windows --json | jq -e --arg id "$id" '.[] | select(.id == $id) | .border_effect | .name == "recovery_border_z" and (.suppressed | not)' > /dev/null || { echo "recovery-runtime-no-pick"; exit 1; }

# Active bindings must also recover while animation time is frozen. No action
# after recovery may be needed to repair either the border overlay or window slot.
"$UMBRIEL" msg effect-screen-reset > /dev/null
"$UMBRIEL" renderer-recover > /dev/null
for _ in $(seq 100); do
  [[ $(tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | grep -c "renderer recreated") -ge 4 ]] && break
  sleep 0.02
done
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$x" "$y")
if (( r < 240 || g > 15 || b > 15 )); then
  echo "recovery-active-border: active cached overlay did not rebind with frozen time: $r $g $b"
  exit 1
fi
"$UMBRIEL" msg "effect-border-reset:$id" > /dev/null
"$UMBRIEL" msg "effect-window-set:recovery_overlay/$id" > /dev/null
"$UMBRIEL" renderer-recover > /dev/null
for _ in $(seq 100); do
  [[ $(tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | grep -c "renderer recreated") -ge 5 ]] && break
  sleep 0.02
done
"$UMBRIEL" settle > /dev/null
grim "$IMAGE"
read -r r g b < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$x" "$y")
if (( r < 240 || g > 15 || b > 15 )); then
  echo "recovery-active-window: active cached window effect did not rebind with frozen time: $r $g $b"
  exit 1
fi
