#!/usr/bin/env bash
# A virtual keyboard may exist before its owner uploads a keymap. Regular wl_keyboard clients must see only usable XKB
# keymaps while that device becomes ready and while its keymap changes later.
set -euo pipefail

readonly CLIENT="${UMBRIEL_KEYBOARD_KEYMAP_CLIENT:-./build-debug/tests/keyboard-keymap-client}"

"$CLIENT"
echo "deferred virtual keyboards expose only valid seat keymaps"
