#!/usr/bin/env bash
# A minimizing floating window stays on screen for `animation.windows_minimize`, shrinking and fading toward its
# anchor, and a restore grows it back. Animation time is clock-advanced so every sample is deterministic.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly BEFORE="$UMBRIEL_RUNTIME_DIR/minimize-anim-before.png"
readonly HIDING="$UMBRIEL_RUNTIME_DIR/minimize-anim-hiding.png"
readonly HIDDEN="$UMBRIEL_RUNTIME_DIR/minimize-anim-hidden.png"
readonly REVEALING="$UMBRIEL_RUNTIME_DIR/minimize-anim-revealing.png"
readonly REVEALED="$UMBRIEL_RUNTIME_DIR/minimize-anim-revealed.png"
readonly INSTANT="$UMBRIEL_RUNTIME_DIR/minimize-anim-instant.png"
# Faint enough to still count a quarter-faded window (the reveal starts from zero alpha), dark enough to ignore the
# black backdrop.
readonly RED='r>0.1&&g<0.08&&b<0.08'

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" '[.[] | select(.title == $title)][0][$field]'
}

red_count() { "$UMBRIEL_PIXEL_PROBE" "$1" count "$RED" | awk '{ print $1 }'; }

# The bounding box of the window's pixels, so a shrink is measured as geometry rather than brightness.
red_width() { "$UMBRIEL_PIXEL_PROBE" "$1" bbox "$RED" | awk '{ print $3 }'; }

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "stacking"

[animation]
enabled = true
duration_ms = 2000

[animation.windows_in]
enabled = false

[animation.windows_minimize]
enabled = true
duration_ms = 2000
curve = "linear"
style = "popin"
scale = 0.5
anchor = "center"

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.title_bar]
mode = "off"

[appearance.shadow]
enabled = false

[appearance.blur]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

OPAQUE_BUFFER=1 FILL_COLOR=0xFFFF0000 "$CLIENT" minimize-anim 600 400 > "$UMBRIEL_RUNTIME_DIR/minimize-anim-client.log" 2>&1 &
for _ in $(seq 80); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.1
done
if [[ $("$UMBRIEL" windows --json | jq 'length') -ne 1 ]]; then
  echo "the minimize client never mapped: $("$UMBRIEL" windows --json)"
  exit 1
fi
WINDOW=$(field_of minimize-anim id)
"$UMBRIEL" msg window-focus "$WINDOW" > /dev/null
"$UMBRIEL" settle

"$UMBRIEL" clock-freeze
"$UMBRIEL" clock-advance 2000
grim "$BEFORE"
before_width=$(red_width "$BEFORE")
before_count=$(red_count "$BEFORE")
if (( before_width < 300 || before_count < 100000 )); then
  echo "the window was not drawn before minimizing: width=$before_width count=$before_count"
  exit 1
fi

# A quarter of the way out the window is smaller but still on screen; the same instant of the reveal shows the mirrored
# box, which is the whole point of animating the live view instead of hiding it in one frame.
"$UMBRIEL" msg window-minimize "$WINDOW" > /dev/null
"$UMBRIEL" clock-advance 500
grim "$HIDING"
hiding_width=$(red_width "$HIDING")
hiding_count=$(red_count "$HIDING")
if (( hiding_count == 0 )); then
  echo "the window vanished instead of animating out: count=0"
  exit 1
fi
if (( hiding_width >= before_width )); then
  echo "the window did not shrink while hiding: before=$before_width hiding=$hiding_width"
  exit 1
fi
# Linear 1.0 -> 0.5 at a quarter of the timeline is 0.875 of the resting width.
expected_hiding=$(( before_width * 7 / 8 ))
if (( hiding_width < expected_hiding - 20 || hiding_width > expected_hiding + 20 )); then
  echo "the hiding box does not track the configured scale: expected ~$expected_hiding, got $hiding_width"
  exit 1
fi

# State is reported at once even though the picture is still fading.
if [[ $(field_of minimize-anim minimized) != true || $(field_of minimize-anim visible) != false ]]; then
  echo "minimize did not report its state immediately: $("$UMBRIEL" windows --json)"
  exit 1
fi

"$UMBRIEL" clock-advance 2000
grim "$HIDDEN"
if (( $(red_count "$HIDDEN") > 10 )); then
  echo "the window stayed on screen after its conceal timeline: count=$(red_count "$HIDDEN")"
  exit 1
fi

# The restore plays the same timeline backwards, from the collapsed box.
"$UMBRIEL" msg window-restore-minimized > /dev/null
"$UMBRIEL" clock-advance 500
grim "$REVEALING"
revealing_width=$(red_width "$REVEALING")
if (( revealing_width == 0 )); then
  echo "the window appeared only after the whole reveal: mid-reveal frame is empty"
  exit 1
fi
if (( revealing_width >= before_width )); then
  echo "the window did not grow back over the reveal: before=$before_width revealing=$revealing_width"
  exit 1
fi

"$UMBRIEL" clock-advance 2000
grim "$REVEALED"
revealed_width=$(red_width "$REVEALED")
if (( revealed_width < before_width - 20 || revealed_width > before_width + 20 )); then
  echo "the revealed window did not return to its resting box: before=$before_width revealed=$revealed_width"
  exit 1
fi
if [[ $(field_of minimize-anim minimized) != false || $(field_of minimize-anim visible) != true ]]; then
  echo "restore did not report its state: $("$UMBRIEL" windows --json)"
  exit 1
fi

# Disabling the block restores the plain one-frame hide, which is the switch users get.
# Flip the block's own `enabled` rather than adding a second key to the table.
sed -i '/^\[animation\.windows_minimize\]$/,/^\[/ s/^enabled = true$/enabled = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg window-minimize "$WINDOW" > /dev/null
"$UMBRIEL" clock-advance 1
grim "$INSTANT"
if (( $(red_count "$INSTANT") > 10 )); then
  echo "a disabled windows_minimize still drew the window mid-hide: count=$(red_count "$INSTANT")"
  exit 1
fi

echo "minimize collapsed from $before_width to $hiding_width px and was gone at the end; reveal grew it back to $revealed_width px"
