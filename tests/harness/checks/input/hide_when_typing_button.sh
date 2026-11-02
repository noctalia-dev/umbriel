#!/usr/bin/env bash
# Cursor hiding must not detach the visible cursor surface while a client owns
# an implicit pointer grab. Restoring it on later pointer activity would send a
# leave and enter sequence that cancels held-button actions in games.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly LEFT_BUTTON=272
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly BEFORE="$UMBRIEL_RUNTIME_DIR/hide-typing-before.png"
readonly AFTER="$UMBRIEL_RUNTIME_DIR/hide-typing-after.png"

if [[ ! -x $POINTER ]] || ! command -v foot > /dev/null; then
  echo "hide-when-typing helpers are not available"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1
curve = "linear"

[input.cursor]
hide_when_typing = true
hide_timeout_ms = 0
EOF
"$UMBRIEL" msg config-reload > /dev/null

foot --config=/dev/null --title=hide-typing-button sh -c 'sleep 120' > /dev/null 2>&1 &

window=''
for _ in $(seq 60); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "hide-typing-button")')
  [[ -n $window ]] && break
  sleep 0.1
done
if [[ -z $window ]]; then
  echo "hide-typing-button window never appeared"
  exit 1
fi

pointer_x=$(jq -r '(.x + .w / 2 | round)' <<< "$window")
pointer_y=$(jq -r '(.y + .h / 2 | round)' <<< "$window")
crop="48x48+$pointer_x+$pointer_y"

# Pixels in the crop that differ from its top-left pixel: zero when the crop is one flat colour, so no cursor is drawn.
off_corner_pixels() {
  local predicate
  predicate=$("$UMBRIEL_PIXEL_PROBE" "$1" pixel "$pointer_x" "$pointer_y" | awk '{
    printf "r < %.4f || r > %.4f || g < %.4f || g > %.4f || b < %.4f || b > %.4f",
      ($1 - 0.5) / 255, ($1 + 0.5) / 255, ($2 - 0.5) / 255, ($2 + 0.5) / 255, ($3 - 0.5) / 255, ($3 + 0.5) / 255
  }')
  "$UMBRIEL_PIXEL_PROBE" "$1" count "$predicate" "$crop"
}

# Keep the press down while a second helper sends a key. Separate connections
# make both cursor-inclusive captures deterministic without timing guesses.
pointer_hold "$OUTPUT_W" "$OUTPUT_H" move "$pointer_x" "$pointer_y" press "$LEFT_BUTTON" -- release "$LEFT_BUTTON"
grim -c "$BEFORE"
before_pixels=$(off_corner_pixels "$BEFORE")
if ((before_pixels == 0)); then
  echo "positive control did not capture a visible cursor: crop=$crop"
  exit 1
fi

"$POINTER" "$OUTPUT_W" "$OUTPUT_H" tap 30
grim -c "$AFTER"
read -r _ changed < <("$UMBRIEL_PIXEL_PROBE" "$BEFORE" diff "$AFTER" "$crop")
if ((changed != 0)); then
  echo "typing hid the cursor during a held button: $changed pixels changed in $crop"
  exit 1
fi

pointer_release || {
  echo "pointer client failed: $(< "$POINTER_HOLD_LOG")"
  exit 1
}

echo "typing preserves the cursor and pointer grab while a button is held"
