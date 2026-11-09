#!/usr/bin/env bash
# One client may hold only a few concurrent create_icc creators: the request past the cap is a protocol error for that
# client, and the compositor stays up.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

rc=$(color_run icc-cap-creators icc-cap-creators)
if [[ $rc != 2 ]] || ! grep -Fq '{"event":"icc_cap_creators"' "$RUNTIME/icc-cap-creators.log"; then
  echo "the creator budget did not refuse the past-cap creator: rc=$rc $(cat "$RUNTIME/icc-cap-creators.log")"
  exit 1
fi
if ! "$UMBRIEL" color --json | jq -e '.color_manager == true' > /dev/null; then
  echo "the compositor did not survive the creator-budget client: $("$UMBRIEL" color --json)"
  exit 1
fi

echo "a client past the concurrent-creator cap is refused and the compositor survives"
