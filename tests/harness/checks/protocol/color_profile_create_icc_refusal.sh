#!/usr/bin/env bash
# Profiles the renderer cannot paint exactly are refused with 'unsupported' rather than substituted by the nearest named
# curve: a gamma 1.8 TRC, a truncated file and an ICC v3 header all fail the description gracefully.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

color_fixture
readonly TRUNCATED="$PROFILE.truncated"
readonly WRONG_VERSION="$PROFILE.wrongversion"
head -c 200 "$PROFILE" > "$TRUNCATED"
python3 - "$PROFILE" "$WRONG_VERSION" <<'PY'
import sys

mutated = bytearray(open(sys.argv[1], 'rb').read())
mutated[8] = 3
open(sys.argv[2], 'wb').write(mutated)
PY

# 'unsupported' is cause 1; the failed description answers no information.
for variant in power truncated wrongversion; do
  profile=$PROFILE
  [[ $variant == truncated ]] && profile=$TRUNCATED
  [[ $variant == wrongversion ]] && profile=$WRONG_VERSION
  rc=$(color_run "icc-failed-$variant" icc-failed "$profile")
  if [[ $rc != 0 ]] \
      || ! grep -Fq '{"event":"icc_failed","cause":1,"information_denied":"not_ready"}' \
          "$RUNTIME/icc-failed-$variant.log"; then
    echo "rejected profile ($variant) did not fail gracefully: rc=$rc $(cat "$RUNTIME/icc-failed-$variant.log")"
    exit 1
  fi
done

echo "an unrepresentable curve and an invalid profile fail the client description with 'unsupported'"
