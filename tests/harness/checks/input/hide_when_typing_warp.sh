#!/usr/bin/env bash
# A follows_focus warp after a keyboard focus action keeps a cursor hidden by typing hidden at the new window, and the
# next real motion shows it there without hover returning focus. A wheel bind shows the cursor before its warp.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly BTN_LEFT=272
readonly KEY_L=38
readonly KEY_A=30
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/hide-typing-warp.png"

if [[ ! -x $POINTER ]] || ! command -v foot > /dev/null; then
  echo "hide-when-typing helpers are not available"
  exit 1
fi

pointer() { "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"; }
windows() { "$UMBRIEL" windows --json; }

wait_for_active() {
  for _ in $(seq 60); do
    [[ $(windows | jq -r --arg t "$1" '.[] | select(.title == $t) | .active') == true ]] && return 0
    sleep 0.1
  done
  echo "expected $1 to be active: $(windows)"
  return 1
}

center() { windows | jq -r --arg t "$1" '.[] | select(.title == $t) | "\(.x + .w / 2 | floor) \(.y + .h / 2 | floor)"'; }

# Pixels in a 48x48 crop at the hotspot that differ from the hotspot pixel: zero when the window background is flat,
# so no cursor is drawn there.
cursor_pixels_at() {
  local x=$1 y=$2 predicate
  grim -c "$SHOT"
  predicate=$("$UMBRIEL_PIXEL_PROBE" "$SHOT" pixel "$x" "$y" | awk '{
    printf "r < %.4f || r > %.4f || g < %.4f || g > %.4f || b < %.4f || b > %.4f",
      ($1 - 0.5) / 255, ($1 + 0.5) / 255, ($2 - 0.5) / 255, ($2 + 0.5) / 255, ($3 - 0.5) / 255, ($3 + 0.5) / 255
  }')
  "$UMBRIEL_PIXEL_PROBE" "$SHOT" count "$predicate" "48x48+$x+$y"
}

cat >> "$UMBRIEL_CONFIG" <<'EOF2'

[animation]
enabled = false

[layout.scrolling]
default_extent_fraction = 0.5

[input.focus]
follows_mouse = "sloppy"

[input.cursor]
follows_focus = true
hide_when_typing = true
hide_timeout_ms = 0

[keybinds]
"Mod+L" = "window-focus-right"
"Mod+WheelUp" = "window-focus-left"
EOF2
"$UMBRIEL" msg config-reload > /dev/null

foot --config=/dev/null --title=warp-a sh -c 'sleep 120' > /dev/null 2>&1 &
foot --config=/dev/null --title=warp-b sh -c 'sleep 120' > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $(windows | jq 'length') -eq 2 ]] && break
  sleep 0.1
done
"$UMBRIEL" settle
left=$(windows | jq -r 'sort_by(.x) | .[0].title')
right=$(windows | jq -r 'sort_by(.x) | .[1].title')
read -r left_x left_y < <(center "$left")
read -r right_x right_y < <(center "$right")

"$UMBRIEL" msg window-focus "$(windows | jq -r --arg t "$left" '.[] | select(.title == $t) | .id')" > /dev/null
pointer move "$left_x" "$left_y"
"$UMBRIEL" settle
wait_for_active "$left"
if (($(cursor_pixels_at "$left_x" "$left_y") == 0)); then
  echo "positive control did not capture a visible cursor at $left_x,$left_y"
  exit 1
fi

pointer mod logo tap "$KEY_L" mod none
wait_for_active "$right"
"$UMBRIEL" settle
for at in "$left_x $left_y" "$right_x $right_y"; do
  read -r x y <<< "$at"
  if ((pixels = $(cursor_pixels_at "$x" "$y"))); then
    echo "keyboard focus warp showed the typing-hidden cursor: $pixels cursor pixels at $x,$y"
    exit 1
  fi
done

pointer relative 3 2
"$UMBRIEL" settle
wait_for_active "$right"
if (($(cursor_pixels_at $((right_x + 3)) $((right_y + 2))) == 0)); then
  echo "motion did not reveal the cursor on the newly focused window"
  exit 1
fi

# Hide the cursor again with an unbound key, so the wheel bind starts from a hidden cursor and has to show it.
pointer tap "$KEY_A"
"$UMBRIEL" settle
if ((pixels = $(cursor_pixels_at $((right_x + 3)) $((right_y + 2))))); then
  echo "typing did not hide the cursor before the wheel bind: $pixels cursor pixels"
  exit 1
fi
pointer mod logo notch -1 mod none
wait_for_active "$left"
"$UMBRIEL" settle
if (($(cursor_pixels_at "$left_x" "$left_y") == 0)); then
  echo "wheel focus warp did not leave a visible cursor on the left window"
  exit 1
fi
pointer click "$BTN_LEFT"
wait_for_active "$left"

echo "typing-hidden cursor stays hidden through a focus warp and reappears on the focused window"
