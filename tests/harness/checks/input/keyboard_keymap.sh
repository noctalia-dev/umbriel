#!/usr/bin/env bash
# Clients receive usable keymaps across virtual keyboard initialization, updates, removal, and reconnection.
# It asserts how the first virtual keyboard reaches clients, so the harness keyboard stays out.
# harness: keyboard=none
set -euo pipefail

readonly CLIENT="${UMBRIEL_KEYBOARD_KEYMAP_CLIENT:-./build-debug/tests/keyboard-keymap-client}"

"$CLIENT"
echo "virtual keyboard lifecycle preserves usable seat keymaps"
