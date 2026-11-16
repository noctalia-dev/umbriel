#!/usr/bin/env bash
# "edid" on a headless output has no EDID to read: the characterization stays inert, gamut mapping is not staged, and
# pixels are untouched.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

color_config 'color_profile = "edid"
sdr_gamut = "srgb"'
if ! color_fields_are edid srgb false false; then
  echo "EDID characterization was not inert on headless: $("$UMBRIEL" color --json)"
  exit 1
fi
if ! "$UMBRIEL" color --json | jq -e '.outputs[0].color_profile_resolved == "none"' > /dev/null; then
  echo "a headless output with no EDID reported a resolved characterization: $("$UMBRIEL" color --json)"
  exit 1
fi

color_start_client edid untagged
color_fill FFFF0000
color_assert_pass_through 'edid fallback'

echo "EDID characterization is inert on a headless output and leaves pixels untouched"
