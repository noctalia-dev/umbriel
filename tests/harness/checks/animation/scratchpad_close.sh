#!/usr/bin/env bash
# harness: outputs=2
# A scratchpad member's close snapshot fades at the same rate as a plain floating window's: the pad draws its members
# above its own dim rect, so the snapshot has to live in the pad's tree rather than under the output root. It must also
# follow the pad's own backdrop fade, so hiding the pad mid-close takes the snapshot with it.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly SHOTS="$UMBRIEL_RUNTIME_DIR/scratchpad-close"
readonly CLOSE_MS=5000
mkdir -p "$SHOTS"

# The 40x40 mean at the window's centre: the clients paint 0xFF5577AA, so a fully visible window reads about 170.
sample_blue() {
  "$UMBRIEL_PIXEL_PROBE" "$1" mean 40x40+240+160 | awk '{ print int($3) }'
}

# Inside the 300x200 dialog centred on its 100x100 parent, and outside the parent itself.
sample_dialog() {
  "$UMBRIEL_PIXEL_PROBE" "$1" mean 40x40+170+120 | awk '{ print int($3) }'
}

window_id() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | .id'
}

await_log() {
  local log=$1 needle=$2
  for _ in $(seq 100); do
    grep -q "^$needle\$" "$log" && return 0
    sleep 0.025
  done
  echo "timed out waiting for $needle in $log"
  exit 1
}

spawn() {
  local title=$1
  local log="$UMBRIEL_RUNTIME_DIR/$title.log"
  "$CLIENT" "$title" 400 240 > "$log" 2>&1 &
  await_log "$log" mapped
}

close_window() {
  local title=$1
  "$UMBRIEL" msg window-close "$(window_id "$title")" > /dev/null
  await_log "$UMBRIEL_RUNTIME_DIR/$title.log" unmapped
}

