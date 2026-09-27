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
