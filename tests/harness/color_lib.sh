#!/usr/bin/env bash
# Fixtures, probes and the client lifecycle shared by the ICC characterization checks (protocol/color_profile*).
# Each check runs one scenario in its own headless instance; this file only supplies the pieces every scenario needs.
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_COLOR_FEEDBACK_CLIENT:?color feedback client path is required}"
readonly RUNTIME="$UMBRIEL_RUNTIME_DIR"
readonly PROFILE="$RUNTIME/color-profile.icc"
readonly PADDED="$RUNTIME/color-profile-padded.icc"
readonly SPARSE="$RUNTIME/icc-sparse.bin"
readonly DUMP="$RUNTIME/icc-dump"
readonly SHOT="$RUNTIME/color-profile.png"
BASELINE=$(< "$UMBRIEL_CONFIG")
readonly BASELINE

# The pixel client runs in the background behind a FIFO, so any exit between starting it and its own teardown must
# still reap it and close the writer.
client_pid=
CLIENT_FD=
color_cleanup() {
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
trap color_cleanup EXIT

# The standard fixture: Display-P3-like colorants with a gamma 1.8 TRC (wide enough to desaturate pure red under
# "srgb", far from any named curve). FIXTURE_XY holds the colorants the generator wrote, so checks can assert the
# round trip against its own values.
FIXTURE_XY=()
color_fixture() {
  read -r -a FIXTURE_XY <<<"$(write_icc_matrix_profile "$PROFILE" "$@")"
}

# Replace the output section, reload and wait for the new state to apply.
color_config() {
  printf '%s\n\n[animation]\nenabled = false\n[output.HEADLESS-1]\n%s\n' "$BASELINE" "$1" > "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" settle > /dev/null
}

# color_fields_are <color_profile> <sdr_gamut> <staged> <panel-fields-present>
color_fields_are() {
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

# One client per pixel phase: `output-desc` snapshots the output description when the client starts, so a client
# that must follow the characterization has to be started after it is live.
color_start_client() {
  local name=$1
  shift
  CLIENT_LOG="$RUNTIME/color-$name.log"
  mkfifo "$RUNTIME/color-$name.fifo"
  "$CLIENT" "$@" < "$RUNTIME/color-$name.fifo" > "$CLIENT_LOG" 2>&1 &
  client_pid=$!
  exec {CLIENT_FD}> "$RUNTIME/color-$name.fifo"
  await_lines "$CLIENT_LOG" '{"event":"mapped"}' 1
}

color_stop_client() {
  color_cleanup
  "$UMBRIEL" settle > /dev/null
}

# Solid fill plus its snapshot event, so the next capture is fully painted.
color_fill() {
  printf 'color %s\n' "$1" >&"$CLIENT_FD"
  await_lines "$CLIENT_LOG" "{\"event\":\"snapshot\",\"label\":\"color $1\"" 1
  "$UMBRIEL" settle > /dev/null
}

# Probe run: stdout stays pure JSON for jq, stderr keeps the human-readable messages.
color_probe() {
  local name=$1
  shift
  INFO_LOG="$RUNTIME/color-$name.log"
  INFO_ERR="$RUNTIME/color-$name.err"
  UMBRIEL_ICC_DUMP="$DUMP" "$CLIENT" "$@" > "$INFO_LOG" 2> "$INFO_ERR"
}

# One-shot client invocation; echoes its exit status so the caller can assert on protocol errors.
color_run() {
  local log="$RUNTIME/$1.log"
  shift
  set +e
  "$CLIENT" "$@" > "$log" 2>&1
  local rc=$?
  set -e
  echo "$rc"
}

# Fully saturated untagged red, sampled at the center the captured output actually has.
color_sample_red() {
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
      # The mapped band brackets the measured desaturated red (229 38 27): red stays dominant but below pass-through
      # while green and blue rise above their pass-through floor. A blank or black frame matches none of it.
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

color_assert_pass_through() {
  local phase=$1 pixel
  if ! pixel=$(color_sample_red pass-through); then
    echo "$phase: untagged red was not pass-through: $pixel"
    exit 1
  fi
}

color_assert_mapped() {
  local pixel
  if ! pixel=$(color_sample_red mapped); then
    echo "srgb gamut mapping did not desaturate untagged red: $pixel"
    exit 1
  fi
}

# Center pixel of the captured output, read once the frame has settled.
color_sample_center() {
  local size
  grim "$SHOT"
  size=$("$UMBRIEL_PIXEL_PROBE" "$SHOT" size)
  "$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel "$((${size%x*} / 2))" "$((${size#*x} / 2))"
}

color_pid() {
  pgrep -f -- "-c $UMBRIEL_CONFIG" | head -1
}

color_fd_count() {
  ls /proc/"$1"/fd 2>/dev/null | wc -l
}

color_memfd_count() {
  ls -l /proc/"$1"/fd 2>/dev/null | grep -c 'memfd:umbriel-icc-profile' || true
}
