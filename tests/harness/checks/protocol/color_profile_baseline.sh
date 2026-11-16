#!/usr/bin/env bash
# Without a characterization nothing is advertised or mapped: the output reports inert fields and untagged red stays
# bit-identical.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

color_config ''
if ! color_fields_are none native false false; then
  echo "inert output did not report inert fields: $("$UMBRIEL" color --json)"
  exit 1
fi

color_start_client baseline untagged
color_fill FFFF0000
color_assert_pass_through 'baseline'

echo "an uncharacterized output maps nothing and passes untagged red through"
