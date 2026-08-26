#!/usr/bin/env bash
# Cursor hiding must not detach the visible cursor surface while a client owns
# an implicit pointer grab. Restoring it on later pointer activity would send a
# leave and enter sequence that cancels held-button actions in games.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly LEFT_BUTTON=272
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"
readonly POINTER_LOG="$UMBRIEL_RUNTIME_DIR/hide-typing-pointer.log"
readonly BEFORE="$UMBRIEL_RUNTIME_DIR/hide-typing-before.png"
readonly AFTER="$UMBRIEL_RUNTIME_DIR/hide-typing-after.png"

if [[ ! -x $POINTER ]] || ! command -v foot > /dev/null; then
  echo "hide-when-typing helpers are not available"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
animation_ms = 1

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

# Keep the press down while a second helper sends a key. Separate connections
# make both cursor-inclusive captures deterministic without timing guesses.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" \
  move "$pointer_x" "$pointer_y" press "$LEFT_BUTTON" pause 5000 release "$LEFT_BUTTON" \
  > "$POINTER_LOG" 2>&1 &
pointer_pid=$!

sleep 0.5
grim -c "$BEFORE"
before_colors=$(magick "$BEFORE" -crop "$crop" +repage -format '%k' info:)
if ((before_colors < 2)); then
  echo "positive control did not capture a visible cursor: colors=$before_colors crop=$crop"
  exit 1
fi

"$POINTER" "$OUTPUT_W" "$OUTPUT_H" tap 30
grim -c "$AFTER"
before_hash=$(magick "$BEFORE" -crop "$crop" +repage rgba:- | sha256sum | cut -d' ' -f1)
after_hash=$(magick "$AFTER" -crop "$crop" +repage rgba:- | sha256sum | cut -d' ' -f1)
if [[ $before_hash != "$after_hash" ]]; then
  after_colors=$(magick "$AFTER" -crop "$crop" +repage -format '%k' info:)
  echo "typing hid the cursor during a held button: before_colors=$before_colors after_colors=$after_colors"
  exit 1
fi

wait "$pointer_pid" || {
  echo "pointer client failed: $(< "$POINTER_LOG")"
  exit 1
}

echo "typing preserves the cursor and pointer grab while a button is held"
