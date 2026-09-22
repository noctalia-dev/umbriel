#!/usr/bin/env bash
# harness: outputs=2
# Carrying the pointer onto another output is follows_mouse focus too, so follows_mouse_reveals governs it. Crossing
# heads goes through refocusExplicit, not a hover enter, and that fallback used to reveal unconditionally: entering an
# output whose remembered focus had been scrolled out of view yanked its strip back. The fallback still has to land
# focus on the output being entered, so this asserts focus arrives AND the strip stays where it was left.
set -euo pipefail

readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}"
readonly BASELINE="$(< "$UMBRIEL_CONFIG")"
# Two 1280x720 outputs side by side, so pointer coordinates are layout-global.
readonly LAYOUT_W=2560
readonly LAYOUT_H=720
readonly OUTPUT_W=1280
readonly RIGHT_X=1280

write_config() {
  {
    printf '%s\n' "$BASELINE"
    printf '\n[animation]\nenabled = false\n'
    printf '\n[layout]\nmode = "scrolling"\n'
    printf '\n[layout.scrolling]\ndefault_extent_fraction = 0.6\ncenter_focused = "never"\n'
    printf '\n[input.cursor]\nfollows_focus = false\n'
    printf '\n[input.focus]\nfollows_mouse = true\nfollows_mouse_reveals = %s\n' "$1"
    printf '\n[output.HEADLESS-1]\nposition = [0, 0]\nworkspaces = ["LEFT"]\n'
    printf '\n[output.HEADLESS-2]\nposition = [%s, 0]\nworkspaces = ["RIGHT"]\n' "$RIGHT_X"
  } > "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}

accepts() {
  if ! out=$("$UMBRIEL" msg "$1" 2>&1); then
    echo "expected '$1' to be accepted, got: $out"
    exit 1
  fi
}

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" \
    '.[] | select(.title == $title) | .[$field]'
}

wait_for_windows() {
  local expected=$1 count=
  for _ in $(seq 40); do
    count=$("$UMBRIEL" windows --json | jq 'length')
    if [[ $count == "$expected" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $expected window(s), got $count"
  exit 1
}

wait_for_workspace() {
  local title=$1 expected=$2 actual=
  for _ in $(seq 50); do
    actual=$(field_of "$title" workspace)
    if [[ $actual == "$expected" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected '$title' on $expected, got $actual"
  exit 1
}

workspace_id_named() {
  "$WORKSPACE" --all | awk -F'\t' -v name="$1" '$2 == name { print $1; exit }'
}

# Boxes of the windows on one workspace only, so the other output's layout cannot
# mask a scroll on the one being entered.
boxes_on() {
  "$UMBRIEL" windows --json | jq -c --arg ws "$1" \
    '[.[] | select(.workspace == $ws) | {title, x, y, w, h}] | sort_by(.title)'
}

settle_on() {
  local ws=$1 a= b=
  a=$(boxes_on "$ws")
  for _ in $(seq 40); do
    sleep 0.05
    b=$(boxes_on "$ws")
    if [[ $a == "$b" ]]; then
      printf '%s' "$a"
      return 0
    fi
    a=$b
  done
  printf '%s' "$a"
}

# The entered output only asks for a reveal when its remembered focus is not
# fully on screen, which is the whole precondition of this check.
assert_not_fully_visible() {
  local title=$1
  if ! "$UMBRIEL" windows --json | jq -e --arg title "$title" --argjson ow "$OUTPUT_W" \
    '.[] | select(.title == $title) | .x < 0 or .x + .w > $ow' > /dev/null; then
    echo "'$title' is fully visible, so entering its output would not reveal anything: $(boxes_on "$2")"
    exit 1
  fi
}

# Exactly one activated window, and on the expected workspace.
wait_for_active_on() {
  local ws=$1 actual=
  for _ in $(seq 60); do
    actual=$("$UMBRIEL" windows --json | jq -r '[.[] | select(.active) | .workspace] | join(",")')
    if [[ $actual == "$ws" ]]; then
      return 0
    fi
    sleep 0.05
  done
  echo "expected exactly one activated window on $ws, got '$actual'"
  exit 1
}

# Leaves the left strip scrolled away from its remembered focus: focus the first
# column, then pan the strip past it without changing what the workspace
# remembers. The pointer stays on the left output throughout, since the scroll
# actions resolve their workspace from it.
derange_left_strip() {
  "$POINTER" "$LAYOUT_W" "$LAYOUT_H" move 640 360
  accepts "window-focus:$(field_of left-a id)"
  for _ in 1 2 3 4 5; do
    accepts layout-scroll-right
  done
}

write_config false

left=$(workspace_id_named LEFT)
right=$(workspace_id_named RIGHT)
if [[ -z $left || -z $right ]]; then
  echo "named workspaces missing: left='$left' right='$right'"
  exit 1
fi

# Overflow the left strip.
accepts "workspace-switch:LEFT/HEADLESS-1"
spawn_client left-a
wait_for_windows 1
spawn_client left-b
wait_for_windows 2
spawn_client left-c
wait_for_windows 3
wait_for_workspace left-c "$left"

# One window on the right output to hold focus before each crossing.
accepts "workspace-switch:RIGHT/HEADLESS-2"
spawn_client right-only
wait_for_windows 4
wait_for_workspace right-only "$right"

derange_left_strip
assert_not_fully_visible left-a "$left"
if [[ $(field_of left-a focused) != true ]]; then
  echo "left-a should still be the left workspace's remembered focus after scrolling away from it"
  exit 1
fi

# Hand focus to the right output, then come back.
"$POINTER" "$LAYOUT_W" "$LAYOUT_H" move $((RIGHT_X + 640)) 360
wait_for_active_on "$right"
before=$(settle_on "$left")

"$POINTER" "$LAYOUT_W" "$LAYOUT_H" move 640 360
wait_for_active_on "$left"
after=$(settle_on "$left")
if [[ $before != "$after" ]]; then
  echo "crossing onto the left output scrolled its strip with follows_mouse_reveals off"
  echo "  before: $before"
  echo "  after:  $after"
  exit 1
fi

# With reveals on, the same crossing is allowed to pull the strip back.
write_config true
derange_left_strip
assert_not_fully_visible left-a "$left"
"$POINTER" "$LAYOUT_W" "$LAYOUT_H" move $((RIGHT_X + 640)) 360
wait_for_active_on "$right"
revealed_before=$(settle_on "$left")

"$POINTER" "$LAYOUT_W" "$LAYOUT_H" move 640 360
wait_for_active_on "$left"
revealed_after=$(settle_on "$left")
if [[ $revealed_before == "$revealed_after" ]]; then
  echo "crossing left the strip untouched with follows_mouse_reveals on, so the key gates nothing here"
  echo "  boxes: $revealed_after"
  exit 1
fi

echo "crossing outputs landed focus on the entered output without scrolling its strip, and still revealed with follows_mouse_reveals on"
