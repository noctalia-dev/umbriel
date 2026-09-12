#!/usr/bin/env bash
# SDR10 format-sequence integration coverage.
#   1. VRR + SDR10: VRR is unsupported on headless. Compositor warns and commits XR30.
#   2. VRR off with SDR10 active: Removing VRR config while SDR10 is live keeps XR30/XB30.
#   3. DPMS cycle: Powering off then on while bit_depth=10 is configured re-selects XR30/XB30.
#   4. HDR failure: SDR10 with a live window: HDR fails (no PQ on headless), SDR10 then
#      commits XR30/XB30 independently.
#   5. Mode + SDR10: Configuring a mode alongside bit_depth=10 does not suppress SDR10
#      selection (headless accepts the mode as a custom commit).
set -euo pipefail

BASELINE=$(< "$UMBRIEL_CONFIG")
CLIENT_PID=

# -- Phase 1: VRR + SDR10 ------------------------------------------------------
# Headless does not set adaptive_sync_supported, so the compositor logs the VRR
# warning and proceeds without VRR. SDR10 must still commit XR30 or XB30.
printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
printf '\n[output.HEADLESS-1]\nbit_depth = 10\nvrr = "always"\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

if ! grep -F "output 'HEADLESS-1': VRR requested but adaptive sync is not supported" "$UMBRIEL_LOG" > /dev/null; then
  echo "vrr-sdr10: missing expected VRR-not-supported warning in log"
  exit 1
fi

color=$("$UMBRIEL" color --json)
if ! jq -e '
  .outputs[0].bit_depth == 10
  and .outputs[0].sdr10_active == true
  and .outputs[0].bit_depth_fallback_reason == ""
  and (.outputs[0].render_format == "XR30" or .outputs[0].render_format == "XB30")
' <<< "$color" > /dev/null; then
  echo "vrr-sdr10: unexpected color state: $color"
  exit 1
fi
echo "vrr-sdr10: VRR warning logged, SDR10 committed to $("$UMBRIEL" color --json | jq -r '.outputs[0].render_format')"

# -- Phase 2: VRR off while SDR10 active ---------------------------------------
# Removing vrr = "always" (reverts to disabled) while bit_depth=10 stays active.
# The output must stay on XR30/XB30 with no bit_depth fallback reason.
printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
printf '\n[output.HEADLESS-1]\nbit_depth = 10\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

color=$("$UMBRIEL" color --json)
if ! jq -e '
  .outputs[0].bit_depth == 10
  and .outputs[0].sdr10_active == true
  and .outputs[0].bit_depth_fallback_reason == ""
  and (.outputs[0].render_format == "XR30" or .outputs[0].render_format == "XB30")
' <<< "$color" > /dev/null; then
  echo "vrr-off-sdr10: unexpected color state after VRR removal: $color"
  exit 1
fi
echo "vrr-off-sdr10: output retained XR30/XB30 after VRR disabled"

# -- Phase 3: DPMS cycle re-selects SDR10 format -------------------------------
# Power the output off and back on while bit_depth=10 is still configured.
# configure() must run on re-enable and select XR30/XB30 again.
"$UMBRIEL" msg dpms-off > /dev/null
sleep 0.1
"$UMBRIEL" msg dpms-on > /dev/null
sleep 0.2

color=$("$UMBRIEL" color --json)
if ! jq -e '
  .outputs[0].bit_depth == 10
  and .outputs[0].sdr10_active == true
  and .outputs[0].bit_depth_fallback_reason == ""
  and (.outputs[0].render_format == "XR30" or .outputs[0].render_format == "XB30")
' <<< "$color" > /dev/null; then
  echo "dpms-cycle: output did not reselect XR30/XB30 after DPMS cycle: $color"
  exit 1
fi
echo "dpms-cycle: XR30/XB30 re-selected after DPMS off/on"

# -- Phase 4: HDR failure -> SDR10 with a live window -------------------------
# Open a foot window so the render pipeline is warm (blur/corner_radius geometry
# is live), then configure hdr+bit_depth=10. HDR fails on headless (no PQ), the
# SDR10 probe runs next and commits XR30/XB30. The FX render path must survive
# the composite HDR-fail + SDR10-commit sequence.
foot --config=/dev/null sh -c 'while :; do sleep 1; done' > /dev/null 2>&1 &
CLIENT_PID=$!
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -ge 1 ]] && break
  sleep 0.1
done
if [[ $("$UMBRIEL" windows --json | jq 'length') -lt 1 ]]; then
  echo "hdr-fail-sdr10: foot window never mapped"
  exit 1
fi

printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
printf '\n[output.HEADLESS-1]\nhdr = "on"\nbit_depth = 10\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

color=$("$UMBRIEL" color --json)
if ! jq -e '
  .outputs[0].hdr_requested == true
  and .outputs[0].hdr_active == false
  and (.outputs[0].fallback_reason | length) > 0
  and .outputs[0].bit_depth == 10
  and .outputs[0].sdr10_active == true
  and .outputs[0].bit_depth_fallback_reason == ""
  and (.outputs[0].render_format == "XR30" or .outputs[0].render_format == "XB30")
' <<< "$color" > /dev/null; then
  echo "hdr-fail-sdr10: unexpected color state with live window: $color"
  exit 1
fi
echo "hdr-fail-sdr10: HDR fallback set, SDR10 committed while render pipeline was warm"

kill "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=

# -- Phase 5: SDR10 with a configured mode ------------------------------------
# Configure bit_depth=10 alongside a mode spec. On headless the mode is committed
# as a custom mode (headless has no fixed mode list), so no mode-fallback warning
# is emitted. SDR10 is still selected and there is no bit_depth_fallback_reason.
# i.e. the format sequence runs correctly regardless of whether the mode came
# from the mode list or a custom commit.
printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
printf '\n[output.HEADLESS-1]\nbit_depth = 10\nmode = "9999x9999@999"\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

color=$("$UMBRIEL" color --json)
if ! jq -e '
  .outputs[0].bit_depth == 10
  and .outputs[0].sdr10_active == true
  and .outputs[0].bit_depth_fallback_reason == ""
  and (.outputs[0].render_format == "XR30" or .outputs[0].render_format == "XB30")
' <<< "$color" > /dev/null; then
  echo "mode-with-sdr10: unexpected color state with mode+bit_depth configured: $color"
  exit 1
fi
echo "mode-with-sdr10: SDR10 committed correctly alongside a configured mode"
