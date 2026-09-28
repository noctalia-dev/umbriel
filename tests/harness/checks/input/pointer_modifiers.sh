#!/usr/bin/env bash
# Core modifiers without keyboard focus, including implicit pointer grabs.
# harness: keyboard=none
set -euo pipefail
"${UMBRIEL_POINTER_MODIFIERS_CLIENT:-./build-debug/tests/pointer-modifiers-client}"
