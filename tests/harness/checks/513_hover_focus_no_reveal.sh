#!/usr/bin/env bash
# With follows_mouse_reveals off, hovering a column that straddles the output edge focuses it where it stands: the
# strip keeps its scroll offset and every window box stays put, which is the decoupling Mod+drag already gets through
# FocusReason::Grab. The same hover with reveals back on has to move the strip, so the key is shown to gate something
# rather than the geometry merely being stable on its own.
set -euo pipefail

readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly BASELINE="$(< "$UMBRIEL_CONFIG")"
readonly OUTPUT_W=1280
readonly OUTPUT_H=720

write_config() {
  {
    printf '%s\n' "$BASELINE"
    printf '\n[animation]\nenabled = false\n'
    printf '\n[layout]\nmode = "scrolling"\n'
    # Three columns at 0.6 of the viewport cannot share it, so the strip overflows
    # and the trailing columns hang off the edge.
    printf '\n[layout.scrolling]\ndefault_extent_fraction = 0.6\ncenter_focused = "never"\n'
    printf '\n[input.cursor]\nfollows_focus = false\n'
    printf '\n[input.focus]\nfollows_mouse = true\nfollows_mouse_reveals = %s\n' "$1"
  } > "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
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

# Every window box, title-ordered, as one comparable string. Any scroll of the
# strip moves all of them, so this is the whole viewport's position.
all_boxes() {
  "$UMBRIEL" windows --json | jq -c '[.[] | {title, x, y, w, h}] | sort_by(.title)'
}

# Boxes once two consecutive reads agree, so the comparison never races a reflow.
settle_boxes() {
  local a= b=
  a=$(all_boxes)
  for _ in $(seq 40); do
    sleep 0.05
    b=$(all_boxes)
    if [[ $a == "$b" ]]; then
      printf '%s' "$a"
      return 0
    fi
    a=$b
  done
  printf '%s' "$a"
}

# Fails unless the window has part of itself on screen and part off it, which is
# the only case where hovering it can ask for a reveal.
assert_straddles_edge() {
  local title=$1
  if ! "$UMBRIEL" windows --json | jq -e --arg title "$title" --argjson ow "$OUTPUT_W" \
    '.[] | select(.title == $title) | (.x < 0 and .x + .w > 0) or (.x < $ow and .x + .w > $ow)' > /dev/null; then
    echo "'$title' does not straddle an output edge, so this check cannot ask for a reveal: $(all_boxes)"
    exit 1
  fi
}

# Centre of the on-screen part of a window.
hover_point() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --argjson ow "$OUTPUT_W" \
    '.[] | select(.title == $title)
     | ([.x, 0] | max) as $l
     | ([(.x + .w), $ow] | min) as $r
     | "\((($l + $r) / 2) | floor) \((.y + (.h / 2)) | floor)"'
}

hover() {
  # shellcheck disable=SC2046
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" move $(hover_point "$1")
}

wait_for_focused() {
  local title=$1 actual=
  for _ in $(seq 60); do
    actual=$(field_of "$title" focused)
    if [[ $actual == true ]]; then
      return 0
    fi
    sleep 0.05
  done
  echo "expected '$title' to be focused, got focused=$actual"
  exit 1
}

write_config false

spawn_client first
wait_for_windows 1
spawn_client second
wait_for_windows 2
spawn_client third
wait_for_windows 3

# The newest column was revealed as it opened, so the strip rests scrolled right
# and 'second' hangs off the left edge.
before=$(settle_boxes)
assert_straddles_edge second

hover second
wait_for_focused second
after=$(settle_boxes)
if [[ $before != "$after" ]]; then
  echo "hover moved the strip with follows_mouse_reveals off"
  echo "  before: $before"
  echo "  after:  $after"
  exit 1
fi

# With reveals on, the same hover must move the strip. Focus 'third' first, since
# a hover onto the already focused window is not an enter and changes nothing.
write_config true
hover third
wait_for_focused third
revealed_before=$(settle_boxes)
assert_straddles_edge second

hover second
wait_for_focused second
revealed_after=$(settle_boxes)
if [[ $revealed_before == "$revealed_after" ]]; then
  echo "hover left the strip untouched with follows_mouse_reveals on, so the key gates nothing"
  echo "  boxes: $revealed_after"
  exit 1
fi

echo "hover focused a column straddling the output edge without scrolling the strip, and still revealed it with follows_mouse_reveals on"
