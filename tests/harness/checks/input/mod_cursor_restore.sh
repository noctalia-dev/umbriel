#!/usr/bin/env bash
# Holding the mod key hands the cursor to the compositor for its move and resize
# affordance. Releasing it must give the client's own cursor back from what the
# compositor recorded, without a pointer leave and enter round trip.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly BEFORE="$UMBRIEL_RUNTIME_DIR/mod-cursor-before.png"
readonly DURING="$UMBRIEL_RUNTIME_DIR/mod-cursor-during.png"
readonly AFTER="$UMBRIEL_RUNTIME_DIR/mod-cursor-after.png"
readonly PREVIOUS="$UMBRIEL_RUNTIME_DIR/mod-cursor-previous.png"

if [[ ! -x $POINTER ]] || ! command -v foot > /dev/null; then
  echo "cursor helpers are not available"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1
curve = "linear"
EOF
"$UMBRIEL" msg config-reload > /dev/null

foot --config=/dev/null --title=mod-cursor sh -c 'sleep 120' > /dev/null 2>&1 &

window=''
for _ in $(seq 60); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "mod-cursor")')
  [[ -n $window ]] && break
  sleep 0.1
done
if [[ -z $window ]]; then
  echo "mod-cursor window never appeared"
  exit 1
fi
x=$(jq -r '(.x + .w / 2 | round)' <<< "$window")
y=$(jq -r '(.y + .h / 2 | round)' <<< "$window")
crop="48x48+$x+$y"
same_cursor() {
  local changed
  read -r _ changed < <("$UMBRIEL_PIXEL_PROBE" "$1" diff "$2" "$crop")
  ((changed == 0))
}

# Park the pointer inside the window so the client sets its own cursor.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move "$x" "$y"
# The client sets its cursor asynchronously after pointer enter; wait until two captures agree.
grim -c "$BEFORE"
for _ in $(seq 20); do
  mv "$BEFORE" "$PREVIOUS"
  grim -c "$BEFORE"
  same_cursor "$BEFORE" "$PREVIOUS" && break
done

pointer_hold "$OUTPUT_W" "$OUTPUT_H" mod logo -- mod none
grim -c "$DURING"
if same_cursor "$BEFORE" "$DURING"; then
  echo "positive control failed: holding mod did not change the cursor"
  exit 1
fi

pointer_release
"$UMBRIEL" settle
grim -c "$AFTER"
if ! same_cursor "$BEFORE" "$AFTER"; then
  echo "releasing mod did not restore the client cursor: difference $("$UMBRIEL_PIXEL_PROBE" "$BEFORE" diff "$AFTER" "$crop")"
  exit 1
fi

echo "the client cursor is replayed when the mod affordance ends"
