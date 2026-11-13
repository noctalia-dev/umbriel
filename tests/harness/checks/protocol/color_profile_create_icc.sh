#!/usr/bin/env bash
# A client may import an exactly representable profile: create_icc reaches ready, and destroying a surface tagged with
# the resulting description is safe in either order because the committed state copies the payload.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

# Client-created descriptions accept only curves the renderer paints exactly; gamma 2.2 is one of them.
write_icc_matrix_profile "$PADDED" 0 2.2 > /dev/null

rc=$(color_run icc-ready-valid icc-ready "$PADDED")
if [[ $rc != 0 ]] || ! grep -Fq '{"event":"icc_ready"' "$RUNTIME/icc-ready-valid.log"; then
  echo "create_icc did not reach ready for the valid profile: rc=$rc $(cat "$RUNTIME/icc-ready-valid.log")"
  exit 1
fi

# The synchronized surface state copies the description, so the description resource and the color-management surface
# must be destroyable in that order without freeing ICC bytes a cached commit still points at.
rc=$(color_run icc-surface-destroy-valid icc-surface-destroy "$PADDED")
if [[ $rc != 0 ]] || ! grep -Fq '{"event":"icc_surface_destroy"' "$RUNTIME/icc-surface-destroy-valid.log"; then
  echo "destroying a tagged surface after its description crashed or failed: rc=$rc" \
    "$(cat "$RUNTIME/icc-surface-destroy-valid.log")"
  exit 1
fi
if ! "$UMBRIEL" windows --json > /dev/null 2>&1; then
  echo "the compositor did not survive destroying a tagged surface"
  exit 1
fi

echo "a representable client ICC description reaches ready and survives destroying its surface"
