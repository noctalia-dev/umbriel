#!/usr/bin/env bash
# A tab group's overview card acts as the group: its shortcut label focuses the shown tab, a click on a tab of its bar
# focuses that tab (the one pressed, even if the overview scrolls before the release), and a card dropped on its bar
# joins the group between the tabs under the pointer.
set -euo pipefail

readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly FRAME="$UMBRIEL_RUNTIME_DIR/overview-tab-group-card.png"
# The shown tab's slot is the only magenta on the first workspace's preview.
readonly MAGENTA='r > 0.9 && g < 0.1 && b > 0.9'
source "$UMBRIEL_HARNESS_LIB"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[overview]
zoom = 0.5

[layout.scrolling]
default_extent_fraction = 0.3
center_focused = "never"

[appearance.tab_bar]
style = "indicator"
height = 24

[colors.tab_bar]
background = "#202020FF"
active = "#FF00FFFF"
active_unfocused = "#FF00FFFF"
EOF
"$UMBRIEL" msg config-reload > /dev/null

window_of() { "$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)'; }
focused_title() { "$UMBRIEL" windows --json | jq -r '[.[] | select(.focused) | .title] | first // "none"'; }

spawn() {
  "$CLIENT" "$1" 600 400 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 80); do
    [[ -n "$(window_of "$1")" ]] && return 0
    sleep 0.025
  done
  echo "window '$1' never appeared: $(cat "$UMBRIEL_RUNTIME_DIR/$1.log")"
  return 1
}

focus() { "$UMBRIEL" msg window-focus "$(window_of "$1" | jq -r '.id')" > /dev/null; }

# Opens the overview and reads where the shown tab's slot sits on the group's card.
open_overview() {
  "$UMBRIEL" msg overview-open > /dev/null
  "$UMBRIEL" settle
  grim "$FRAME"
  read -r slot_x slot_y slot_w slot_h < <("$UMBRIEL_PIXEL_PROBE" "$FRAME" bbox "$MAGENTA" 1280x360+0+0)
  if ((slot_w == 0 || slot_h == 0)); then
    echo "the group's card shows no tab bar"
    exit 1
  fi
}

spawn tab-a
spawn tab-b
"$UMBRIEL" msg window-consume-left > /dev/null
spawn tab-c
"$UMBRIEL" msg window-consume-left > /dev/null
"$UMBRIEL" msg column-toggle-tabbed > /dev/null
"$UMBRIEL" msg column-focus-tab 2 > /dev/null
spawn other
"$UMBRIEL" settle

open_overview
"$POINTER" 1280 720 tap 2
"$UMBRIEL" settle
if [[ $(focused_title) != tab-b ]]; then
  echo "shortcut 1 focused $(focused_title), not the group's shown tab tab-b: $("$UMBRIEL" windows --json)"
  exit 1
fi

focus other
open_overview
read -r other_x other_y < <(window_of other | jq -r '"\(320 + (.x + .w / 2) / 2 | floor) \(180 + (.y + .h / 2) / 2 | floor)"')
# Lifting the card re-centres the group's, so its slot is read again mid-drag.
pointer_hold 1280 720 move "$other_x" "$other_y" press 272 move $((other_x + 40)) $((other_y + 200)) --
"$UMBRIEL" settle
grim "$FRAME"
read -r slot_x slot_y _ slot_h < <("$UMBRIEL_PIXEL_PROBE" "$FRAME" bbox "$MAGENTA")
"$POINTER" 1280 720 move $((slot_x + 2)) $((slot_y + slot_h / 2))
"$UMBRIEL" settle
pointer_release
"$UMBRIEL" settle
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" settle
order=$("$UMBRIEL" windows --json | jq -r '[.[] | select(.tabbed)] | sort_by(.tab_index) | map(.title) | join(",")')
if [[ $order != tab-a,other,tab-b,tab-c ]]; then
  echo "the card dropped between tab-a and tab-b left the group as $order: $("$UMBRIEL" windows --json)"
  exit 1
fi

open_overview
"$POINTER" 1280 720 move $((slot_x - slot_w / 2)) $((slot_y + slot_h / 2)) click 272
"$UMBRIEL" settle
if [[ $(focused_title) != tab-a ]]; then
  echo "clicking tab-a on the group's card focused $(focused_title): $("$UMBRIEL" windows --json)"
  exit 1
fi

"$UMBRIEL" msg workspace-next > /dev/null
spawn tab-d
spawn tab-e
"$UMBRIEL" msg window-consume-left > /dev/null
"$UMBRIEL" msg column-toggle-tabbed > /dev/null
"$UMBRIEL" msg workspace-previous > /dev/null
open_overview
# tab-a is shown, two slots left of tab-b. A wheel notch mid-click brings the next workspace's bar under the pointer.
tab_b_x=$((slot_x + slot_w * 5 / 2))
pointer_hold 1280 720 move "$tab_b_x" $((slot_y + slot_h / 2)) press 272 -- notch 1 mark scrolled hold release 272
pointer_step scrolled
"$UMBRIEL" settle
pointer_release
"$UMBRIEL" settle
focused=$("$UMBRIEL" windows --json | jq -r '[.[] | select(.focused and .active) | .title] | first // "none"')
if [[ $focused != tab-b ]]; then
  echo "clicking tab-b while the overview scrolled focused $focused: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "the tab group's card focused its shown tab by shortcut, its clicked tab, and took a drop between its tabs"
