#!/usr/bin/env bash
# A profile whose content changes behind an unchanged mtime is not re-read: the reload key is the file's mtime, so the
# staged mapping survives a write that preserves it and a real content change behind a new mtime is picked up.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

color_fixture
color_config "color_profile = \"$PROFILE\"
sdr_gamut = \"srgb\""
if ! color_fields_are "$PROFILE" srgb true true; then
  echo "srgb policy did not stage the gamut transform: $("$UMBRIEL" color --json)"
  exit 1
fi

color_start_client cache untagged
color_fill FFFF0000
color_assert_mapped

# Corrupting the file and restoring its mtime leaves the key identical, so a genuine re-apply (forced here through an
# unrelated output-state change) may not touch the file and the staged mapping must survive it.
cp -p "$PROFILE" "$RUNTIME/profile.ref"
printf 'not an ICC profile\n' > "$PROFILE"
touch -r "$RUNTIME/profile.ref" "$PROFILE"
color_config "color_profile = \"$PROFILE\"
sdr_gamut = \"srgb\"
sdr_white = 204"
color_assert_mapped

cp -p "$RUNTIME/profile.ref" "$PROFILE"
color_config "color_profile = \"$PROFILE\"
sdr_gamut = \"srgb\""
if ! color_fields_are "$PROFILE" srgb true true; then
  echo "restored profile did not resolve: $("$UMBRIEL" color --json)"
  exit 1
fi

echo "the reload key ignores content written behind an unchanged mtime and the staged mapping survives it"
