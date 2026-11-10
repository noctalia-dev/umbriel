#!/usr/bin/env bash
# harness: xwayland=true
# A fullscreen X11 window in a scrolling strip is configured at its column, not at the output origin, once another
# column scrolls it aside. Override-redirect menus and absolute pointer events are mapped from that X position.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_XWAYLAND_FOCUS_CLIENT:-./build-debug/tests/xwayland-focus-client}"
readonly NEIGHBOUR="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/xwayland-fullscreen-strip-client.log"
readonly NEIGHBOUR_LOG="$UMBRIEL_RUNTIME_DIR/xwayland-fullscreen-strip-neighbour.log"
readonly CONTROL="$UMBRIEL_RUNTIME_DIR/xwayland-fullscreen-strip-control"
readonly TITLE=xwayland-fullscreen-strip
readonly NEIGHBOUR_TITLE=xwayland-fullscreen-strip-neighbour

if [[ ! -x $CLIENT || -z ${DISPLAY:-} ]]; then
  echo "the Xwayland helper or private DISPLAY is not available"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$CONTROL"
exec {control_fd}<>"$CONTROL"
"$CLIENT" "$TITLE" <&"$control_fd" > "$CLIENT_LOG" 2>&1 &

window_field() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" ".[] | select(.title == \$title) | $2"
}

for _ in $(seq 60); do
  [[ -n $(window_field "$TITLE" .id) ]] && break
  sleep 0.1
done
if [[ -z $(window_field "$TITLE" .id) ]]; then
  echo "the X11 fixture did not map: $($UMBRIEL windows --json)"
  exit 1
fi

printf 'fullscreen true\n' >&"$control_fd"
for _ in $(seq 40); do
  [[ $(window_field "$TITLE" .fullscreen) == true ]] && break
  sleep 0.1
done
if [[ $(window_field "$TITLE" .fullscreen) != true ]]; then
  echo "the X11 window did not enter fullscreen: $($UMBRIEL windows --json)"
  exit 1
fi

# A second column takes focus and scrolls the fullscreen column aside.
"$NEIGHBOUR" "$NEIGHBOUR_TITLE" 640 480 > "$NEIGHBOUR_LOG" 2>&1 &
for _ in $(seq 60); do
  [[ $(window_field "$NEIGHBOUR_TITLE" .focused) == true ]] && break
  sleep 0.1
done
"$UMBRIEL" settle > /dev/null
# The column decides x; a fullscreen window spans the output vertically.
x=$(window_field "$TITLE" .x)
read -r y w h < <("$UMBRIEL" outputs --json | jq -r '.[0] | "\(.position.y // 0) \(.logical_size.width) \(.logical_size.height)"')
if ((x == 0)); then
  echo "the neighbour column did not scroll the fullscreen X11 window aside: $($UMBRIEL windows --json)"
  exit 1
fi

expected="$x $y $w $h"
geometry=
for _ in $(seq 40); do
  printf 'geometry\n' >&"$control_fd"
  sleep 0.1
  geometry=$(sed -n 's/^geometry=//p' "$CLIENT_LOG" | tail -n 1)
  [[ $geometry == "$expected" ]] && break
done
if [[ $geometry != "$expected" ]]; then
  echo "the fullscreen X11 window in the strip was configured at '$geometry', expected its column at '$expected'"
  exit 1
fi

echo "a fullscreen X11 window scrolled aside in the strip is configured at its column"
