#!/usr/bin/env bash
# A minimizing floating window stays on screen for `animation.windows_minimize`, shrinking and fading toward its
# anchor, and a restore grows it back. Animation time is clock-advanced so every sample is deterministic.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly FOREIGN_TOPLEVEL="${UMBRIEL_FOREIGN_TOPLEVEL_CLIENT:-./build-debug/tests/foreign-toplevel-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly BTN_LEFT=272 # evdev BTN_LEFT
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

green_count() { "$UMBRIEL_PIXEL_PROBE" "$1" count 'g>0.3&&r<0.1&&b<0.1' | awk '{ print $1 }'; }

# The opening program must never drive a minimize fade: green means a windows_in slot ran.
expect_no_lifecycle_shader() {
  local green
  green=$(green_count "$1")
  if (( green > 10 )); then
    echo "$2: a lifecycle effect program ran during a minimize transition (green=$green)"
    exit 1
  fi
}

# The bounding box of the window's pixels, so a shrink is measured as geometry rather than brightness.
red_width() { "$UMBRIEL_PIXEL_PROBE" "$1" bbox "$RED" | awk '{ print $3 }'; }

# The whole pixel box "x y w h", so a reveal that lands off its resting position is caught as geometry.
red_bbox() { "$UMBRIEL_PIXEL_PROBE" "$1" bbox "$RED"; }

# The headless backend has no pointer, so clicks go through zwlr_virtual_pointer_v1 on the real input path.
pointer() { "$POINTER" 1280 720 "$@"; }

# The opening program paints the whole window green. A minimize fade must not borrow it in either direction, so any
# green at a mid-flight sample means the lifecycle slot leaked into the minimize timeline.
cat > "$UMBRIEL_RUNTIME_DIR/minimize-anim-green.glsl" <<'GLSL'
vec4 animation(vec2 uv) {
    return vec4(0.0, 1.0, 0.0, 1.0);
}
GLSL

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "stacking"

[animation]
enabled = true
duration_ms = 2000

[effects.preset.minimize_anim_green]
kind = "animation"
shader = "minimize-anim-green.glsl"

[animation.windows_in]
enabled = true
duration_ms = 2000
curve = "linear"
effect = "minimize_anim_green"

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
read -r before_x before_y before_bw before_bh <<< "$(red_bbox "$BEFORE")"
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
expect_no_lifecycle_shader "$HIDING" "hiding"
# The plain alpha is what fades here, so the shadow-free border and the content leave together.
if (( hiding_count >= before_count )); then
  echo "the window did not fade while hiding: before=$before_count hiding=$hiding_count"
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
expect_no_lifecycle_shader "$REVEALING" "revealing"
revealing_width=$(red_width "$REVEALING")
if (( revealing_width == 0 )); then
  echo "the window appeared only after the whole reveal: mid-reveal frame is empty"
  exit 1
fi
if (( revealing_width >= before_width )); then
  echo "the window did not grow back over the reveal: before=$before_width revealing=$revealing_width"
  exit 1
fi
# The reveal is the conceal reversed, so the same instant of a 300 -> 600 px tween is 375 px.
expected_revealing=$(( before_width * 5 / 8 ))
if (( revealing_width < expected_revealing - 20 || revealing_width > expected_revealing + 20 )); then
  echo "the reveal is not the conceal reversed: expected ~$expected_revealing, got $revealing_width"
  exit 1
fi
expect_no_lifecycle_shader "$REVEALING" "revealing"
expect_no_lifecycle_shader "$BEFORE" "resting"


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
expect_no_lifecycle_shader "$REVEALED" "revealed"


# `windows_in.style = "slide"` brightens a lifecycle fade with sqrt(). A minimize fade follows its own curve, so with
# the same 25 % instant the window must sit at the linear 0.75 alpha, not at 0.87.
sed -i 's/^effect = "minimize_anim_green"$//' "$UMBRIEL_CONFIG"
sed -i '/^\[animation\.windows_in\]$/,/^\[/ s/^curve = "linear"$/curve = "linear"\nstyle = "slide"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg window-minimize "$WINDOW" > /dev/null
"$UMBRIEL" clock-advance 500
grim "$HIDING"
bright=$("$UMBRIEL_PIXEL_PROBE" "$HIDING" count 'r>0.85&&g<0.1&&b<0.1' | awk '{ print $1 }')
mid=$("$UMBRIEL_PIXEL_PROBE" "$HIDING" count 'r>0.6&&r<0.85&&g<0.1&&b<0.1' | awk '{ print $1 }')
if (( bright > 1000 || mid == 0 )); then
  echo "a slide opening style warped the minimize opacity: bright=$bright mid=$mid"
  exit 1
fi
"$UMBRIEL" clock-advance 2000
"$UMBRIEL" msg window-restore-minimized > /dev/null
"$UMBRIEL" clock-advance 2000

