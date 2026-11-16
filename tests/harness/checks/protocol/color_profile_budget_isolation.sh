#!/usr/bin/env bash
# The budget is per client: a client that filled its own budget must not debit the next connection, whose wl_client
# address the compositor may even reuse.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

write_icc_matrix_profile "$PADDED" 12 2.2 > /dev/null

if [[ $(color_run icc-cap-retained icc-cap-retained "$PADDED") != 2 ]]; then
  echo "the budget fixture did not exhaust this client: $(cat "$RUNTIME/icc-cap-retained.log")"
  exit 1
fi
rc=$(color_run icc-post-budget icc-ready "$PADDED")
if [[ $rc != 0 ]] || ! grep -Fq '{"event":"icc_ready"' "$RUNTIME/icc-post-budget.log"; then
  echo "a later client inherited the previous client's retained ICC charges:" \
    "rc=$rc $(cat "$RUNTIME/icc-post-budget.log")"
  exit 1
fi

echo "a new client starts with an empty ICC budget even after its predecessor's address is reused"
