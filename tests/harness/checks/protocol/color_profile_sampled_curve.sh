#!/usr/bin/env bash
# A sampled panel curve that is no power has no parametric form. The output and preferred descriptions state the
# primaries and the ICC file exactly and send no transfer event; content tagged with them decodes through the decode
# table the description carries. get_preferred_parametric still owes an immediate ready (it inherits get_preferred's
# contract), answered with the sRGB working encoding the compositor converts into the characterized output.
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

# The parametric request must still answer: get_preferred_parametric inherits
# get_preferred's immediate-ready contract, so a curve with no parametric form
# is answered with the sRGB working encoding the compositor converts into the
# characterized output. This probe binds v1, where the wire name is srgb.
if ! jq -se '
  ([.[] | select(.event == "description" and .source == "preferred_parametric")] | last) as $d
  | $d.tf == 9 and $d.primaries == 1 and $d.icc_size == 0
    and ([.[] | select(.event == "description_failed" and .source == "preferred_parametric")] | length) == 0
' "$INFO_LOG" > /dev/null; then
  echo "get_preferred_parametric did not answer with a usable parametric description: $(cat "$INFO_LOG")"
  exit 1
fi
# The substituted working encoding is a different description record from the
# ICC-backed preferred one, so the two must not share an identity (ready2: two
# records cannot carry the same id at once).
if ! jq -se '
  ([.[] | select(.event == "description" and .source == "preferred")] | last) as $p
  | ([.[] | select(.event == "description" and .source == "preferred_parametric")] | last) as $q
  | $p.identity != 0 and $q.identity != 0 and $p.identity != $q.identity
' "$INFO_LOG" > /dev/null; then
  echo "get_preferred_parametric reused the preferred description's identity for a different record:" \
    "$(jq -sc '[.[] | select(.event == "description") | {source, identity}]' "$INFO_LOG")"
  exit 1
fi
for source in output preferred; do
  if ! cmp -s "$PROFILE" "$DUMP.$source.0"; then
    echo "icc_file bytes for $source did not match the configured profile exactly"
    exit 1
  fi
done

# The sampled curve has no exponent, so the description carries a 3x1D decode
# table instead. Tagging a buffer with it must land unchanged: the panel
# encoding decodes through that table and the output re-encodes it.
color_start_client sampled-output untagged output-desc
color_fill FF808080
untagged_gray=$(color_sample_center)
printf 'tag\n' >&"$CLIENT_FD"
await_lines "$CLIENT_LOG" '{"event":"snapshot","label":"tag"' 1
"$UMBRIEL" settle > /dev/null
tagged_gray=$(color_sample_center)
if ! color_pixels_close "$tagged_gray" "128 128 128"; then
  echo "content tagged with a sampled-curve output description did not decode through it:" \
    "untagged [$untagged_gray] tagged [$tagged_gray]"
  exit 1
fi
if [[ ${untagged_gray%% *} -ge 120 ]]; then
  echo "untagged gray was not converted into the sampled panel encoding: [$untagged_gray]"
  exit 1
fi
color_stop_client

# The parametric preference is a renderable working encoding, not just a ready
# event: content tagged with it must render exactly as untagged content does.
color_start_client sampled-parametric untagged parametric-desc
color_fill FF808080
parametric_untagged=$(color_sample_center)
printf 'parametric\n' >&"$CLIENT_FD"
await_lines "$CLIENT_LOG" '{"event":"snapshot","label":"parametric"' 1
"$UMBRIEL" settle > /dev/null
parametric_tagged=$(color_sample_center)
if ! color_pixels_close "$parametric_tagged" "$parametric_untagged"; then
  echo "content tagged with the parametric preference did not render like untagged content:" \
    "untagged [$parametric_untagged] tagged [$parametric_tagged]"
  exit 1
fi

echo "a sampled curve keeps primaries and icc_file, answers the parametric request with the sRGB working encoding, and renders both tagged paths exactly"
