#!/usr/bin/env bash
# harness: outputs=2
# Opted-in scratchpads resize real client buffers across outputs, using manual
# adjustments as the next size basis. Leaving the pad releases that policy.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="$UMBRIEL_UNMAP_CLIENT"
readonly POINTER="$UMBRIEL_POINTER_CLIENT"
readonly TITLE=proportional-member

capture_config
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
gap = 0

[input.focus]
follows_mouse = false

[input.cursor]
follows_focus = false

[output.HEADLESS-1]
mode = "1280x720"
scale = 1.0
position = [0, 0]
workspaces = ["LEFT", "OTHER"]

[output.HEADLESS-2]
mode = "1920x1080"
position = [1280, 0]
workspaces = ["RIGHT"]

[[scratchpad]]
name = "scaled"
proportional_size = true

[[scratchpad]]
name = "fixed"

[[window_rule]]
match.title = "^proportional-"
default_output = "HEADLESS-1"
default_floating = true
default_floating_size = { width = 0.5, height = 0.5 }
EOF
"$UMBRIEL" msg config-reload > /dev/null

action() { "$UMBRIEL" msg "$@" > /dev/null; }

assert_size() {
  "$UMBRIEL" settle
  local actual
  actual=$(window_by_title "$1" | jq -r '"\(.w)x\(.h)"')
  [[ $actual == "$2" ]] || { echo "$1: expected $2, got $actual"; return 1; }
}

assert_state() {
  "$UMBRIEL" settle
  window_by_title "$TITLE" | jq -e "$1" > /dev/null || {
    echo "expected $1: $(window_by_title "$TITLE")"; return 1;
  }
}

pointer_to() {
  local x=640
  [[ $1 == right ]] && x=1920
  "$POINTER" 3200 1080 move "$x" 360
}

summon_on() {
  pointer_to "$1"
  action scratchpad-toggle "${2:-scaled}"
}

pointer_to left
RESIZE_FILL_COLOR=0xFFFF0000 "$CLIENT" "$TITLE" 640 360 > "$UMBRIEL_RUNTIME_DIR/member.log" 2>&1 &
await_window "$TITLE"
assert_size "$TITLE" 640x360
member=$(window_by_title "$TITLE" | jq -r .id)
action window-focus "$member"
action window-move-to-scratchpad scaled
summon_on left
assert_size "$TITLE" 640x360
summon_on right
assert_size "$TITLE" 960x540

# Verify the new size is drawn, beyond the IPC layout target.
grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/scaled.png"
read -r _ _ width height < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/scaled.png" bbox 'r>0.9&&g<0.1&&b<0.1')
[[ $width == 960 && $height == 540 ]] || { echo "scaled pixels were ${width}x${height}"; exit 1; }

# Dropping a dragged member on another output resizes it without remapping its
# pointer-selected drop position as though it had been summoned.
read -r center_x center_y < <(window_by_title "$TITLE" | jq -r '[.x + .w / 2, .y + .h / 2] | @tsv')
"$POINTER" 3200 1080 move "$center_x" "$center_y" mod logo press 272 move 640 360 release 272 mod none
assert_size "$TITLE" 640x360
assert_state '.scratchpad == "scaled" and .x < 1280'
summon_on right
assert_size "$TITLE" 960x540

action window-set-primary-extent 0.25
action window-set-secondary-extent 0.5
assert_size "$TITLE" 480x540
summon_on left
assert_size "$TITLE" 320x360

# Movement, hiding, and a live scale change on the same output keep the size.
action window-move-by 20 10
action scratchpad-toggle scaled
action scratchpad-toggle scaled
assert_size "$TITLE" 320x360
sed -i 's/scale = 1.0/scale = 1.25/' "$UMBRIEL_CONFIG"
action config-reload
assert_size "$TITLE" 320x360
summon_on right
assert_size "$TITLE" 600x675

# Maximized and fullscreen presentations retain their state; their normal
# restore sizes follow the same monitor transfer policy.
action window-set-primary-extent 0.25
action window-set-secondary-extent 0.25
assert_size "$TITLE" 480x270
action window-toggle-maximize-to-edges
assert_size "$TITLE" 1920x1080
summon_on left
assert_size "$TITLE" 1024x576
assert_state '.maximized_to_edges == true'
action window-toggle-maximize-to-edges
assert_size "$TITLE" 256x144
action window-toggle-fullscreen
assert_size "$TITLE" 1024x576
summon_on right
assert_size "$TITLE" 1920x1080
action window-toggle-fullscreen
assert_size "$TITLE" 480x270

# Restore returns a floating member to its saved workspace without resizing it.
action window-restore-from-scratchpad scaled
assert_state '.scratchpad == "" and .floating and (.workspace | startswith("HEADLESS-1:"))'
assert_size "$TITLE" 480x270

# Explicit workspace moves likewise retain a resized float's size and release
# membership, even when the destination is on another output.
pointer_to left
action window-move-to-scratchpad scaled
action window-focus "$member"
action window-set-primary-extent 0.5
action window-set-secondary-extent 0.5
assert_size "$TITLE" 512x288
action window-move-to-workspace RIGHT --output HEADLESS-2
assert_state '.scratchpad == "" and .floating and (.workspace | startswith("HEADLESS-2:"))'
assert_size "$TITLE" 512x288

# A member originally tiled returns tiled; floating it later recovers the size
# the user chose in the scratchpad.
pointer_to right
action window-toggle-floating
assert_state '.floating == false'
action window-move-to-scratchpad scaled
action window-focus "$member"
action window-set-primary-extent 0.25
action window-set-secondary-extent 0.25
assert_size "$TITLE" 480x270
action window-move-to-workspace OTHER --output HEADLESS-1
assert_state '.scratchpad == "" and .floating == false'
action window-toggle-floating
assert_size "$TITLE" 480x270

# A scratchpad with the option omitted retains its logical-pixel size.
pointer_to left
action window-move-to-scratchpad fixed
action window-focus "$member"
assert_size "$TITLE" 480x270
summon_on right fixed
assert_size "$TITLE" 480x270

# Client size limits also constrain opted-in transfers.
pointer_to left
FIXED_SIZE=1 "$UMBRIEL_SUBSURFACE_CLIENT" proportional-capped 400 200 > "$UMBRIEL_RUNTIME_DIR/capped.log" 2>&1 &
await_window proportional-capped
assert_size proportional-capped 400x200
action window-focus "$(window_by_title proportional-capped | jq -r .id)"
action window-move-to-scratchpad scaled
action window-focus "$(window_by_title proportional-capped | jq -r .id)"
summon_on right
assert_size proportional-capped 400x200

echo "scratchpad monitor transfers preserve manual size proportions and workspace restore behavior"
