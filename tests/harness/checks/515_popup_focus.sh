#!/usr/bin/env bash
# Re-focusing an already focused toplevel must preserve its active XDG popup
# grab. An actual outside click must still dismiss the popup afterward.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly LEFT_BUTTON=272
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"
readonly POPUP_CLIENT="${UMBRIEL_POPUP_CLIENT:-./build-debug/popup-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/popup-client.log"
readonly POINTER_LOG="$UMBRIEL_RUNTIME_DIR/popup-pointer.log"

if [[ ! -x $POINTER || ! -x $POPUP_CLIENT ]]; then
  echo "popup focus clients are not built"
  exit 1
fi

wait_for_log() {
  local expected=$1
  for _ in $(seq 50); do
    grep -qx "$expected" "$CLIENT_LOG" 2>/dev/null && return 0
    sleep 0.1
  done
  echo "timed out waiting for '$expected': $(< "$CLIENT_LOG")"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
animation_ms = 1
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$POPUP_CLIENT" > "$CLIENT_LOG" 2>&1 &
wait_for_log ready

windows='[]'
for _ in $(seq 50); do
  windows=$("$UMBRIEL" windows --json)
  if jq -e 'any(.[]; .title == "popup-focus-regression")' <<< "$windows" > /dev/null; then
    break
  fi
  sleep 0.1
done
window_id=$(jq -r '.[] | select(.title == "popup-focus-regression") | .id' <<< "$windows")
window_x=$((OUTPUT_W / 2))
window_y=$((OUTPUT_H / 2))
if [[ -z $window_id || $window_id == null ]]; then
  echo "popup client window was not registered: $windows"
  exit 1
fi

printf 'window=%s x=%s y=%s state=%s\n' "$window_id" "$window_x" "$window_y" "$windows" > "$POINTER_LOG"

# Keep one pointer connected across popup creation and the redundant focus
# request. The final click lands in the output gap, outside every client.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" \
  move "$window_x" "$window_y" click "$LEFT_BUTTON" pause 1500 move 1 1 click "$LEFT_BUTTON" \
  >> "$POINTER_LOG" 2>&1 &
pointer_pid=$!
wait_for_log popup-mapped

"$UMBRIEL" msg "window-focus:$window_id" > /dev/null
sleep 0.2
if grep -qx 'popup-done' "$CLIENT_LOG" 2>/dev/null; then
  echo "redundant toplevel focus dismissed its popup: $(< "$CLIENT_LOG")"
  exit 1
fi

wait "$pointer_pid" || {
  echo "pointer client failed: $(< "$POINTER_LOG")"
  exit 1
}
wait_for_log popup-done

echo "same-view focus preserves the popup grab; an outside click dismisses it"
