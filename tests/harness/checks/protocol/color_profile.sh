#!/usr/bin/env bash
# A real ICC characterization drives the SDR gamut policy end to end: panel
# fields and custom-preferred descriptions on the wire, the exact profile bytes
# through get_information's icc_file, edid inert on headless, and the create_icc
# contract (ready, graceful failure, information denial, bad_size, bad_fd). The pixel
# pin is the feature's point: "native" stays bit-identical while "srgb" maps
# untagged red into the profiled wide gamut.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_COLOR_FEEDBACK_CLIENT:?color feedback client path is required}"
readonly BASELINE=$(< "$UMBRIEL_CONFIG")
readonly RUNTIME="$UMBRIEL_RUNTIME_DIR"
readonly PROFILE="$RUNTIME/color-profile.icc"
readonly TRUNCATED="$PROFILE.truncated"
readonly WRONG_VERSION="$PROFILE.wrongversion"
readonly DUMP="$RUNTIME/icc-dump"
readonly SHOT="$RUNTIME/color-profile.png"

# A matrix-shaper ICC v2 display profile built byte for byte at check time (no
# fixture files), from the shared generator: Display-P3-like colorants with a
# gamma 1.8 TRC — wide enough to desaturate pure red under "srgb", far enough
# from any named curve for the power-fit path. The generator prints the
# colorants it wrote so checks can assert a round-trip against its own values
# instead of restating them. The truncated and wrong-version copies drive the
# graceful create_icc failure contract.
read -r -a FIXTURE_XY <<<"$(write_icc_matrix_profile "$PROFILE")"
head -c 200 "$PROFILE" > "$TRUNCATED"
python3 - "$PROFILE" "$WRONG_VERSION" <<'PY'
import sys

mutated = bytearray(open(sys.argv[1], 'rb').read())
mutated[8] = 3
open(sys.argv[2], 'wb').write(mutated)
PY

readonly ICC_SIZE=$(wc -c < "$PROFILE")
readonly ICC_HEAD=$(od -An -tx1 -N8 "$PROFILE" | tr -d ' \n')

# The untagged phase runs a backgrounded client fed through a FIFO. Any exit
# between starting it and the phase's own teardown must still reap the client
# and close the FIFO writer, so an induced failure leaves nothing behind.
client_pid=
CLIENT_FD=
cleanup() {
  if [[ -n ${CLIENT_FD:-} ]]; then
    { exec {CLIENT_FD}>&-; } 2>/dev/null || true
    CLIENT_FD=
  fi
  if [[ -n ${client_pid:-} ]] && kill -0 "$client_pid" 2>/dev/null; then
    kill -TERM "$client_pid" 2>/dev/null || true
    wait "$client_pid" 2>/dev/null || true
  fi
  client_pid=
}
trap cleanup EXIT

write_config() {
  printf '%s\n\n[animation]\nenabled = false\n[output.HEADLESS-1]\n%s\n' "$BASELINE" "$1" > "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" settle > /dev/null
}

fields_are() {
  local color_profile=$1 gamut=$2 staged=$3 panel=$4
  "$UMBRIEL" color --json | jq -e \
    --arg profile "$color_profile" --arg gamut "$gamut" --argjson staged "$staged" --argjson panel "$panel" '
      .outputs[0].color_profile == $profile
      and .outputs[0].sdr_gamut == $gamut
      and .outputs[0].sdr_gamut_staged == $staged
      and (.outputs[0].panel_primaries != null) == $panel
      and (.outputs[0].panel_transfer != null) == $panel
    ' > /dev/null
}

# Fully saturated untagged red, sampled at the center the captured output
# actually has, so the pin does not assume one headless mode size.
sample_red() {
  local red green blue size width height x y
  for _ in $(seq 40); do
    grim "$SHOT"
    size=$("$UMBRIEL_PIXEL_PROBE" "$SHOT" size)
    width=${size%x*}
    height=${size#*x}
    x=$((width / 2))
    y=$((height / 2))
    read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel "$x" "$y")
    if [[ $1 == pass-through ]]; then
      if ((red >= 253 && green <= 2 && blue <= 2)); then
        echo "$red $green $blue"
        return 0
      fi
    else
      # The mapped band brackets the measured desaturated red (229 38 27): red
      # stays dominant but below pass-through while green and blue rise above
      # their pass-through floor. A blank or black frame matches none of it.
      if ((red >= 200 && red <= 240 && green >= 12 && blue >= 12)); then
        echo "$red $green $blue"
        return 0
      fi
    fi
    "$UMBRIEL" settle > /dev/null
  done
  echo "$red $green $blue"
  return 1
}

