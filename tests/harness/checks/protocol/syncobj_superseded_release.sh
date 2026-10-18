#!/usr/bin/env bash
# A client that commits explicitly synchronized buffers to a synchronized subsurface whose parent does not commit again
# gets a cached buffer's release point signalled once a later cached commit replaces it, since wlroots then applies both
# in one pass. Firefox renders a prompt panel this way with three swapchain buffers, then blocks on their release
# points before its main thread commits the parent, which freezes the browser. The buffer on screen, an earlier pending
# point on the same timeline, the final cached buffer, and a buffer whose replacement waits for its acquire fence are
# never released early.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SYNCOBJ_CLIENT:-./build-debug/tests/syncobj-client}"

for mode in superseded superseded-displayed superseded-shared-timeline superseded-locked; do
  log="$UMBRIEL_RUNTIME_DIR/syncobj-$mode.log"
  if ! "$CLIENT" "$mode" > "$log" 2>&1; then
    echo "$mode: $(cat "$log")"
    exit 1
  fi
  if ! grep -q '^ok$' "$log"; then
    echo "$mode: expectations not reported: $(cat "$log")"
    exit 1
  fi
done

echo "replaced cached subsurface buffers released early; displayed, timeline-blocked, final, and lock-blocked ones held"
