#!/usr/bin/env bash
# Bytes a committed surface retains stay charged: the compositor copies the description into the surface state, so the
# budget may not release the payload when the description resource dies.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

write_icc_matrix_profile "$PADDED" 12 2.2 > /dev/null

rc=$(color_run icc-surface-retained icc-surface-retained "$PADDED")
if [[ $rc != 2 ]] \
    || ! grep -Fq '{"event":"icc_surface_retain","code":3,"iteration":2}' "$RUNTIME/icc-surface-retained.log"; then
  echo "the byte budget released a profile a committed surface still retains:" \
    "rc=$rc $(cat "$RUNTIME/icc-surface-retained.log")"
  exit 1
fi

echo "a committed surface keeps its ICC bytes charged after the description resource is gone"
