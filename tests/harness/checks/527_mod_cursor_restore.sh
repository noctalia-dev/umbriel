#!/usr/bin/env bash
# Holding the mod key hands the cursor to the compositor for its move and resize
# affordance. Releasing it must give the client's own cursor back from what the
# compositor recorded, without a pointer leave and enter round trip.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly BEFORE="$UMBRIEL_RUNTIME_DIR/mod-cursor-before.png"
readonly DURING="$UMBRIEL_RUNTIME_DIR/mod-cursor-during.png"
readonly AFTER="$UMBRIEL_RUNTIME_DIR/mod-cursor-after.png"

if [[ ! -x $POINTER ]] || ! command -v foot > /dev/null; then
  echo "cursor helpers are not available"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1
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

# Park the pointer inside the window so the client sets its own cursor.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move "$x" "$y"
sleep 0.3
grim -c "$BEFORE"
before=$(magick "$BEFORE" -crop "$crop" +repage rgba:- | sha256sum | cut -d' ' -f1)

"$POINTER" "$OUTPUT_W" "$OUTPUT_H" mod logo pause 2000 mod none > /dev/null 2>&1 &
mod_pid=$!
sleep 0.5
grim -c "$DURING"
during=$(magick "$DURING" -crop "$crop" +repage rgba:- | sha256sum | cut -d' ' -f1)
if [[ $before == "$during" ]]; then
  echo "positive control failed: holding mod did not change the cursor"
  exit 1
fi

wait "$mod_pid"
sleep 0.3
grim -c "$AFTER"
after=$(magick "$AFTER" -crop "$crop" +repage rgba:- | sha256sum | cut -d' ' -f1)
if [[ $before != "$after" ]]; then
  echo "releasing mod did not restore the client cursor: before=$before during=$during after=$after"
  exit 1
fi

echo "the client cursor is replayed when the mod affordance ends"
