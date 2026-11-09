#!/usr/bin/env bash
# A sampled panel curve that is no power has no parametric form. The output and preferred descriptions state the
# primaries and the ICC file exactly and send no transfer event, and get_preferred_parametric — which promises a
# parametric description — fails with 'unsupported' instead of returning one that no parametric client can use.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

color_fixture 0 1.8 sampled
readonly ICC_SIZE=$(wc -c < "$PROFILE")

color_config "color_profile = \"$PROFILE\"
sdr_gamut = \"srgb\""
if ! color_fields_are "$PROFILE" srgb true true; then
  echo "sampled-curve profile did not stage the gamut transform: $("$UMBRIEL" color --json)"
  exit 1
fi

if ! color_probe color-profile-sampled v1 probe; then
  echo "the color feedback client did not collect the sampled-curve descriptions:" \
    "$(cat "$INFO_LOG" "$INFO_ERR")"
  exit 1
fi

# No tf event at all: the profile is the only exact statement of this curve.
if ! jq -se --argjson size "$ICC_SIZE" '
  . as $log
  | ["output", "preferred"] | all(. as $source
    | ([$log[] | select(.event == "description" and .source == $source)] | last) as $d
    | $d.tf == 0 and $d.tf_power == 0 and $d.primaries == 0 and $d.icc_size == $size)
' "$INFO_LOG" > /dev/null; then
  echo "a curve with no parametric form advertised a transfer event: $(cat "$INFO_LOG")"
  exit 1
fi

# 'unsupported' is cause 1, and the failed description answers no information.
if ! jq -se '
  any(.[]; .event == "description_failed" and .source == "preferred_parametric" and .cause == 1)
  and ([.[] | select(.event == "description" and .source == "preferred_parametric")] | length) == 0
' "$INFO_LOG" > /dev/null; then
  echo "get_preferred_parametric did not fail for a curve with no parametric form: $(cat "$INFO_LOG")"
  exit 1
fi
for source in output preferred; do
  if ! cmp -s "$PROFILE" "$DUMP.$source.0"; then
    echo "icc_file bytes for $source did not match the configured profile exactly"
    exit 1
  fi
done

echo "a sampled non-power curve advertises primaries and icc_file only, and the parametric request fails"
