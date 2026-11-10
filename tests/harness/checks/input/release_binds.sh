#!/usr/bin/env bash
# A release action runs even when its press opened a confirmation dialog, but a release action never confirms that
# dialog: only a press does.
set -euo pipefail

readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly BTN_SIDE=275
readonly BTN_EXTRA=276
readonly MARKS="$UMBRIEL_RUNTIME_DIR/release-binds.log"

cat >> "$UMBRIEL_CONFIG" <<EOF

[keybinds]
"Mod+MouseBack" = { action = "session-quit", release = "spawn echo released >> $MARKS", repeat = false }
"Mod+MouseForward" = { action = "session-quit", release = "session-quit", repeat = false }
EOF
"$UMBRIEL" msg config-reload > /dev/null

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
