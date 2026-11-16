#!/usr/bin/env bash
# Window shadows on a gamut-mapped output compose in linear light. The uncharacterized reference pins each pixel's
# coverage (it blends the shadow over white in the encoded space); the mapped output must render the panel encode of
# the same linear composite. A straight-alpha blend into the FP16 buffer also scales the destination alpha, which the
# output transform unpremultiplies by, so a wrong blend mode shows up as a dark shadow ramp.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

color_fixture
color_config "color_profile = \"$PROFILE\"
sdr_gamut = \"native\""

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[colors]
backdrop = "#FFFFFFFF"
shadow = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.title_bar]
mode = "off"

[appearance.shadow]
enabled = true
softness = 60
offset_x = 0
offset_y = 0

[appearance.blur]
enabled = false

[[window_rule]]
match.title = "^shadow-mapped$"
default_floating = true
default_floating_size_px = { width = 480, height = 320 }
default_position = { x = 400, y = 200, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

FILL_COLOR=0x00000000 "$UMBRIEL_UNMAP_CLIENT" shadow-mapped 480 320 > "$UMBRIEL_RUNTIME_DIR/shadow.log" 2>&1 &
await_window shadow-mapped
"$UMBRIEL" settle > /dev/null

read -r x y width height < <(window_by_title shadow-mapped | jq -r '"\(.x) \(.y) \(.w) \(.h)"')
readonly NATIVE_SHOT="$UMBRIEL_RUNTIME_DIR/shadow-native.png"
readonly MAPPED_SHOT="$UMBRIEL_RUNTIME_DIR/shadow-mapped.png"
probe_y=$((y + height / 2))

profile() {
  local image=$1 dx
  for ((dx = -70; dx <= -1; dx += 1)); do
    "$UMBRIEL_PIXEL_PROBE" "$image" mean "1x24+$((x + dx))+$((probe_y - 12))"
  done
}

# phase <shadow colour> <colour alpha>
phase() {
  local colour=$1 alpha=$2
  sed -i "s/^shadow = \"#.*\"\$/shadow = \"$colour\"/" "$UMBRIEL_CONFIG"
  sed -i 's/^sdr_gamut = "srgb"$/sdr_gamut = "native"/' "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" settle > /dev/null
  grim -o HEADLESS-1 "$NATIVE_SHOT"
  local native
  native=$(profile "$NATIVE_SHOT")
  read -r edge_native _ _ < <("$UMBRIEL_PIXEL_PROBE" "$NATIVE_SHOT" pixel "$((x - 1))" "$probe_y")
  if ((edge_native > 230)); then
    echo "no shadow outside the window in the reference run: $edge_native"
    exit 1
  fi
  sed -i 's/^sdr_gamut = "native"$/sdr_gamut = "srgb"/' "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" settle > /dev/null
  if ! color_fields_are "$PROFILE" srgb true true; then
    echo "sdr_gamut = \"srgb\" did not stage the gamut transform"
    exit 1
  fi
  grim -o HEADLESS-1 "$MAPPED_SHOT"
  local mapped
  mapped=$(profile "$MAPPED_SHOT")
  python3 - "$native" "$mapped" "$2" "$1" <<'PY'
import sys

native = [[int(v) for v in line.split()] for line in sys.argv[1].splitlines()]
mapped = [[int(v) for v in line.split()] for line in sys.argv[2].splitlines()]
color_alpha = float(sys.argv[3])
colour = sys.argv[4]
# color_fixture writes a gamma 1.8 profile, so the panel encode is v^(1/1.8).
encode = lambda linear: max(linear, 0.0) ** (1 / 1.8)
worst = 0
worst_line = ""
samples = 0
for index, (reference, observed) in enumerate(zip(native, mapped)):
    # The uncharacterized output blends in the encoded space: out = 1 - alpha * coverage.
    coverage = (1 - reference[0] / 255.0) / color_alpha
    if not 0.05 <= coverage <= 0.9:
        continue
    samples += 1
    expected = round(encode(1 - color_alpha * coverage) * 255)
    delta = abs(observed[0] - expected)
    if delta > worst:
        worst = delta
        worst_line = (
            f"dx={index - 70}: reference {reference[0]} (coverage {coverage:.3f}), observed {observed[0]}, "
            f"linear-light {expected}"
        )
if samples < 20:
    print(f"only {samples} shadow-ramp samples; the check did not see the shadow ramp")
    sys.exit(1)
print(f"{colour}: {samples} ramp samples; largest error from the linear-light composition {worst}")
print(worst_line)
if worst > 3:
    print(f"{colour}: the mapped shadow is not the linear-light composition of the same coverage")
    sys.exit(1)
PY
}

phase "#000000FF" 1.0
phase "#00000080" 0.5019607843

echo "window shadows compose in linear light on a mapped output"
