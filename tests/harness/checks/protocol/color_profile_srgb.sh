#!/usr/bin/env bash
# sdr_gamut = "srgb" makes the panel the destination: untagged red is mapped into the profiled gamut, and the output and
# preferred descriptions carry the panel's custom primaries, its curve as tf_power, and the exact profile bytes.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

color_fixture
readonly ICC_SIZE=$(wc -c < "$PROFILE")
readonly ICC_HEAD=$(od -An -tx1 -N8 "$PROFILE" | tr -d ' \n')
ICC_TF_POWER=$(icc_curve_tf_power "$PROFILE")
readonly ICC_TF_POWER

color_config "color_profile = \"$PROFILE\"
sdr_gamut = \"srgb\""
if ! color_fields_are "$PROFILE" srgb true true; then
  echo "srgb policy did not stage the gamut transform: $("$UMBRIEL" color --json)"
  exit 1
fi
if ! "$UMBRIEL" color --json | jq -e --arg profile "$PROFILE" '.outputs[0].color_profile_resolved == $profile' > /dev/null; then
  echo "the applied profile did not report itself as the resolved characterization: $("$UMBRIEL" color --json)"
  exit 1
fi

color_start_client srgb untagged output-desc
color_fill FFFF0000
color_assert_mapped

# Untagged SDR is sRGB, so the shadow end of the ramp is where a substituted
# decode shows: the panel encode of the sRGB-decoded value, not of the panel's
# (gamma 1.8 read as 2.2 lands on the code itself instead of 14 and 24).
for pair in 16:14 32:24; do
  code=${pair%%:*}
  expected=${pair##*:}
  color_fill "FF$(printf '%02X' "$code")$(printf '%02X' "$code")$(printf '%02X' "$code")"
  measured=$(color_sample_center)
  if ! color_pixels_close "$measured" "$expected $expected $expected"; then
    echo "untagged code $code decoded with the wrong curve: measured [$measured], the sRGB interpretation" \
      "gives $expected"
    exit 1
  fi
done

# Content tagged with the output's own description is already in the panel's
# encoding: it must decode through the advertised curve and land unchanged,
# while the same untagged gray takes the sRGB -> panel conversion.
color_fill FF808080
untagged_gray=$(color_sample_center)
printf 'tag\n' >&"$CLIENT_FD"
await_lines "$CLIENT_LOG" '{"event":"snapshot","label":"tag"' 1
"$UMBRIEL" settle > /dev/null
tagged_gray=$(color_sample_center)
if ! color_pixels_close "$tagged_gray" "128 128 128"; then
  echo "content tagged with the output description did not decode through its curve:" \
    "untagged [$untagged_gray] tagged [$tagged_gray]"
  exit 1
fi
if [[ ${untagged_gray%% *} -ge 120 ]]; then
  echo "untagged gray was not converted into the panel encoding: [$untagged_gray]"
  exit 1
fi
if ! color_pixels_close "$untagged_gray" "109 109 109"; then
  echo "untagged gray did not decode as sRGB before the panel encode:" \
    "measured [$untagged_gray], the sRGB interpretation gives 109 (a gamma 2.2 decode gives 110)"
  exit 1
fi


# The primaries assertion is a round-trip contract against the generator's own colorants (the daemon must report the
# file as written); the parser math itself is pinned by tests/unit/colorimetry.cpp.
if ! color_probe color-profile-srgb v1 probe; then
  echo "the color feedback client did not collect output/preferred descriptions:" \
    "$(cat "$INFO_LOG" "$INFO_ERR")"
  exit 1
fi
if ! jq -se --argjson size "$ICC_SIZE" --arg head "$ICC_HEAD" --argjson power "$ICC_TF_POWER" --arg ex "${FIXTURE_XY[*]}" '
  def near($a; $b): (($a - $b) | if . < 0 then -. else . end) <= 30;
  ($ex | split(" ") | map(tonumber * 1000000)) as $want
  | . as $log
  | ["output", "preferred"] | all(. as $source
    | ([$log[] | select(.event == "description" and .source == $source)] | last) as $d
    | $d.tf == 0 and $d.tf_power == $power and $d.primaries == 0
      and $d.icc_size == $size and $d.icc_head == $head
      and near($d.primaries_xy[0]; $want[0]) and near($d.primaries_xy[1]; $want[1])
      and near($d.primaries_xy[2]; $want[2]) and near($d.primaries_xy[3]; $want[3])
      and near($d.primaries_xy[4]; $want[4]) and near($d.primaries_xy[5]; $want[5])
      and near($d.primaries_xy[6]; $want[6]) and near($d.primaries_xy[7]; $want[7]))
' "$INFO_LOG" > /dev/null; then
  echo "staged descriptions lacked custom primaries, tf_power or icc_file bytes: $(cat "$INFO_LOG")"
  exit 1
fi
for source in output preferred preferred_parametric; do
  if ! cmp -s "$PROFILE" "$DUMP.$source.0"; then
    echo "icc_file bytes for $source did not match the configured profile exactly"
    exit 1
  fi
done

echo "srgb gamut mapping, the panel primaries and tf_power, and the exact icc_file bytes verified live"
