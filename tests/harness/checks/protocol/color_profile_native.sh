#!/usr/bin/env bash
# An ICC characterization with sdr_gamut = "native" stages no transform, so pixels stay bit-identical — including for a
# client that tags its buffer with the output's own description. That description must therefore name the sRGB encoding
# the renderer really writes, not the panel's colorimetry.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

color_fixture
color_config "color_profile = \"$PROFILE\"
sdr_gamut = \"native\""
if ! color_fields_are "$PROFILE" native false true; then
  echo "characterized native output did not report panel fields: $("$UMBRIEL" color --json)"
  exit 1
fi
color=$("$UMBRIEL" color --json)
if ! jq -e '
  .outputs[0] as $o
  | $o.panel_transfer == "curve"
    and ($o.panel_primaries | [.red, .green, .blue, .white] | all(has("x") and has("y")))
' <<< "$color" > /dev/null; then
  echo "panel characterization did not surface in color --json: $color"
  exit 1
fi

# `output-desc` makes `tag` apply the output's own description, so this client compares following the advertisement
# with ignoring it.
color_start_client native untagged output-desc
color_fill FFFF0000
color_assert_pass_through 'profile with native gamut'

# The renderer composes into sRGB here, so that is what the output advertises: the panel's colorimetry must stay out
# of the description (and out of icc_file), or a client would convert into an encoding we never produce.
if ! color_probe color-profile-native v1 probe; then
  echo "the color feedback client did not collect output/preferred descriptions:" \
    "$(cat "$INFO_LOG" "$INFO_ERR")"
  exit 1
fi
if ! jq -se '
  . as $log
  | ["output", "preferred"] | all(. as $source
    | ([$log[] | select(.event == "description" and .source == $source)] | last) as $d
    | $d.tf == 9 and $d.primaries == 1 and $d.icc_size == 0)
' "$INFO_LOG" > /dev/null; then
  echo "native output advertised something other than its sRGB destination: $(cat "$INFO_LOG")"
  exit 1
fi

# The pixel contract behind that advertisement: tagging the buffer with the output's own description must not move a
# single pixel.
color_fill FF807060
untagged_pixel=$(color_sample_center)
printf 'tag\n' >&"$CLIENT_FD"
await_lines "$CLIENT_LOG" '{"event":"snapshot","label":"tag"' 1
"$UMBRIEL" settle > /dev/null
tagged_pixel=$(color_sample_center)
if [[ $untagged_pixel != "128 112 96" || $tagged_pixel != "$untagged_pixel" ]]; then
  echo "the native output description did not match its rendering:" \
    "untagged [$untagged_pixel] tagged [$tagged_pixel]"
  exit 1
fi

echo "characterized native output stages nothing, advertises sRGB and renders tagged frames identically"
