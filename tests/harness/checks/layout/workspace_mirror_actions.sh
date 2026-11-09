#!/usr/bin/env bash
# harness: outputs=2
# Keyboard window actions operate on the copied layout, not on the mirror's empty passive workspace.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
cat >> "$UMBRIEL_CONFIG" <<'TOML'
[animation]
enabled = false
[input.focus]
follows_mouse = false
[output.HEADLESS-1]
mode = "1280x720@60"
position = [0, 0]
workspaces = ["source", "away"]
[output.HEADLESS-2]
mode = "960x720@60"
position = [1280, 0]
workspaces = ["own", "source"]
[[workspace]]
output = "HEADLESS-2"
name = "source"
mirror = { output = "HEADLESS-1", name = "source" }
[layout.scrolling]
default_extent_fraction = 0.3
[keybinds]
"Mod+H" = "window-focus-left"
"Mod+L" = "window-focus-right"
"Mod+J" = "window-focus-down"
"Mod+K" = "window-focus-up"
"Mod+F" = "window-toggle-maximize"
[[window_rule]]
match.title = "^mirror-key-"
default_output = "HEADLESS-1"
default_workspace = "source"
TOML
"$UMBRIEL" msg config-reload > /dev/null
for name in a b c; do
  "$UMBRIEL_SEAT_LOG_CLIENT" "mirror-key-$name" > "$UMBRIEL_RUNTIME_DIR/$name.log" 2>&1 &
  await_events "$UMBRIEL_RUNTIME_DIR/$name.log" keyboard-enter 1
done
id_a=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "mirror-key-a") | .id')
"$UMBRIEL_POINTER_CLIENT" 2240 720 move 640 350
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg window-focus "$id_a" > /dev/null
"$UMBRIEL" msg window-consume-from-right > /dev/null
"$UMBRIEL" settle > /dev/null

assert_field() {
  local title=$1 field=$2 expected=$3 actual
  "$UMBRIEL" settle > /dev/null
  actual=$("$UMBRIEL" windows --json | jq -r --arg title "mirror-key-$title" --arg field "$field" '.[] | select(.title == $title) | .[$field]')
  if [[ $actual != "$expected" ]]; then
    echo "$title: expected $field=$expected, got $actual"
    return 1
  fi
}

assert_context() {
  "$UMBRIEL" workspaces --json | jq -e '
    any(.[]; .output == "HEADLESS-1" and .name == "away" and .active)
    and any(.[]; .output == "HEADLESS-2" and .name == "source" and .active and .focused)' > /dev/null
  "$UMBRIEL" windows --json | jq -e 'all(.[]; .output == "HEADLESS-1" and .workspace == "HEADLESS-1:1")' > /dev/null
}

key() {
  "$UMBRIEL_POINTER_CLIENT" 2240 720 mod logo tap "$1" mod none
  "$UMBRIEL" settle > /dev/null
}

# Two rows in the first column, one window in the second.
assert_field a column 0
assert_field a row 0
assert_field b column 0
assert_field b row 1
assert_field c column 1
"$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
"$UMBRIEL" msg workspace-switch away --output HEADLESS-1 > /dev/null
"$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
"$UMBRIEL_POINTER_CLIENT" 2240 720 move 1600 350
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg window-focus "$id_a" > /dev/null
assert_context

# Super+F maximizes the source column and restores its width, with the origin hidden or visible.
normal_width=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "mirror-key-a") | .w')
for origin in away source; do
  "$UMBRIEL" msg workspace-switch "$origin" --output HEADLESS-1 > /dev/null
  "$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
  key 33 # F
  assert_field a maximized true
  maximized_width=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "mirror-key-a") | .w')
  if ((maximized_width <= normal_width)); then
    echo "Super+F did not widen the copied source: $normal_width -> $maximized_width"
    exit 1
  fi
  grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/maximized.png"
  pixel=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/maximized.png" pixel 880 200)
  if [[ $pixel != "51 136 204" ]]; then
    echo "Maximized copied window is not drawn at the right edge: $pixel"
    exit 1
  fi
  "$UMBRIEL" workspaces --json | jq -e --arg origin "$origin" '
    any(.[]; .output == "HEADLESS-1" and .name == $origin and .active)
    and any(.[]; .output == "HEADLESS-2" and .name == "source" and .active and .focused)' > /dev/null
  key 33 # F restores the previous column width.
  assert_field a maximized false
  assert_field a w "$normal_width"
done
"$UMBRIEL" msg workspace-switch away --output HEADLESS-1 > /dev/null
"$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
assert_context

# The actual Super+H/J/K/L key events must navigate the hidden source's columns and rows.
key 38 # L
assert_field c focused true
assert_context
key 35 # H
assert_field a focused true
key 36 # J
assert_field b focused true
assert_context
key 37 # K
assert_field a focused true

# These are the actions bound to Super+Shift+J/K/H/L in Noxis.
"$UMBRIEL" msg window-move-down > /dev/null
assert_field a row 1
assert_field b row 0
assert_context
"$UMBRIEL" msg window-move-up > /dev/null
assert_field a row 0
assert_field b row 1
"$UMBRIEL" msg column-move-right > /dev/null
assert_field c column 0
assert_field a column 1
assert_field b column 1
assert_context
"$UMBRIEL" msg column-move-left > /dev/null
assert_field a column 0
assert_field b column 0
assert_field c column 1
assert_context

