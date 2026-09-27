#!/usr/bin/env bash
# A spawned application inherits a clean signal state. The compositor handles SIGINT and SIGTERM on the event loop, which means libwayland blocks both process-wide via sigprocmask. A blocked signal mask survives fork and exec, so without an explicit reset in the child every application launched from a keybind would silently ignore Ctrl+C and systemd's stop signal.
set -euo pipefail

MARKER="umbriel-harness-spawn-$$"
CHILD=

"$UMBRIEL" msg "spawn:python3 -c 'import time; time.sleep(120)' $MARKER" > /dev/null

for _ in $(seq 40); do
  CHILD=$(pgrep -f "$MARKER" | head -1 || true)
  [[ -n $CHILD ]] && break
  sleep 0.25
done
if [[ -z $CHILD ]]; then
  echo "spawned process never appeared"
  exit 1
fi

# SigBlk is a hex bitmask; signal N occupies bit N-1.
blocked=$(awk '/^SigBlk:/ {print $2}' "/proc/$CHILD/status")
if [[ -z $blocked ]]; then
  echo "could not read SigBlk for pid $CHILD"
  exit 1
fi
sigint_bit=$(( 0x$blocked >> 1 & 1 ))   # SIGINT  = 2
sigterm_bit=$(( 0x$blocked >> 14 & 1 )) # SIGTERM = 15

if [[ $sigint_bit -ne 0 || $sigterm_bit -ne 0 ]]; then
  echo "spawned child inherited a blocked signal mask (SigBlk=$blocked)"
  echo "  SIGINT blocked: $sigint_bit, SIGTERM blocked: $sigterm_bit"
  exit 1
fi

# The mask is only half the story: prove the child actually dies on SIGTERM.
kill -TERM "$CHILD"
for _ in $(seq 20); do
  kill -0 "$CHILD" 2>/dev/null || break
  sleep 0.2
done
if kill -0 "$CHILD" 2>/dev/null; then
  echo "spawned child survived SIGTERM"
  exit 1
fi

echo "child signal mask clean, dies on SIGTERM"
