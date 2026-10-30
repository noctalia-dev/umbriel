#!/usr/bin/env bash
# A native client receives coherent output/preferred metadata and keeps its PQ tag through the compositor path.
# Headless outputs remain SDR, so changing the HDR white level must neither change SDR metadata nor notify clients.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_COLOR_FEEDBACK_CLIENT:?color feedback client path is required}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/color-feedback.log"
readonly BASELINE=$(< "$UMBRIEL_CONFIG")

write_config() {
  printf '%s\n\n[output.HEADLESS-1]\nhdr = "auto"\nsdr_white = %s\n' "$BASELINE" "$1" > "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}

snapshot() {
  printf '%s\n' "$1" >&"$CLIENT_FD"
  await_lines "$CLIENT_LOG" "{\"event\":\"snapshot\",\"label\":\"$1\"" 1
  jq -cs --arg label "$1" '.[] | select(.event == "snapshot" and .label == $label)' "$CLIENT_LOG"
}

assert_sdr_descriptions() {
  local log=${1:-$CLIENT_LOG}
  if ! jq -se '
    [.[] | select(.event == "description")] as $descriptions
    | ["output", "preferred", "preferred_parametric"] | all(. as $source
        | [$descriptions[] | select(.source == $source)] | last
        | .reference == 80 and .max == 80 and .min <= (.reference * 10000)
          and .target_max == 80 and .tf == 2 and .primaries == 1)
  ' "$log" > /dev/null; then
    echo "native SDR descriptions were missing or incoherent: $(cat "$log")"
    exit 1
  fi
}

assert_preferred_identity() {
  local log=$1 version=$2
  if ! jq -se --argjson version "$version" '
    [.[] | select(.event == "description" and (.source == "preferred" or .source == "preferred_parametric"))]
    | length >= 2 and all(.[]; .identity > 0 and .ready_version == $version)
      and ([.[].identity] | unique | length) == 1
  ' "$log" > /dev/null; then
    echo "preferred identity changed without a preference change, or used the wrong ready event: $(cat "$log")"
    exit 1
  fi
}

assert_surface() {
  local transfer=$1 requested=$2 color
  for _ in $(seq 60); do
    color=$("$UMBRIEL" color --json)
    if jq -e --arg transfer "$transfer" --argjson requested "$requested" '
      .outputs[0].hdr_requested == $requested and .outputs[0].hdr_active == false
      and (.surfaces[] | select(.app_id == "color-feedback-client")
        | .transfer_function == $transfer and .primaries == (if $transfer == "PQ" then "BT.2020" else "none" end))
    ' <<< "$color" > /dev/null; then
      return 0
    fi
    sleep 0.05
  done
  echo "native surface metadata did not transition to $transfer: $color"
  exit 1
}

write_config 500
"$CLIENT" v1 > "$UMBRIEL_RUNTIME_DIR/color-feedback-v1.log" 2>&1
assert_sdr_descriptions "$UMBRIEL_RUNTIME_DIR/color-feedback-v1.log"
assert_preferred_identity "$UMBRIEL_RUNTIME_DIR/color-feedback-v1.log" 1
if ! jq -se 'any(.[]; .event == "manager" and .version == 1)' \
    "$UMBRIEL_RUNTIME_DIR/color-feedback-v1.log" > /dev/null; then
  echo "native color client did not bind protocol version 1"
  exit 1
fi
mkfifo "$UMBRIEL_RUNTIME_DIR/color-feedback.fifo"
"$CLIENT" subsurface < "$UMBRIEL_RUNTIME_DIR/color-feedback.fifo" > "$CLIENT_LOG" 2>&1 &
exec {CLIENT_FD}> "$UMBRIEL_RUNTIME_DIR/color-feedback.fifo"
await_lines "$CLIENT_LOG" '{"event":"mapped"}' 1
initial=$(snapshot initial)
assert_sdr_descriptions
assert_surface PQ true

# icc_v2_v4 (feature 0) is advertised only in builds with the real ICC parser;
# probe the shipped binary's own undefined lcms2 symbols instead of assuming
# the feature list (ldd is useless here: unrelated libraries can pull liblcms2
# into the transitive closure). grep -c, not -q: under pipefail an early-exit
# grep SIGPIPEs nm and the pipeline reports failure.
expected_features='[0, 1, 2, 3, 4, 5, 6, 7]'
cms_symbols=$(nm -D --undefined-only "$UMBRIEL" 2>/dev/null | grep -cE '\bcms[A-Z]' || true)
if ((cms_symbols == 0)); then
  expected_features='[1, 2, 3, 4, 5, 6, 7]'
fi
if ! jq -se --argjson features "$expected_features" '
  any(.[]; .event == "manager" and .version == 2)
  and ([.[] | select(.event == "feature") | .value] | unique | sort) == $features
' "$CLIENT_LOG" > /dev/null; then
  echo "native color manager capabilities were incorrect: $(cat "$CLIENT_LOG")"
  exit 1
fi

snapshot unset > /dev/null
assert_surface none false
snapshot tag > /dev/null
assert_surface PQ true

# A synchronized child caches the committed SDR state. A newer pending PQ description must not replace that cache
# when only the parent commits. The next child commit applies the pending PQ state with its matching buffer state.
snapshot cache > /dev/null
assert_surface PQ true
snapshot parent > /dev/null
assert_surface none false
snapshot apply > /dev/null
assert_surface PQ true

# Removing only the color object clears its applied tag while preserving the underlying surface and its buffer.
# Creating another color object for that same surface must then accept a fresh PQ description.
snapshot destroy > /dev/null
assert_surface none false
snapshot recreate > /dev/null
assert_surface PQ true

write_config 350
changed=$(snapshot changed)
assert_sdr_descriptions
if ! jq -en --argjson before "$initial" --argjson after "$changed" '
  $after.output_changes == $before.output_changes and $after.preferred_changes == $before.preferred_changes
' > /dev/null; then
  echo "HDR white change notified an SDR client: initial=$initial changed=$changed"
  exit 1
fi
write_config 350
unchanged=$(snapshot unchanged)
assert_sdr_descriptions
if ! jq -en --argjson before "$changed" --argjson after "$unchanged" '
  $after.output_changes == $before.output_changes and $after.preferred_changes == $before.preferred_changes
' > /dev/null; then
  echo "unchanged reload emitted color feedback: changed=$changed unchanged=$unchanged"
  exit 1
fi
assert_preferred_identity "$CLIENT_LOG" 2

for version in v1 v2 v3; do
  log="$UMBRIEL_RUNTIME_DIR/native-windows-scrgb-$version.log"
  "$CLIENT" windows-scrgb "$version" > "$log" 2>&1
  if ! grep -q '^{"event":"windows_scrgb_ready"}$' "$log"; then
    echo "native $version Windows scRGB request did not produce a ready description: $(cat "$log")"
    exit 1
  fi
  # Gamescope requires all six advertised features even when its final buffer is PQ.
  if ! jq -se '
    [.[] | select(.event == "feature") | .value] as $features
    | [1, 2, 4, 5, 6, 7] | all(. as $feature | $features | index($feature) != null)
  ' "$log" > /dev/null; then
    echo "native $version color manager is missing Gamescope capabilities: $(cat "$log")"
    exit 1
  fi
done
for mode in named-raw raw-named raw-raw luminances-twice invalid-maximum invalid-reference degenerate; do
  "$CLIENT" "params-$mode" > "$UMBRIEL_RUNTIME_DIR/native-params-$mode.log" 2>&1
done
"$CLIENT" duplicate > "$UMBRIEL_RUNTIME_DIR/native-duplicate.log" 2>&1
"$CLIENT" inert > "$UMBRIEL_RUNTIME_DIR/native-inert.log" 2>&1

echo "native SDR feedback, stable identities, synchronized PQ transitions, scRGB and parametric validation verified"
