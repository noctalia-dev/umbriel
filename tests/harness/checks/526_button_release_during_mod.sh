#!/usr/bin/env bash
# A held button is an implicit grab owned by the surface that got the press.
# Keyboard activity during it, in particular the mod key driving the hover
# affordance cursor, must not move pointer focus: wlroots forgets its pressed
# buttons on every focus change and the release would then reach nobody.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly LEFT_BUTTON=272
readonly KEY_A=30
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly OBSERVER="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly LOG="$UMBRIEL_RUNTIME_DIR/button-release.log"

if [[ ! -x $POINTER || ! -x $OBSERVER ]]; then
  echo "input helpers are not available"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$OBSERVER" button-release > "$LOG" 2>&1 &
window=''
for _ in $(seq 60); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "button-release")')
  [[ -n $window ]] && break
  sleep 0.1
done
if [[ -z $window ]]; then
  echo "button-release window never appeared"
  exit 1
fi
# Geometry right after map is still the pre-arrangement one, so read it again
# once the layout has placed the window.
sleep 0.4
window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "button-release")')
x=$(jq -r '(.x + .w / 2 | round)' <<< "$window")
y=$(jq -r '(.y + .h / 2 | round)' <<< "$window")

# One connection, so the button really stays down while the keys are sent: a
# plain key, then the mod key that hands the cursor to the compositor and back.
# The pause gives the client time to bind wl_pointer, which it only can once
# the virtual pointer makes the seat advertise the capability.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" \
  move "$x" "$y" pause 300 press "$LEFT_BUTTON" \
  tap "$KEY_A" mod logo pause 200 mod none release "$LEFT_BUTTON" \
  > "$UMBRIEL_RUNTIME_DIR/button-release-pointer.log" 2>&1 || {
  echo "pointer client failed: $(< "$UMBRIEL_RUNTIME_DIR/button-release-pointer.log")"
  exit 1
}

for _ in $(seq 30); do
  grep -q "pointer-button code=$LEFT_BUTTON state=released" "$LOG" && break
  sleep 0.1
done

presses=$(grep -c "pointer-button code=$LEFT_BUTTON state=pressed" "$LOG" || true)
releases=$(grep -c "pointer-button code=$LEFT_BUTTON state=released" "$LOG" || true)
if ((presses != 1)); then
  echo "expected one press, got $presses"
  exit 1
fi
if ((releases != 1)); then
  echo "the button stayed down in the client: presses=$presses releases=$releases"
  echo "log: $(tr '\n' '|' < "$LOG")"
  exit 1
fi

# The release surviving is the contract, and focus never leaving is why: assert
# the transition itself so a future refocus during a grab cannot pass by
# happening to re-enter the same surface.
sequence=$(grep -E 'pointer-(enter|leave|button)' "$LOG" | tr '\n' '|')
if [[ $sequence == *"state=pressed|pointer-leave"* ]]; then
  echo "pointer focus left the grab owner while its button was held: $sequence"
  exit 1
fi

echo "a held button survives typing and a mod tap, and its release arrives"