set_minimize_enabled() {
  sed -i "/^\[animation\.windows_minimize\]$/,/^\[/ s/^enabled = .*/enabled = $1/" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}

# An animated restore that lands mid-conceal must end at the resting box. The conceal has already retargeted the live
# box to its collapsed origin, so a reveal that reads its destination from the live target grows back off-centre and
# small. The taskbar path shows the window without focusing it, so no arrange follows to repair the geometry.
"$UMBRIEL" msg window-minimize "$WINDOW" > /dev/null
"$UMBRIEL" clock-advance 500
"$FOREIGN_TOPLEVEL" minimize-anim "$(field_of minimize-anim output)" unminimize > /dev/null
"$UMBRIEL" clock-advance 2000
grim "$REVEALED"
read -r restore_x restore_y restore_w restore_h <<< "$(red_bbox "$REVEALED")"
if (( restore_w == 0 )); then
  echo "a restore during a conceal left the window hidden: bbox=$(red_bbox "$REVEALED")"
  exit 1
fi
if (( restore_x < before_x - 4 || restore_x > before_x + 4 || restore_y < before_y - 4 || restore_y > before_y + 4
      || restore_w < before_bw - 4 || restore_w > before_bw + 4 || restore_h < before_bh - 4 || restore_h > before_bh + 4 ))
then
  echo "a restore during a conceal did not return to the resting box: before=[$before_x $before_y $before_bw $before_bh]" \
    "after=[$restore_x $restore_y $restore_w $restore_h]"
  exit 1
fi
if [[ $(field_of minimize-anim minimized) != false ]]; then
  echo "an animated restore during a conceal did not report the window restored: $("$UMBRIEL" windows --json)"
  exit 1
fi

# The conceal leaves the node enabled so the window can be seen leaving, but it is already minimized: a click on the
# fading remnant must fall through instead of focusing and unminimizing the window underneath it.
pointer move 0 0
"$UMBRIEL" msg window-minimize "$WINDOW" > /dev/null
"$UMBRIEL" clock-advance 500
pointer move "$(( before_x + before_bw / 2 ))" "$(( before_y + before_bh / 2 ))"
pointer click "$BTN_LEFT"
"$UMBRIEL" clock-advance 2000
grim "$REVEALED"
if [[ $(field_of minimize-anim minimized) != true ]]; then
  echo "a click during a conceal unminimized the fading window: $("$UMBRIEL" windows --json)"
  exit 1
fi
if [[ $(field_of minimize-anim focused) != false ]]; then
  echo "a click during a conceal focused the fading window: $("$UMBRIEL" windows --json)"
  exit 1
fi
if (( $(red_count "$REVEALED") > 10 )); then
  echo "a click during a conceal kept the window on screen: count=$(red_count "$REVEALED")"
  exit 1
fi
pointer move 0 0
"$UMBRIEL" msg window-restore-minimized > /dev/null
"$UMBRIEL" clock-advance 2000

# A restore that will not animate (the block is off) while a conceal is still fading must cancel the conceal. A
# taskbar unminimize shows the window without focusing it, so no arrange follows to paper over a finish that hides a
# window which is no longer minimized.
"$UMBRIEL" msg window-minimize "$WINDOW" > /dev/null
"$UMBRIEL" clock-advance 300
set_minimize_enabled false
"$FOREIGN_TOPLEVEL" minimize-anim "$(field_of minimize-anim output)" unminimize > /dev/null
"$UMBRIEL" clock-advance 2000
grim "$REVEALED"
if (( $(red_count "$REVEALED") == 0 )); then
  echo "an unminimize during a conceal left the window hidden: count=$(red_count "$REVEALED")"
  exit 1
fi
if [[ $(field_of minimize-anim minimized) != false ]]; then
  echo "an unminimize during a conceal did not report the window restored: $("$UMBRIEL" windows --json)"
  exit 1
fi

# The same sequence through the focus path, which does arrange afterwards.
"$UMBRIEL" msg window-minimize "$WINDOW" > /dev/null
"$UMBRIEL" clock-advance 2000
set_minimize_enabled true
"$UMBRIEL" msg window-restore-minimized > /dev/null
"$UMBRIEL" clock-advance 2000
"$UMBRIEL" msg window-minimize "$WINDOW" > /dev/null
"$UMBRIEL" clock-advance 300
set_minimize_enabled false
"$UMBRIEL" msg window-restore-minimized > /dev/null
"$UMBRIEL" clock-advance 2000
grim "$REVEALED"
if (( $(red_count "$REVEALED") == 0 )); then
  echo "a restore during a conceal left the window hidden: count=$(red_count "$REVEALED")"
  exit 1
fi
if [[ $(field_of minimize-anim minimized) != false ]]; then
  echo "a restore during a conceal did not report the window restored: $("$UMBRIEL" windows --json)"
  exit 1
fi

