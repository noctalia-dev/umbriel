#!/usr/bin/env bash
# harness: outputs=2
# A pad with hide_when_empty stays open while its first launch starts and when one of several members closes. Losing
# its last member hides it, so the next toggle relaunches in one press; a hidden pad stays hidden, and a pad emptied
# on another output leaves focus where it was. Members leave by unmapping or by exiting.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="$UMBRIEL_UNMAP_CLIENT"
readonly POINTER="$UMBRIEL_POINTER_CLIENT"
readonly RELEASE="$UMBRIEL_RUNTIME_DIR/launch-release"

cat >> "$UMBRIEL_CONFIG" <<EOF

[animation]
enabled = false

[animation.scratchpad]
dim = 0.5
blur = false

[colors]
backdrop = "#FFFFFFFF"

[input.focus]
follows_mouse = false

[output.HEADLESS-1]
mode = "1280x720"
position = [0, 0]
workspaces = ["LEFT"]

[output.HEADLESS-2]
mode = "1280x720"
position = [1280, 0]
workspaces = ["RIGHT"]

[[scratchpad]]
name = "term"
hide_when_empty = true
spawn_when_empty = "while [ ! -e '$RELEASE' ]; do sleep 0.05; done; exec '$CLIENT' term-launch 300 200 >> '$UMBRIEL_RUNTIME_DIR/term-launch.log' 2>&1"
EOF
"$UMBRIEL" msg config-reload > /dev/null

action() { "$UMBRIEL" msg "$@" > /dev/null; }
field() { window_by_title "$1" | jq -r ".$2"; }

# Mean red of the backdrop on HEADLESS-1 or HEADLESS-2: white while the pad is hidden, dimmed to half while shown.
assert_pad() {
  local x=5 red image="$UMBRIEL_RUNTIME_DIR/pad.png"
  [[ $1 == right ]] && x=1285
  "$UMBRIEL" settle > /dev/null
  grim "$image"
  read -r red _ _ <<< "$("$UMBRIEL_PIXEL_PROBE" "$image" mean "10x10+$x+5")"
  if [[ $2 == shown ]] && ((red >= 120 && red <= 135)); then return 0; fi
  if [[ $2 == hidden ]] && ((red >= 250)); then return 0; fi
  echo "expected the $1 pad $2, backdrop red $red: $("$UMBRIEL" windows --json)"
  return 1
}

"$POINTER" 2560 720 move 640 360
"$CLIENT" home 300 200 > "$UMBRIEL_RUNTIME_DIR/home.log" 2>&1 &
await_window home

# Explicitly opening the empty pad keeps it open while its application starts.
action scratchpad-toggle term
assert_pad left shown
touch "$RELEASE"
await_window term-launch
[[ $(field term-launch visible) == true ]] || { echo "launch arrived hidden: $(window_by_title term-launch)"; exit 1; }

# Closing one of several members keeps the pad open.
action spawn "exec '$CLIENT' term-extra 300 200 > '$UMBRIEL_RUNTIME_DIR/term-extra.log' 2>&1"
await_window term-extra
[[ $(field term-extra scratchpad) == term ]] || { echo "extra did not join: $(window_by_title term-extra)"; exit 1; }
action window-close "$(field term-extra id)"
await_window_closed term-extra
assert_pad left shown

# Closing the focused last member hides the pad and hands focus back to the workspace.
action window-focus "$(field term-launch id)"
[[ $(field term-launch active) == true ]] || { echo "could not focus the launch: $(window_by_title term-launch)"; exit 1; }
action window-close "$(field term-launch id)"
await_window_closed term-launch
assert_pad left hidden
[[ $(field home active) == true ]] || { echo "workspace window lost focus: $(window_by_title home)"; exit 1; }

# One toggle shows the pad and relaunches.
action scratchpad-toggle term
await_window term-launch
[[ $(field term-launch visible) == true ]] || { echo "relaunch arrived hidden: $(window_by_title term-launch)"; exit 1; }

# A pad hidden when its last member exits stays hidden.
action scratchpad-toggle term
assert_pad left hidden
kill "$(field term-launch pid)"
await_window_closed term-launch
assert_pad left hidden

# Emptying a pad on another output hides it and leaves focus on this one.
"$POINTER" 2560 720 move 1920 360
action scratchpad-toggle term
await_window term-launch
action window-focus "$(field home id)"
[[ $(field home active) == true ]] || { echo "could not focus home: $(window_by_title home)"; exit 1; }
action window-close "$(field term-launch id)"
await_window_closed term-launch
assert_pad right hidden
[[ $(field home active) == true ]] || { echo "closing the other output's pad moved focus: $(window_by_title home)"; exit 1; }

echo "hide_when_empty keeps a starting or partly emptied pad open and hides an emptied one"
