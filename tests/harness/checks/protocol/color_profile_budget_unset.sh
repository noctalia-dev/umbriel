#!/usr/bin/env bash
# Unsetting every description releases the bytes it retained, so a client that cycles set/commit/unset can stage another
# profile instead of running out of budget with nothing in use.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

write_icc_matrix_profile "$PADDED" 12 2.2 > /dev/null

rc=$(color_run icc-surface-unset icc-surface-unset "$PADDED")
if [[ $rc != 0 ]] || ! grep -Fq '{"event":"icc_unset_released"' "$RUNTIME/icc-surface-unset.log"; then
  echo "unsetting descriptions kept their ICC bytes charged: rc=$rc $(cat "$RUNTIME/icc-surface-unset.log")"
  exit 1
fi

echo "unset_image_description releases the bytes it retained instead of holding them until overwritten"