# The block is still off, so the hide is the plain one-frame switch users get.
"$UMBRIEL" msg window-minimize "$WINDOW" > /dev/null
"$UMBRIEL" clock-advance 1
grim "$INSTANT"
if (( $(red_count "$INSTANT") > 10 )); then
  echo "a disabled windows_minimize still drew the window mid-hide: count=$(red_count "$INSTANT")"
  exit 1
fi

set_minimize_enabled true
"$UMBRIEL" msg window-restore-minimized > /dev/null
"$UMBRIEL" clock-advance 2000

# A minimize collapse is geometry and plain alpha: a `windows_move` preset must not warp the picture on top of it.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation.windows_move]
enabled = true
duration_ms = 2000
effect = "minimize_anim_green"
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg window-minimize "$WINDOW" > /dev/null
"$UMBRIEL" clock-advance 500
grim "$HIDING"
expect_no_lifecycle_shader "$HIDING" "collapsing with a windows_move preset"
"$UMBRIEL" clock-advance 2000
"$UMBRIEL" msg window-restore-minimized > /dev/null
"$UMBRIEL" clock-advance 500
grim "$REVEALING"
expect_no_lifecycle_shader "$REVEALING" "revealing with a windows_move preset"
"$UMBRIEL" clock-advance 2000
sed -i '/^\[animation\.windows_move\]$/,/^effect = "minimize_anim_green"$/d' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

# A style reloaded to `fade` mid-conceal must not skip the tweens back: the conceal already moved the box, so the
# restore still has to land on the resting box instead of the collapsed origin.
"$UMBRIEL" msg window-minimize "$WINDOW" > /dev/null
"$UMBRIEL" clock-advance 500
sed -i '/^\[animation\.windows_minimize\]$/,/^\[/ s/^style = "popin"$/style = "fade"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg window-restore-minimized > /dev/null
"$UMBRIEL" clock-advance 2000
grim "$REVEALED"
read -r faded_x faded_y faded_w faded_h <<< "$(red_bbox "$REVEALED")"
if (( faded_w == 0 || faded_x < before_x - 4 || faded_x > before_x + 4 || faded_y < before_y - 4
      || faded_y > before_y + 4 || faded_w < before_bw - 4 || faded_w > before_bw + 4 )); then
  echo "a style reloaded to fade mid-conceal moved the restored box off its resting position:" \
    "before=[$before_x $before_y $before_bw $before_bh] after=[$faded_x $faded_y $faded_w $faded_h]"
  exit 1
fi
sed -i '/^\[animation\.windows_minimize\]$/,/^\[/ s/^style = "fade"$/style = "popin"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

# The decorated frame is enabled for the fade too, so its title bar and resize border must refuse the same input as the
# content: a press there would focus the view, and focusing a minimized view restores it. The check above runs
# undecorated so its pixel maths stays simple; this leg turns the frame back on for the hit test alone.
sed -i 's/^border_width = 0$/border_width = 2/' "$UMBRIEL_CONFIG"
sed -i '/^\[appearance\.title_bar\]$/,/^\[/ s/^mode = "off"$/mode = "on"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" clock-advance 2000
read -r box_x box_y box_w box_h <<< "$("$UMBRIEL" windows --json \
  | jq -r '.[] | select(.title == "minimize-anim") | "\(.x) \(.y) \(.w) \(.h)"')"
"$UMBRIEL" msg window-minimize "$WINDOW" > /dev/null
"$UMBRIEL" clock-advance 500
# The live box is 7/8 of the resting one at this instant and centred on it, so its top edge sits box_h/16 below; the
# decorated frame's title bar is drawn above that edge and does not shrink with the presented box.
pointer move 0 0
pointer move "$(( box_x + box_w / 2 ))" "$(( box_y + box_h / 16 - 8 ))"
pointer click "$BTN_LEFT"
"$UMBRIEL" clock-advance 2000
pointer move 0 0
if [[ $(field_of minimize-anim minimized) != true ]]; then
  echo "a press on the fading title bar unminimized the window: $("$UMBRIEL" windows --json)"
  exit 1
fi
if [[ $(field_of minimize-anim focused) != false ]]; then
  echo "a press on the fading title bar focused the window: $("$UMBRIEL" windows --json)"
  exit 1
fi
if [[ $("$UMBRIEL" windows --json | jq 'length') -ne 1 ]]; then
  echo "a press on the fading title bar closed the window: $("$UMBRIEL" windows --json)"
  exit 1
fi
if [[ $(field_of minimize-anim x) != "$box_x" || $(field_of minimize-anim y) != "$box_y" ]]; then
  echo "a press on the fading title bar moved the window: was [$box_x $box_y], now" \
    "[$(field_of minimize-anim x) $(field_of minimize-anim y)]"
  exit 1
fi

echo "minimize collapsed from $before_width to $hiding_width px and was gone at the end; reveal grew it back to $revealed_width px"
