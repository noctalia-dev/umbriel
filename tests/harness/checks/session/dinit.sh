#!/usr/bin/env bash
# Exercise readiness, environment publication and session lifetime with a private manager and bus.
set -euo pipefail
if command -v dinit >/dev/null 2>&1 && command -v dinitctl >/dev/null 2>&1; then
  python3 "$UMBRIEL_REPO/tests/dinit_session.py" "$UMBRIEL"
else
  echo 'dinit integration requires dinit and dinitctl; pipe readiness is covered by the unit suite'
fi
