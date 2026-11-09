#!/usr/bin/env bash
# The create contract forbids get_information on a client-created description: asking is a protocol error for that
# client, and the compositor survives it.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

write_icc_matrix_profile "$PADDED" 0 2.2 > /dev/null

rc=$(color_run icc-denied-valid icc-denied "$PADDED")
if [[ $rc != 0 ]] || ! grep -Fq '{"event":"icc_info_denied"' "$RUNTIME/icc-denied-valid.log"; then
  echo "get_information on a client-created description was not denied: rc=$rc $(cat "$RUNTIME/icc-denied-valid.log")"
  exit 1
fi
if ! "$UMBRIEL" color --json | jq -e '.color_manager == true' > /dev/null; then
  echo "the compositor did not survive the denied information request: $("$UMBRIEL" color --json)"
  exit 1
fi

echo "get_information on a client-created ICC description is denied as the create contract requires"
