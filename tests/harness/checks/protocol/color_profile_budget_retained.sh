#!/usr/bin/env bash
# Created descriptions retain their bytes: a third description built from the same 12 MiB profile is refused once the
# two earlier ones fill the client's budget.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

write_icc_matrix_profile "$PADDED" 12 2.2 > /dev/null

rc=$(color_run icc-cap-retained icc-cap-retained "$PADDED")
if [[ $rc != 2 ]] \
    || ! grep -Fq '{"event":"icc_cap_retained","code":3,"iteration":2}' "$RUNTIME/icc-cap-retained.log"; then
  echo "the byte budget did not refuse the third retained description: rc=$rc $(cat "$RUNTIME/icc-cap-retained.log")"
  exit 1
fi

echo "bytes retained by created descriptions are charged, and the third retain is refused"
