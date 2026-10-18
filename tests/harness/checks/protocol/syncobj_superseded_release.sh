#!/usr/bin/env bash
# A client that commits two explicitly synchronized buffers to a synchronized subsurface whose parent does not commit
# again gets the first buffer's release point signalled, since the second replaces it before it can be shown. Firefox
# renders a prompt panel this way with three swapchain buffers, then blocks on their release points before its main
# thread commits the parent, which freezes the browser.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SYNCOBJ_CLIENT:-./build-debug/tests/syncobj-client}"

log="$UMBRIEL_RUNTIME_DIR/syncobj-superseded.log"
if ! "$CLIENT" superseded > "$log" 2>&1; then
  echo "superseded: $(cat "$log")"
  exit 1
fi
if ! grep -q '^released$' "$log"; then
  echo "superseded: release point not reported: $(cat "$log")"
  exit 1
fi

echo "release point signalled for a cached subsurface buffer that a later cached commit replaced"
