#!/usr/bin/env bash
# A client-side titlebar receives the initiating press before asking the
# compositor for an xdg-toplevel move. Finishing that move must return the
# matching release to the client and clear the seat's implicit pointer grab.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly LEFT_BUTTON=272
readonly KEY_A=30
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly OBSERVER="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly LOG="$UMBRIEL_RUNTIME_DIR/client-move-key-release.log"

if [[ ! -x $POINTER || ! -x $OBSERVER ]]; then
  echo "input helpers are not available"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$OBSERVER" client-move-key-release --request-move > "$LOG" 2>&1 &
window=''
for _ in $(seq 60); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "client-move-key-release")')
  [[ -n $window ]] && break
  sleep 0.1
done
if [[ -z $window ]]; then
  echo "client-move window never appeared"
  exit 1
fi
sleep 0.4
window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "client-move-key-release")')
x=$(jq -r '(.x + .w / 2 | round)' <<< "$window")
y=$(jq -r '(.y + .h / 2 | round)' <<< "$window")

# Press on the client decoration surrogate, move far enough to commit the
# compositor drag, type during it, then release. A second click proves that the
# first release cleared the seat instead of leaving its button count stuck.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" \
  move "$x" "$y" pause 300 press "$LEFT_BUTTON" \
  move "$((x + 80))" "$((y + 40))" tap "$KEY_A" \
  move "$((x + 120))" "$((y + 60))" release "$LEFT_BUTTON" \
  pause 100 click "$LEFT_BUTTON"

for _ in $(seq 30); do
  (( $(grep -c "pointer-button code=$LEFT_BUTTON state=released" "$LOG" || true) >= 2 )) && break
  sleep 0.1
done

presses=$(grep -c "pointer-button code=$LEFT_BUTTON state=pressed" "$LOG" || true)
releases=$(grep -c "pointer-button code=$LEFT_BUTTON state=released" "$LOG" || true)
if ((presses != 2 || releases != 2)); then
  echo "client move stranded pointer state: presses=$presses releases=$releases"
  echo "log: $(tr '\n' '|' < "$LOG")"
  exit 1
fi

echo "a client-initiated window move forwards its release after typing"
