#!/usr/bin/env bash
# Mice report their thumb buttons as either BTN_SIDE/BTN_EXTRA or BTN_BACK/BTN_FORWARD. MouseBack and MouseForward
# binds fire for both pairs.
set -euo pipefail

readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly BTN_SIDE=275
readonly BTN_EXTRA=276
readonly BTN_FORWARD=277
readonly BTN_BACK=278
readonly MARKS="$UMBRIEL_RUNTIME_DIR/thumb-buttons.log"

cat >> "$UMBRIEL_CONFIG" <<EOF

[keybinds]
"Mod+MouseBack" = "spawn echo back >> $MARKS"
"Mod+MouseForward" = "spawn echo forward >> $MARKS"
EOF
"$UMBRIEL" msg config-reload > /dev/null

for button in "$BTN_SIDE" "$BTN_EXTRA" "$BTN_BACK" "$BTN_FORWARD"; do
  "$POINTER" 1280 720 move 640 360 mod logo press "$button" release "$button" mod none
done

expected=$'back\nback\nforward\nforward'
for _ in $(seq 40); do
  [[ -f $MARKS && $(sort "$MARKS") == "$expected" ]] && break
  sleep 0.05
done
actual=$(sort "$MARKS" 2> /dev/null || true)
if [[ $actual != "$expected" ]]; then
  echo "expected two back and two forward binds, got: ${actual//$'\n'/ }"
  exit 1
fi

echo "MouseBack and MouseForward binds fire for both thumb-button pairs"