assert_pass_through() {
  local phase=$1 pixel
  if ! pixel=$(sample_red pass-through); then
    echo "$phase: untagged red was not pass-through: $pixel"
    exit 1
  fi
}

assert_mapped() {
  local pixel
  if ! pixel=$(sample_red mapped); then
    echo "srgb gamut mapping did not desaturate untagged red: $pixel"
    exit 1
  fi
}

# --- baseline: no characterization ------------------------------------------
write_config ''
if ! fields_are none native false false; then
  echo "inert output did not report inert fields: $( "$UMBRIEL" color --json)"
  exit 1
fi

CLIENT_LOG="$RUNTIME/color-profile-surface.log"
mkfifo "$RUNTIME/color-profile-surface.fifo"
"$CLIENT" untagged < "$RUNTIME/color-profile-surface.fifo" > "$CLIENT_LOG" 2>&1 &
client_pid=$!
exec {CLIENT_FD}> "$RUNTIME/color-profile-surface.fifo"
await_lines "$CLIENT_LOG" '{"event":"mapped"}' 1
printf 'color FFFF0000\n' >&"$CLIENT_FD"
await_lines "$CLIENT_LOG" '{"event":"snapshot","label":"color FFFF0000"' 1
assert_pass_through 'baseline'

# --- ICC file, sdr_gamut = "native": characterized, pixels untouched ---------
write_config "color_profile = \"$PROFILE\"
sdr_gamut = \"native\""
if ! fields_are "$PROFILE" native false true; then
  echo "characterized native output did not report panel fields: $( "$UMBRIEL" color --json)"
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
assert_pass_through 'profile with native gamut'

# Output and preferred descriptions carry the panel's custom primaries and the
# exact profile bytes on icc_file. The primaries assertion is a round-trip
# contract against the generator's own colorants (the daemon must report the
# file as written); the parser math itself is pinned by tests/unit/colorimetry.cpp.
INFO_LOG="$RUNTIME/color-profile-info.log"
if ! UMBRIEL_ICC_DUMP="$DUMP" "$CLIENT" v1 > "$INFO_LOG" 2>&1; then
  echo "the color feedback client did not collect output/preferred descriptions: $(cat "$INFO_LOG")"
  exit 1
fi
if ! jq -se --argjson size "$ICC_SIZE" --arg head "$ICC_HEAD" --arg ex "${FIXTURE_XY[*]}" '
  def near($a; $b): (($a - $b) | if . < 0 then -. else . end) <= 30;
  ($ex | split(" ") | map(tonumber * 1000000)) as $want
  | . as $log
  | ["output", "preferred"] | all(. as $source
    | ([$log[] | select(.event == "description" and .source == $source)] | last) as $d
    | $d.tf == 2 and $d.primaries == 0 and $d.icc_size == $size and $d.icc_head == $head
      and near($d.primaries_xy[0]; $want[0]) and near($d.primaries_xy[1]; $want[1])
      and near($d.primaries_xy[2]; $want[2]) and near($d.primaries_xy[3]; $want[3])
      and near($d.primaries_xy[4]; $want[4]) and near($d.primaries_xy[5]; $want[5])
      and near($d.primaries_xy[6]; $want[6]) and near($d.primaries_xy[7]; $want[7]))
' "$INFO_LOG" > /dev/null; then
  echo "characterized descriptions lacked custom primaries or icc_file bytes: $(cat "$INFO_LOG")"
  exit 1
fi
for source in output preferred preferred_parametric; do
  if ! cmp -s "$PROFILE" "$DUMP.$source.0"; then
    echo "icc_file bytes for $source did not match the configured profile exactly"
    exit 1
  fi
done

# --- sdr_gamut = "srgb": the opt-in mapping, pinned live ---------------------
write_config "color_profile = \"$PROFILE\"
sdr_gamut = \"srgb\""
if ! fields_are "$PROFILE" srgb true true; then
  echo "srgb policy did not stage the gamut transform: $( "$UMBRIEL" color --json)"
  exit 1
fi
assert_mapped

