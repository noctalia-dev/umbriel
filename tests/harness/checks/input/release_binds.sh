#!/usr/bin/env bash
# A mouse chord with press and release actions runs each once. A release action runs even when its press opened a
# confirmation dialog, but a release action never confirms that dialog: only a press does.
set -euo pipefail

readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly BTN_SIDE=275
readonly BTN_EXTRA=276
readonly BTN_MIDDLE=274
readonly MARKS="$UMBRIEL_RUNTIME_DIR/release-binds.log"
readonly TALK="$UMBRIEL_RUNTIME_DIR/push-to-talk.log"

cat >> "$UMBRIEL_CONFIG" <<EOF

[keybinds]
"Mod+MouseMiddle" = { action = "spawn echo down >> $TALK", release = "spawn echo up >> $TALK", repeat = false }
"Mod+MouseBack" = { action = "session-quit", release = "spawn echo released >> $MARKS", repeat = false }
"Mod+MouseForward" = { action = "session-quit", release = "session-quit", repeat = false }
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$POINTER" 1280 720 move 640 360 mod logo press "$BTN_MIDDLE" release "$BTN_MIDDLE" mod none
for _ in $(seq 40); do
  [[ -f $TALK && $(sort "$TALK") == $'down\nup' ]] && break
  sleep 0.05
done
talk=$(sort "$TALK" 2> /dev/null || true)
if [[ $talk != $'down\nup' ]]; then
  echo "expected one press and one release action, got: ${talk//$'\n'/ }"
  exit 1
fi

"$POINTER" 1280 720 move 640 360 mod logo press "$BTN_SIDE" release "$BTN_SIDE" mod none
for _ in $(seq 40); do
  [[ -f $MARKS ]] && break
  sleep 0.05
done
if [[ $(cat "$MARKS" 2> /dev/null || true) != released ]]; then
  echo "release action did not run while its press's confirmation was visible"
  exit 1
fi

# The press dismisses the open dialog and opens it again; the release must leave it open instead of quitting.
"$POINTER" 1280 720 move 640 360 mod logo press "$BTN_EXTRA" release "$BTN_EXTRA" mod none
"$UMBRIEL" settle
if ! "$UMBRIEL" windows --json > /dev/null; then
  echo "release session-quit confirmed the visible dialog"
  exit 1
fi

echo "release actions run under a visible confirmation but never confirm it"
