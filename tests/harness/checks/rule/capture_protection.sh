#!/usr/bin/env bash
# Inherited rules, retained overview/close images, and a rule which revokes
# its own new window-capture session must all preserve the exclusion boundary.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/protection.png"
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
corner_radius = 0
[appearance.shadow]
enabled = false
[overview]
background_blur = false
[[window_rule]]
match.title = "^transient-parent$"
block_capture = true
[[window_rule]]
match.title = "^transient-child$"
block_capture = false
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
frozen=false
assert_hidden() {
  if [[ $frozen == true ]]; then
    "$UMBRIEL" clock-advance 1 > /dev/null
  else
    "$UMBRIEL" settle > /dev/null
  fi
  capture_assert_hidden "$IMAGE"
}
TRANSIENT_SUITE=mapped-together FILL_COLOR=0xFFFF00FF "$UMBRIEL_UNMAP_CLIENT" transient-child 400 300 > "$UMBRIEL_RUNTIME_DIR/transient.log" 2>&1 &
await_window transient-child
parent=$(window_by_title transient-parent | jq -r .id)
child=$(window_by_title transient-child | jq -r .id)
assert_hidden
"$UMBRIEL_CAPTURE_CLIENT" --window "$child" > "$UMBRIEL_RUNTIME_DIR/denied-child.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/denied-child.log" 'session-stopped ' 1
if rg '^frame |protocol error' "$UMBRIEL_RUNTIME_DIR/denied-child.log"; then exit 1; fi
# A card follows its window's protection while the overview stays open. A
# rule reload closes the overview, so the toggle comes from a title change.
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[[window_rule]]
match.title = "^card-protected$"
block_capture = true
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
mkfifo "$UMBRIEL_RUNTIME_DIR/card"
exec 7<> "$UMBRIEL_RUNTIME_DIR/card"
TITLE_AFTER_MAP=card-protected FILL_COLOR=0xFF00FF00 \
  "$UMBRIEL_UNMAP_CLIENT" card-open 400 300 <&7 > "$UMBRIEL_RUNTIME_DIR/card.log" 2>&1 &
await_window card-open
# An unprotected witness proves the overview is still open after the toggle.
FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" witness 400 300 > "$UMBRIEL_RUNTIME_DIR/witness.log" 2>&1 &
await_window witness
zoomed() {
  local w
  read -r _ _ w _ < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox "$1")
  ((w > 0 && w < 400)) || { echo "$2 not shown at overview zoom: $w"; return 1; }
}
"$UMBRIEL" msg overview-open > /dev/null
# The transient cards were created while protected.
assert_hidden
zoomed 'g>0.9&&r<0.1&&b<0.1' card-open
printf t >&7
await_lines "$UMBRIEL_RUNTIME_DIR/card.log" title-updated 1
assert_hidden
zoomed 'b>0.9&&r<0.1&&g<0.1' witness
green=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'g>0.9&&r<0.1&&b<0.1')
((green == 0)) || { echo "card kept capture after its window became protected: $green"; exit 1; }
"$UMBRIEL" msg overview-close > /dev/null
assert_hidden
# A close retains client images after unmap. Keep the transition at its start.
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation.windows_out]
enabled = true
style = "fade"
duration_ms = 5000
CONFIG
sed -i '0,/enabled = false/s//enabled = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" clock-freeze > /dev/null
frozen=true
"$UMBRIEL" msg window-close "$child" > /dev/null
"$UMBRIEL" msg window-close "$parent" > /dev/null
assert_hidden
"$UMBRIEL" clock-advance 6000 > /dev/null
"$UMBRIEL" clock-resume > /dev/null
frozen=false
# is_cast_target is evaluated inside the new-session callback. Revocation must
# reject copies immediately without destroying that callback's source in place.
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[[window_rule]]
match.title = "^dynamic$"
match.is_cast_target = true
block_capture = true
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL_UNMAP_CLIENT" dynamic > "$UMBRIEL_RUNTIME_DIR/dynamic.log" 2>&1 &
await_window dynamic
id=$(window_by_title dynamic | jq -r .id)
"$UMBRIEL_CAPTURE_CLIENT" --window "$id" > "$UMBRIEL_RUNTIME_DIR/dynamic-capture.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/dynamic-capture.log" 'session-stopped ' 1
if rg '^frame |protocol error' "$UMBRIEL_RUNTIME_DIR/dynamic-capture.log"; then exit 1; fi
echo 'inherited protection, retained images, and reentrant session revocation verified'