shot() {
  grim "$SHOTS/$1.png"
  sample_blue "$SHOTS/$1.png"
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[animation]
duration_ms = 5000
curve = "linear"

[animation.windows_in]
enabled = false

# Linear over five seconds so one advance lands on a known fraction of the close timeline.
[animation.windows_out]
enabled = true
style = "fade"
duration_ms = 5000
curve = "linear"

[animation.windows_move]
enabled = false

# The default dim, with a fade short enough that finishing it leaves the close timeline running.
[animation.scratchpad]
enabled = true
duration_ms = 1500
curve = "linear"
dim = 0.8

[[window_rule]]
match.title = "^close-"
default_floating = true
default_floating_size_px = { width = 400, height = 240 }
default_position = { x = 40, y = 40, anchor = "top_left" }

# The dialog leg's parent: the child dialog is centred on it and is bigger than it.
[[window_rule]]
match.title = "^transient-parent$"
default_floating = true
default_floating_size_px = { width = 100, height = 100 }
default_position = { x = 40, y = 40, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" clock-freeze

# 2350 ms of the 5000 ms fade leaves 53% opacity, so a snapshot above the dim reads about 90.
readonly MID_ADVANCE=2350

# Control: the same window on a plain workspace, so the pad's numbers are compared against a known-good fade.
spawn close-control
"$UMBRIEL" clock-advance "$CLOSE_MS"
control_before=$(shot control-before)
if ((control_before < 120)); then
  echo "floating control was not visible before its close: blue=$control_before"
  exit 1
fi
close_window close-control
"$UMBRIEL" clock-advance "$MID_ADVANCE"
control_mid=$(shot control-mid)
"$UMBRIEL" clock-advance "$CLOSE_MS"
control_after=$(shot control-after)
if ((control_mid < 70 || control_mid > 160 || control_after > 5)); then
  echo "floating close snapshot did not fade as configured: before=$control_before mid=$control_mid after=$control_after"
  exit 1
fi

# A member of a shown scratchpad must fade exactly like the control. Under the output root the pad's 0.8 dim rect would
# cut the mid-fade sample to a fraction of it.
spawn close-member
"$UMBRIEL" msg window-focus "$(window_id close-member)" > /dev/null
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" clock-advance "$CLOSE_MS"
member_before=$(shot member-before)
if ((member_before < 120)); then
  echo "scratchpad member was not visible before its close: blue=$member_before"
  exit 1
fi
close_window close-member
"$UMBRIEL" clock-advance "$MID_ADVANCE"
member_mid=$(shot member-mid)
"$UMBRIEL" clock-advance "$CLOSE_MS"
member_after=$(shot member-after)
if ((member_mid < control_mid - 20 || member_mid > control_mid + 20)); then
  echo "scratchpad close snapshot was attenuated by the pad: control=$control_mid member=$member_mid"
  exit 1
fi
if ((member_after > 5)); then
  echo "scratchpad close snapshot outlived its timeline: control=$control_after member=$member_after"
  exit 1
fi
# The reaped snapshot must not keep the compositor "animating" on the frozen clock.
"$UMBRIEL" settle

# Leave the pad hidden so the next leg can show it again.
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" clock-advance 1500

# Hiding the pad while the snapshot is still fading must take the snapshot with it: the 1500 ms backdrop fade ends
# while 3400 ms of the close timeline remain.
spawn close-hiding-member
"$UMBRIEL" msg window-focus "$(window_id close-hiding-member)" > /dev/null
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" clock-advance "$CLOSE_MS"
hiding_before=$(shot hiding-before)
if ((hiding_before < 120)); then
  echo "scratchpad member was not visible before the hidden-mid-fade leg: blue=$hiding_before"
  exit 1
fi
close_window close-hiding-member
"$UMBRIEL" clock-advance 100
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" clock-advance 1600
hiding_after=$(shot hiding-after)
if ((hiding_after > 5)); then
  echo "scratchpad close snapshot outlived the pad's fade: blue=$hiding_after"
  exit 1
fi
"$UMBRIEL" clock-advance "$CLOSE_MS"
"$UMBRIEL" settle

# A dialog of a member is attached to its parent, and that link is released while the window closes. It must still be
# captured into the pad.
TRANSIENT_SUITE=mapped-together TRANSIENT_MODAL=1 TRANSIENT_PARENT_SIZE=100x100 \
  "$CLIENT" pad-dialog 300 200 > "$UMBRIEL_RUNTIME_DIR/pad-dialog.log" 2>&1 &
await_log "$UMBRIEL_RUNTIME_DIR/pad-dialog.log" mapped
"$UMBRIEL" clock-advance "$CLOSE_MS"
"$UMBRIEL" msg window-focus "$(window_id transient-parent)" > /dev/null
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" clock-advance "$CLOSE_MS"
grim "$SHOTS/dialog-before.png"
dialog_before=$(sample_dialog "$SHOTS/dialog-before.png")
if ((dialog_before < 120)); then
  echo "scratchpad dialog was not visible before its close: blue=$dialog_before"
  exit 1
fi
close_window pad-dialog
"$UMBRIEL" clock-advance "$MID_ADVANCE"
grim "$SHOTS/dialog-mid.png"
dialog_mid=$(sample_dialog "$SHOTS/dialog-mid.png")
if ((dialog_mid < control_mid - 20 || dialog_mid > control_mid + 20)); then
  echo "scratchpad dialog close snapshot was attenuated by the pad: control=$control_mid dialog=$dialog_mid"
  exit 1
fi
"$UMBRIEL" clock-advance "$CLOSE_MS"
grim "$SHOTS/dialog-after.png"
dialog_after=$(sample_dialog "$SHOTS/dialog-after.png")
if ((dialog_after > 5)); then
  echo "scratchpad dialog close snapshot outlived its timeline: blue=$dialog_after"
  exit 1
fi
"$UMBRIEL" settle

# An unplugged output reaps its snapshots; the pad must drop the wrapper layers in the same pass, or its tracked list
# keeps the compositor permanently "animating".
# The dialog leg left the pad shown, so the member appears without another toggle.
spawn close-hotplug
"$UMBRIEL" msg window-focus "$(window_id close-hotplug)" > /dev/null
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
"$UMBRIEL" clock-advance "$CLOSE_MS"
hotplug_before=$(shot hotplug-before)
if ((hotplug_before < 120)); then
  echo "scratchpad member was not visible before the hot-unplug leg: blue=$hotplug_before"
  exit 1
fi
# Unplug the output the pad is actually on; the other one keeps the instance alive.
hotplug_output=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "close-hotplug") | .output')
if [[ -z $hotplug_output || $hotplug_output == null ]]; then
  echo "hot-unplug leg lost its pad member's output"
  exit 1
fi
close_window close-hotplug
"$UMBRIEL" clock-advance 100
"$UMBRIEL" output-destroy "$hotplug_output" > /dev/null
"$UMBRIEL" clock-advance "$CLOSE_MS"
"$UMBRIEL" settle

echo "scratchpad close snapshot faded like a floating window: control $control_before -> $control_mid -> $control_after, member $member_before -> $member_mid -> $member_after, pad-hide $hiding_before -> $hiding_after, dialog $dialog_before -> $dialog_mid -> $dialog_after, hot-unplug $hotplug_before"
