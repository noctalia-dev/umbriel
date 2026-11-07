#!/usr/bin/env bash
# harness: xwayland=true
# A floating X11 window that enters fullscreen is configured at its output's origin at once, while the move animation
# toward it is still running. Wine compares the configured position with the monitor and leaves fullscreen when the
# window keeps its floating position with the output size.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_XWAYLAND_FOCUS_CLIENT:-./build-debug/tests/xwayland-focus-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/xwayland-fullscreen-move-client.log"
readonly CONTROL="$UMBRIEL_RUNTIME_DIR/xwayland-fullscreen-move-control"
readonly TITLE=xwayland-fullscreen-move

if [[ ! -x $CLIENT || -z ${DISPLAY:-} ]]; then
  echo "the Xwayland helper or private DISPLAY is not available"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = true

[animation.windows_move]
enabled = true
duration_ms = 2000
curve = "linear"

[[window_rule]]
match.title = "^xwayland-fullscreen-move$"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 300, y = 200, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$CONTROL"
exec {control_fd}<>"$CONTROL"
"$CLIENT" "$TITLE" <&"$control_fd" > "$CLIENT_LOG" 2>&1 &

window_state() {
  "$UMBRIEL" tearing --json | jq -r --arg title "$TITLE" '.surfaces[] | select(.title == $title) | .fullscreen'
}

for _ in $(seq 60); do
  [[ -n $(window_state) ]] && break
  sleep 0.1
done
if [[ -z $(window_state) ]]; then
  echo "the X11 fixture did not map: $($UMBRIEL windows --json)"
  exit 1
fi
"$UMBRIEL" settle > /dev/null

read -r output_x output_y output_w output_h < <(
  "$UMBRIEL" outputs --json | jq -r '.[0] | "\(.position.x // 0) \(.position.y // 0) \(.logical_size.width) \(.logical_size.height)"'
)

# Hold the move animation at its first frame, so only a configure sent ahead of it can reach the origin.
"$UMBRIEL" clock-freeze > /dev/null
printf 'fullscreen true\n' >&"$control_fd"
for _ in $(seq 40); do
  [[ $(window_state) == true ]] && break
  sleep 0.1
done
if [[ $(window_state) != true ]]; then
  echo "the X11 window did not enter fullscreen: $($UMBRIEL windows --json)"
  exit 1
fi

expected="$output_x $output_y $output_w $output_h"
geometry=
for _ in $(seq 40); do
  printf 'geometry\n' >&"$control_fd"
  sleep 0.1
  geometry=$(sed -n 's/^geometry=//p' "$CLIENT_LOG" | tail -n 1)
  [[ $geometry == "$expected" ]] && break
done
if [[ $geometry != "$expected" ]]; then
  echo "the fullscreen X11 window was configured at '$geometry' during the move animation, expected '$expected'"
  exit 1
fi

echo "a floating X11 window entering fullscreen is configured at the output origin while its move animation runs"
