#!/usr/bin/env bash
# 10-bit SDR format selection.
#   0. umbrielfx rendering phase: corner_radius + optimized blur active when the
#      bit-depth switches. Exercises fx_pass.c and the FP16 offscreen buffer path.
#   1. SDR8 -> SDR10: Probe succeeds, output commits to XR30 or XB30, no fallback reason.
#   2. HDR (unavailable) -> SDR10: HDR reason clears when HDR is no longer
#      requested; SDR10 independently selects XR30 or XB30.
#   3. HDR unavailable + SDR10 configured: The HDR reason stays set while SDR10
#      commits XR30 or XB30 successfully.
#   4. SDR10 -> SDR8: No fallback reason, output returns to XR24.
set -euo pipefail

readonly SCREENSHOT_SDR8="$UMBRIEL_RUNTIME_DIR/sdr10-luma-sdr8.png"
readonly SCREENSHOT_SDR10="$UMBRIEL_RUNTIME_DIR/sdr10-luma-sdr10.png"

BASELINE=$(< "$UMBRIEL_CONFIG")
CLIENT_PID=

# -- Phase 0: umbrielfx rendering ---------------------------------------------
# Enable corner_radius and optimized blur so that the FX renderer populates the
# per-output blur cache and the corner-radius clip geometry in SDR8, then switch
# to bit_depth=10 while the window and effects are live. This exercises the
# fx_pass.c SDR10 render path and the FP16 offscreen buffer selection.
printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#1e1e2eff"

[appearance]
corner_radius = 32

[appearance.blur]
enabled = true
optimized = true
passes = 2
radius = 8
noise = 0.0
brightness = 1.0
contrast = 1.0
saturation = 1.0

[[window_rule]]
blur = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

foot --config=/dev/null sh -c 'while :; do sleep 1; done' > /dev/null 2>&1 &
CLIENT_PID=$!
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -ge 1 ]] && break
  sleep 0.1
done
if [[ $("$UMBRIEL" windows --json | jq 'length') -lt 1 ]]; then
  echo "phase0: foot window never mapped"
  exit 1
fi

# Let two frames render with the FX effects active to populate the blur cache.
sleep 0.3
grim "$SCREENSHOT_SDR8"
luma_sdr8=$(magick "$SCREENSHOT_SDR8" -alpha off -colorspace gray -format '%[fx:round(255*mean)]' info:)
if (( luma_sdr8 == 0 )); then
  echo "phase0: SDR8 screenshot is all-black (mean luma=${luma_sdr8})"
  exit 1
fi
echo "phase0: SDR8 frame rendered with blur/corner_radius active (luma=${luma_sdr8})"

# Switch to SDR10 while the FX-active window is live: the blur cache and
# corner-radius geometry must survive the format transition.
printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#1e1e2eff"

[appearance]
corner_radius = 32

[appearance.blur]
enabled = true
optimized = true
passes = 2
radius = 8
noise = 0.0
brightness = 1.0
contrast = 1.0
saturation = 1.0

[[window_rule]]
blur = true

[output.HEADLESS-1]
bit_depth = 10
EOF
"$UMBRIEL" msg config-reload > /dev/null
sleep 0.3
grim "$SCREENSHOT_SDR10"
luma_sdr10=$(magick "$SCREENSHOT_SDR10" -alpha off -colorspace gray -format '%[fx:round(255*mean)]' info:)
if (( luma_sdr10 == 0 )); then
  echo "phase0: SDR10 screenshot is all-black after format switch (mean luma=${luma_sdr10})"
  exit 1
fi
echo "phase0: SDR10 frame rendered correctly after format switch (luma=${luma_sdr10})"


# Headless accepts XR30 or XB30 via wlr_output_test_state (XR30 is tried first),
# so the probe succeeds and the output commits to 10-bit without a fallback reason.
printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
printf '\n[output.HEADLESS-1]\nbit_depth = 10\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

color=$("$UMBRIEL" color --json)
if ! jq -e '
  .outputs[0].bit_depth == 10
  and .outputs[0].bit_depth_fallback_reason == ""
  and .outputs[0].sdr10_active == true
  and (.outputs[0].render_format == "XR30" or .outputs[0].render_format == "XB30")
