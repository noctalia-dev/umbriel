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
cursor_hash() {
  grim -c "$1"
  magick "$1" -crop "$crop" +repage rgba:- | sha256sum | cut -d' ' -f1
}

# Park the pointer inside the window so the client sets its own cursor.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move "$x" "$y"
# The client sets its cursor asynchronously after pointer enter; wait until two captures agree.
before=$(cursor_hash "$BEFORE")
for _ in $(seq 20); do
  previous=$before
  before=$(cursor_hash "$BEFORE")
  [[ $before == "$previous" ]] && break
done

pointer_hold "$OUTPUT_W" "$OUTPUT_H" mod logo -- mod none
during=$(cursor_hash "$DURING")
if [[ $before == "$during" ]]; then
  echo "positive control failed: holding mod did not change the cursor"
  exit 1
fi

pointer_release
"$UMBRIEL" settle
after=$(cursor_hash "$AFTER")
if [[ $before != "$after" ]]; then
  echo "releasing mod did not restore the client cursor: before=$before during=$during after=$after"
  exit 1
fi

echo "the client cursor is replayed when the mod affordance ends"
