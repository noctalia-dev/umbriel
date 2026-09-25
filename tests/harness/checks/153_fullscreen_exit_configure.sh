#!/usr/bin/env bash
# Entering fullscreen must send the output size, and leaving it must restore the tiled size, in the first configure
# of the new state.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LAYER_CLIENT="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/fullscreen-exit-configure-client.log"
readonly PANEL_LOG="$UMBRIEL_RUNTIME_DIR/fullscreen-exit-configure-panel.log"
readonly TITLE=fullscreen-exit-configure

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[layout.scrolling]
default_extent_fraction = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$LAYER_CLIENT" HEADLESS-1 40 > "$PANEL_LOG" 2>&1 &
for _ in $(seq 80); do
  if grep -q '^ready$' "$PANEL_LOG"; then
    break
  fi
  sleep 0.05
done
if ! grep -q '^ready$' "$PANEL_LOG"; then
  echo "exclusive-zone panel did not map: $(< "$PANEL_LOG")"
  exit 1
fi

env LOG_CONFIGURES=1 "$CLIENT" "$TITLE" 640 480 > "$CLIENT_LOG" 2>&1 &

wait_for_window() {
  local expression=$1 message=$2 windows=
  for _ in $(seq 80); do
    windows=$("$UMBRIEL" windows --json)
    if jq -e "$expression" <<< "$windows" > /dev/null; then
      return 0
    fi
    sleep 0.05
  done
  echo "$message: $windows"
  return 1
}

wait_for_fullscreen() {
  local expected=$1 message=$2 state=
  for _ in $(seq 80); do
    state=$("$UMBRIEL" tearing --json)
    if jq -e ".surfaces[] | select(.title == \"$TITLE\" and .fullscreen == $expected)" \
      <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.05
  done
  echo "$message: $state"
  return 1
}

wait_for_configure_count() {
  local expected=$1 count
  for _ in $(seq 80); do
    count=$(grep -c '^configured-state=' "$CLIENT_LOG" || true)
    if (( count >= expected )); then
      return 0
    fi
    sleep 0.05
  done
  echo "client did not receive configure $expected: $(< "$CLIENT_LOG")"
  return 1
}

wait_for_windowed_configure_count() {
  local expected=$1 count
  for _ in $(seq 80); do
    count=$(grep -c '^configured-state=.* windowed$' "$CLIENT_LOG" || true)
    if (( count >= expected )); then
      return 0
    fi
    sleep 0.05
  done
  echo "client did not receive windowed configure $expected: $(< "$CLIENT_LOG")"
  return 1
}

wait_for_event_count() {
  local event=$1 expected=$2 count
  for _ in $(seq 80); do
    count=$(grep -Fxc "$event" "$CLIENT_LOG" || true)
    if (( count >= expected )); then
      return 0
    fi
    sleep 0.05
  done
  echo "client did not receive event $expected ('$event'): $(< "$CLIENT_LOG")"
  return 1
}

wait_for_window ".[] | select(.title == \"$TITLE\")" "client never mapped"
wait_for_configure_count 1
sleep 0.1

normal_event=$(grep '^configured-state=.* windowed$' "$CLIENT_LOG" | tail -1)
if [[ -z $normal_event ]]; then
  echo "client did not receive an initial windowed configure: $(< "$CLIENT_LOG")"
  exit 1
fi
normal_size=${normal_event#configured-state=}
normal_size=${normal_size% windowed}

"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
for _ in $(seq 80); do
  edge_event=$(grep '^configured-state=.* windowed$' "$CLIENT_LOG" | tail -1 || true)
  if [[ -n $edge_event && $edge_event != "$normal_event" ]]; then
    break
  fi
  sleep 0.05
done
if [[ -z $edge_event || $edge_event == "$normal_event" ]]; then
  echo "client did not receive its maximize-to-edges size: $(< "$CLIENT_LOG")"
  exit 1
fi

normal_count=$(grep -Fxc "$normal_event" "$CLIENT_LOG")
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
wait_for_event_count "$normal_event" "$((normal_count + 1))"

output_size=$("$UMBRIEL" outputs --json | jq -r '.[0].modes[] | select(.current) | "\(.width)x\(.height)"')
before_enter_count=$(grep -c '^configured-state=.* fullscreen$' "$CLIENT_LOG" || true)
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen true "client did not enter fullscreen"
for _ in $(seq 80); do
  fullscreen_event=$(grep '^configured-state=.* fullscreen$' "$CLIENT_LOG" | sed -n "$((before_enter_count + 1))p")
  [[ -n $fullscreen_event ]] && break
  sleep 0.05
done
if [[ $fullscreen_event != "configured-state=$output_size fullscreen" ]]; then
  echo "first fullscreen configure did not carry the output size: expected '$output_size', got '${fullscreen_event:-missing}'"
  echo "configure log: $(< "$CLIENT_LOG")"
  exit 1
fi
sleep 0.1

before_exit_count=$(grep -c '^configured-state=.* windowed$' "$CLIENT_LOG")
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen false "client did not leave fullscreen"
wait_for_windowed_configure_count "$((before_exit_count + 1))"

exit_event=$(grep '^configured-state=.* windowed$' "$CLIENT_LOG" | sed -n "$((before_exit_count + 1))p")
if [[ $exit_event != "$normal_event" ]]; then
  echo "first windowed configure did not restore the tile: expected '$normal_event', got '${exit_event:-missing}'"
  echo "configure log: $(< "$CLIENT_LOG")"
  exit 1
fi

fullscreen_count=$(grep -Fxc "$fullscreen_event" "$CLIENT_LOG")
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen true "client did not re-enter fullscreen"
wait_for_event_count "$fullscreen_event" "$((fullscreen_count + 1))"

before_exit_count=$(grep -c '^configured-state=.* windowed$' "$CLIENT_LOG")
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
wait_for_fullscreen false "maximize-to-edges did not leave fullscreen"
wait_for_windowed_configure_count "$((before_exit_count + 1))"
exit_event=$(grep '^configured-state=.* windowed$' "$CLIENT_LOG" | sed -n "$((before_exit_count + 1))p")
if [[ $exit_event != "$edge_event" ]]; then
  echo "fullscreen maximize transition exposed a tile configure: expected '$edge_event', got '${exit_event:-missing}'"
  echo "configure log: $(< "$CLIENT_LOG")"
  exit 1
fi

normal_count=$(grep -Fxc "$normal_event" "$CLIENT_LOG")
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
wait_for_event_count "$normal_event" "$((normal_count + 1))"

edge_count=$(grep -Fxc "$edge_event" "$CLIENT_LOG")
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
wait_for_event_count "$edge_event" "$((edge_count + 1))"

fullscreen_count=$(grep -Fxc "$fullscreen_event" "$CLIENT_LOG")
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen true "maximized client did not enter fullscreen"
wait_for_event_count "$fullscreen_event" "$((fullscreen_count + 1))"

before_exit_count=$(grep -c '^configured-state=.* windowed$' "$CLIENT_LOG")
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen false "maximized client did not leave fullscreen"
wait_for_windowed_configure_count "$((before_exit_count + 1))"
exit_event=$(grep '^configured-state=.* windowed$' "$CLIENT_LOG" | sed -n "$((before_exit_count + 1))p")
if [[ $exit_event != "$edge_event" ]]; then
  echo "maximized fullscreen exit used the wrong first configure"
  echo "expected '$edge_event', got '${exit_event:-missing}'"
  echo "configure log: $(< "$CLIENT_LOG")"
  exit 1
fi

echo "fullscreen exits restored tiled and maximized sizes in their first windowed configure"
