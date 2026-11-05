#!/usr/bin/env bash
# Unmanaged harness instance: verifies exit, not managed scope killing.
set -euo pipefail
"$UMBRIEL" msg session-quit skip-confirmation no-wait >/dev/null 2>&1 || true
for _ in $(seq 20); do
  if ! "$UMBRIEL" windows >/dev/null 2>&1; then
    exit 0
  fi
  sleep 0.1
done
echo "compositor survived session-quit skip-confirmation no-wait"
exit 1
