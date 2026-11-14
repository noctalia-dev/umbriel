#!/usr/bin/env bash
# harness: xwayland=true
# Fullscreen-only window effects update on both enter and exit, on native and X11 windows, without a reload.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly NATIVE="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly X_CLIENT="${UMBRIEL_XWAYLAND_FOCUS_CLIENT:-./build-debug/tests/xwayland-focus-client}"
readonly X_CONTROL="$UMBRIEL_RUNTIME_DIR/fullscreen-matcher-control"
cat > "$UMBRIEL_RUNTIME_DIR/fullscreen-matcher.glsl" <<'GLSL'
vec4 window(vec2 uv) { return umbriel_sample(uv); }
GLSL
cat >> "$UMBRIEL_CONFIG" <<'TOML'

[animation]
enabled = false
[input.focus]
follows_mouse = false
[effects.preset.fullscreen-test]
kind = "window"
shader = "fullscreen-matcher.glsl"
[[window_rule]]
match.title = "^fullscreen-matcher-"
match.is_fullscreen = true
window_effect = "fullscreen-test"
TOML
"$UMBRIEL" msg config-reload > /dev/null
"$NATIVE" fullscreen-matcher-native > "$UMBRIEL_RUNTIME_DIR/fullscreen-matcher-native.log" 2>&1 &
mkfifo "$X_CONTROL"
exec {control_fd}<>"$X_CONTROL"
"$X_CLIENT" fullscreen-matcher-x11 <&"$control_fd" > "$UMBRIEL_RUNTIME_DIR/fullscreen-matcher-x11.log" 2>&1 &

await_effect() {
  local title=$1 expected=$2
  for _ in $(seq 100); do
    if window_by_title "$title" | jq -e --arg effect "$expected" \
      '(.window_effect.name // "") == $effect' > /dev/null; then
      return 0
    fi
    sleep 0.025
  done
  echo "wrong fullscreen effect for $title: $("$UMBRIEL" windows --json)"
  return 1
}

for title in fullscreen-matcher-native fullscreen-matcher-x11; do
  await_window "$title"
  id=$(window_by_title "$title" | jq -r '.id')
  "$UMBRIEL" msg window-focus "$id" > /dev/null
  await_effect "$title" ""
  for _ in 1 2; do
    "$UMBRIEL" msg window-toggle-fullscreen > /dev/null
    "$UMBRIEL" settle > /dev/null
    await_effect "$title" fullscreen-test
    "$UMBRIEL" msg window-toggle-fullscreen > /dev/null
    "$UMBRIEL" settle > /dev/null
    await_effect "$title" ""
  done
done
echo "fullscreen-only effects update on native and X11 fullscreen transitions"