' <<< "$color" > /dev/null; then
  echo "sdr8-to-sdr10: unexpected color state: $color"
  exit 1
fi

color_human=$("$UMBRIEL" color)
if ! grep -F "10-bit SDR: active" <<< "$color_human" > /dev/null; then
  echo "sdr8-to-sdr10: missing 10-bit SDR active line in human output: $color_human"
  exit 1
fi

echo "sdr8-to-sdr10: probe succeeded, output committed to 10-bit SDR"

# -- Phase 2: HDR (unavailable) -> SDR10 ---------------------------------------
# Configure HDR first (fails on headless). Then switch to bit_depth=10 without
# HDR. The HDR reason must clear and the output must select XR30 or XB30.
printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
printf '\n[output.HEADLESS-1]\nhdr = "on"\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

color=$("$UMBRIEL" color --json)
if ! jq -e '
  .outputs[0].hdr_requested == true
  and .outputs[0].hdr_active == false
  and (.outputs[0].fallback_reason | length) > 0
  and .outputs[0].render_format == "XR24"
' <<< "$color" > /dev/null; then
  echo "hdr-to-sdr10 setup: unexpected color state: $color"
  exit 1
fi

printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
printf '\n[output.HEADLESS-1]\nbit_depth = 10\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

color=$("$UMBRIEL" color --json)
if ! jq -e '
  .outputs[0].hdr_requested == false
  and .outputs[0].hdr_active == false
  and .outputs[0].fallback_reason == ""
  and .outputs[0].bit_depth == 10
  and .outputs[0].bit_depth_fallback_reason == ""
  and .outputs[0].sdr10_active == true
  and (.outputs[0].render_format == "XR30" or .outputs[0].render_format == "XB30")
  and .outputs[0].transfer_function == "none"
  and .outputs[0].primaries == "none"
' <<< "$color" > /dev/null; then
  echo "hdr-to-sdr10: unexpected color state: $color"
  exit 1
fi

echo "hdr-to-sdr10: HDR reason cleared, SDR10 probe succeeded after transition"

# -- Phase 3: HDR unavailable + SDR10 configured -------------------------------
# When HDR and bit_depth=10 are both configured, the HDR probe fails first
# (headless does not advertise PQ or BT.2020), then the SDR10 probe runs
# independently and succeeds. The HDR fallback reason is set, the SDR10 reason is not.
printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
printf '\n[output.HEADLESS-1]\nhdr = "on"\nbit_depth = 10\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

color=$("$UMBRIEL" color --json)
if ! jq -e '
  .outputs[0].hdr_requested == true
  and .outputs[0].hdr_active == false
  and (.outputs[0].fallback_reason | length) > 0
  and .outputs[0].bit_depth == 10
  and .outputs[0].bit_depth_fallback_reason == ""
  and .outputs[0].sdr10_active == true
  and (.outputs[0].render_format == "XR30" or .outputs[0].render_format == "XB30")
' <<< "$color" > /dev/null; then
  echo "hdr-unavailable-with-sdr10: unexpected color state: $color"
  exit 1
fi

color_human=$("$UMBRIEL" color)
if ! grep -F "10-bit SDR: active" <<< "$color_human" > /dev/null; then
  echo "hdr-unavailable-with-sdr10: missing 10-bit SDR active line in human output: $color_human"
  exit 1
fi

echo "hdr-unavailable-with-sdr10: HDR reason set, SDR10 probe succeeded independently"

# -- Phase 4: SDR10 -> SDR8 ----------------------------------------------------
# Reverting to the default config (no bit_depth override) must return the
# output to XR24 with no bit_depth_fallback_reason.
printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

color=$("$UMBRIEL" color --json)
if ! jq -e '
  .outputs[0].bit_depth == 8
  and .outputs[0].bit_depth_fallback_reason == ""
  and .outputs[0].sdr10_active == false
  and .outputs[0].render_format == "XR24"
' <<< "$color" > /dev/null; then
  echo "sdr10-to-sdr8: unexpected color state: $color"
  exit 1
fi

echo "sdr10-to-sdr8: output returned to XR24, no bit_depth_fallback_reason"