# Width and fullscreen actions resize the copied source too, not the mirror's empty layout.
width_of() { "$UMBRIEL" windows --json | jq -r --arg title "mirror-key-$1" '.[] | select(.title == $title) | .w'; }
column_width=$(width_of a)
"$UMBRIEL" msg window-modify-width-right 0.1 > /dev/null
"$UMBRIEL" settle > /dev/null
if (($(width_of a) <= column_width)); then
  echo "window-modify-width-right did not widen the copied source: $column_width -> $(width_of a)"
  exit 1
fi
"$UMBRIEL" msg window-modify-width-right -0.1 > /dev/null
assert_field a w "$column_width"
assert_context
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
assert_field a fullscreen true
assert_context
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
assert_field a fullscreen false
assert_field a w "$column_width"

# Focus follows where the pointer is. With the pointer on the source's own output, focusing a window the hidden source
# holds shows that workspace there, and window-focus-last comes back; with the pointer on the copy it stays hidden.
active_on() { "$UMBRIEL" workspaces --json | jq -r --arg out "$1" '.[] | select(.output == $out and .active) | .name'; }
expect_active() {
  local actual
  actual=$(active_on "$1")
  if [[ $actual != "$2" ]]; then
    echo "$3: expected $1 on $2, got $actual"
    "$UMBRIEL" workspaces --json
    return 1
  fi
}
# The seat's keyboard focus, read from the window that holds it: entered and not yet left.
expect_seat() {
  local log="$UMBRIEL_RUNTIME_DIR/$1.log" enters leaves
  for _ in $(seq 60); do
    enters=$(events "$log" keyboard-enter)
    leaves=$(events "$log" keyboard-leave)
    ((enters - leaves == 1)) && return 0
    sleep 0.05
  done
  echo "$2: expected the keyboard on $1 (enters=$enters leaves=$leaves)"
  return 1
}
window_field() { "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'; }
"$UMBRIEL_POINTER_CLIENT" 2240 720 move 640 350
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg workspace-switch away --output HEADLESS-1 > /dev/null
"$UMBRIEL_SEAT_LOG_CLIENT" other-away > "$UMBRIEL_RUNTIME_DIR/other.log" 2>&1 &
await_events "$UMBRIEL_RUNTIME_DIR/other.log" keyboard-enter 1
id_b=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "mirror-key-b") | .id')
expect_active HEADLESS-1 away "window on the visible workspace"
expect_active HEADLESS-2 source "copy shown"
expect_seat other "new window"

"$UMBRIEL" msg window-focus "$id_b" > /dev/null
"$UMBRIEL" settle > /dev/null
expect_active HEADLESS-1 source "window-focus of a copied window from the source output"
expect_seat b "window-focus of a copied window"
"$UMBRIEL" msg window-focus-last > /dev/null
"$UMBRIEL" settle > /dev/null
expect_active HEADLESS-1 away "window-focus-last back to the visible workspace"
expect_seat other "window-focus-last"
"$UMBRIEL" msg window-focus-last > /dev/null
"$UMBRIEL" settle > /dev/null
expect_active HEADLESS-1 source "window-focus-last to the copied window"
expect_seat b "window-focus-last"

# On the copy's output the source stays hidden and the copied window still takes the keyboard.
"$UMBRIEL" msg workspace-switch away --output HEADLESS-1 > /dev/null
"$UMBRIEL" msg window-focus "$(window_field other-away id)" > /dev/null
expect_seat other "focus on the visible workspace"
"$UMBRIEL_POINTER_CLIENT" 2240 720 move 1600 350
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg window-focus "$id_b" > /dev/null
"$UMBRIEL" settle > /dev/null
expect_active HEADLESS-1 away "window-focus of a copied window from the copy's output"
expect_active HEADLESS-2 source "copy stays shown"
expect_seat b "window-focus from the copy's output"

# Refocusing the window already typed in through the copy leaves the source output alone, wherever the pointer rests.
"$UMBRIEL_POINTER_CLIENT" 2240 720 move 640 350
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg window-focus "$id_b" > /dev/null
"$UMBRIEL" settle > /dev/null
expect_active HEADLESS-1 away "refocus of the window holding the keyboard through the copy"
expect_seat b "refocus through the copy"

# A hidden tab of the copied column takes the keyboard from the copy without switching the source output.
"$UMBRIEL_POINTER_CLIENT" 2240 720 move 1600 350
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg window-focus "$(window_field mirror-key-a id)" > /dev/null
"$UMBRIEL" msg column-set-display tabbed > /dev/null
"$UMBRIEL" settle > /dev/null
expect_seat a "focus on the first tab"
[[ $(window_field mirror-key-b tab_hidden) == true ]] || { echo "b is not a hidden tab"; "$UMBRIEL" windows --json; exit 1; }
"$UMBRIEL" msg column-focus-tab-next > /dev/null
"$UMBRIEL" settle > /dev/null
expect_seat b "column-focus-tab-next from the copy"
[[ $(window_field mirror-key-b tab_hidden) == false ]] || { echo "b did not become the visible tab"; exit 1; }
expect_active HEADLESS-1 away "tab focus from the copy"
expect_active HEADLESS-2 source "copy stays shown after the tab focus"

# Workspace navigation still addresses the receiving output, not the copied source's output.
"$UMBRIEL" msg workspace-switch own --output HEADLESS-2 > /dev/null
"$UMBRIEL" workspaces --json | jq -e '
  any(.[]; .output == "HEADLESS-2" and .name == "own" and .active)
  and any(.[]; .output == "HEADLESS-1" and .name == "away" and .active)' > /dev/null
echo 'Focus, reordering, maximize, width and fullscreen act on the copied source without activating its output; focus from the source output switches it, and hidden tabs focus through the copy.'
