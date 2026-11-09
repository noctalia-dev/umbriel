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

color_start_client srgb untagged
color_fill FFFF0000
color_assert_mapped

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