# --- re-apply must not re-read an unchanged profile (cache key contract) -----
# The key carries the profile's mtime; content changed behind an unchanged
# mtime may not be seen before a real key change. Corrupting the file and
# restoring its mtime leaves the key identical, so a genuine re-apply (forced
# here through an unrelated output-state change) may not touch the file and
# the staged mapping must survive it.
cp -p "$PROFILE" "$RUNTIME/profile.ref"
printf 'not an ICC profile\n' > "$PROFILE"
touch -r "$RUNTIME/profile.ref" "$PROFILE"
write_config "color_profile = \"$PROFILE\"
sdr_gamut = \"srgb\"
sdr_white = 204"
assert_mapped
cp -p "$RUNTIME/profile.ref" "$PROFILE"
write_config "color_profile = \"$PROFILE\"
sdr_gamut = \"srgb\""
if ! fields_are "$PROFILE" srgb true true; then
  echo "restored profile did not resolve: $( "$UMBRIEL" color --json)"
  exit 1
fi

# --- edid on a headless output: warned and inert ----------------------------
write_config 'color_profile = "edid"
sdr_gamut = "srgb"'
if ! fields_are edid srgb false false; then
  echo "EDID characterization was not inert on headless: $( "$UMBRIEL" color --json)"
  exit 1
fi
assert_pass_through 'edid fallback'

kill -TERM "$client_pid"
wait "$client_pid" 2>/dev/null || true
exec {CLIENT_FD}>&-
client_pid=
CLIENT_FD=
"$UMBRIEL" settle > /dev/null

# --- create_icc wire contract ----------------------------------------------
run_icc_mode() {
  local mode=$1 profile=$2 log="$RUNTIME/icc-$1-$3.log"
  set +e
  "$CLIENT" "icc-$mode" "$profile" > "$log" 2>&1
  local rc=$?
  set -e
  echo "$rc"
}

rc=$(run_icc_mode ready "$PROFILE" valid)
if [[ $rc != 0 ]] || ! grep -Fq '{"event":"icc_ready"' "$RUNTIME/icc-ready-valid.log"; then
  echo "create_icc did not reach ready for the valid profile: rc=$rc $(cat "$RUNTIME/icc-ready-valid.log")"
  exit 1
fi
rc=$(run_icc_mode denied "$PROFILE" valid)
if [[ $rc != 0 ]] || ! grep -Fq '{"event":"icc_info_denied"' "$RUNTIME/icc-denied-valid.log"; then
  echo "get_information on a client-created description was not denied: rc=$rc $(cat "$RUNTIME/icc-denied-valid.log")"
  exit 1
fi
for variant in truncated wrongversion; do
  profile=$PROFILE
  [[ $variant == truncated ]] && profile=$TRUNCATED
  [[ $variant == wrongversion ]] && profile=$WRONG_VERSION
  rc=$(run_icc_mode failed "$profile" "$variant")
  # 'unsupported' is cause 1; the failed description answers no information.
  if [[ $rc != 0 ]] \
      || ! grep -Fq '{"event":"icc_failed","cause":1,"information_denied":"not_ready"}' \
          "$RUNTIME/icc-failed-$variant.log"; then
    echo "rejected profile ($variant) did not fail gracefully: rc=$rc $(cat "$RUNTIME/icc-failed-$variant.log")"
    exit 1
  fi
done
# bad_size is a request-contract protocol error: it kills exactly this client
# invocation (exit 2 after observing it) and nothing compositor-side.
rc=$(run_icc_mode oversize "$PROFILE" over)
if [[ $rc != 2 ]] || ! grep -Fq '{"event":"icc_bad_size"' "$RUNTIME/icc-oversize-over.log"; then
  echo "oversized set_icc_file did not raise bad_size for the client: rc=$rc $(cat "$RUNTIME/icc-oversize-over.log")"
  exit 1
fi
if ! "$UMBRIEL" color --json | jq -e '.color_manager == true' > /dev/null; then
  echo "the compositor did not survive the bad_size client: $( "$UMBRIEL" color --json)"
  exit 1
fi

# bad_fd is the request-contract error for an fd that is not a seekable,
# readable regular file: a directory fd must be refused at set_icc_file
# instead of surfacing later as a read failure.
rc=$(run_icc_mode nonregular "$PROFILE" nonreg)
if [[ $rc != 2 ]] || ! grep -Fq '{"event":"icc_bad_fd"' "$RUNTIME/icc-nonregular-nonreg.log"; then
  echo "directory fd did not raise bad_fd for the client: rc=$rc $(cat "$RUNTIME/icc-nonregular-nonreg.log")"
  exit 1
fi
if ! "$UMBRIEL" color --json | jq -e '.color_manager == true' > /dev/null; then
  echo "the compositor did not survive the bad_fd client: $( "$UMBRIEL" color --json)"
  exit 1
fi

echo "ICC characterization, native pass-through, srgb gamut mapping, icc_file bytes and the create_icc contract verified live"
