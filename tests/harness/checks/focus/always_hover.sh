#!/usr/bin/env bash
# follows_mouse = "always" focuses the window under the pointer on every motion, even without a border crossing. A
# still pointer keeps a keyboard choice or a new window's focus, but a layout command or workspace switch that leaves
# another window beneath it focuses that window without motion. With follows_focus the pointer travels to the keyboard
# choice, so motion keeps it.
set -euo pipefail

readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

windows() { "$UMBRIEL" windows --json; }
field() { windows | jq -r --arg title "$1" ".[] | select(.title == \$title) | .$2"; }
pointer() { "$POINTER" 1280 720 "$@"; }

spawn() {
  "$CLIENT" "$1" 1200 700 > /dev/null 2>&1 &
  for _ in $(seq 60); do
    [[ -n $(field "$1" id) ]] && return 0
    sleep 0.1
  done
  echo "window $1 did not map: $(windows)"
  exit 1
}

wait_for_active() {
  for _ in $(seq 60); do
    [[ $(field "$1" active) == true ]] && return 0
    sleep 0.05
  done
  echo "$2: expected $1 to be active: $(windows)"
  exit 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "dwindle"

[animation]
enabled = false

[input.focus]
follows_mouse = "always"

[[window_rule]]
match.title = "^always-away$"
default_floating = true
default_floating_size_px = { width = 300, height = 200 }
default_position = { x = 900, y = 450, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn always-left
spawn always-right
"$UMBRIEL" settle
left_x=$(($(field always-left x) + 40))
pointer move "$left_x" 300
wait_for_active always-left 'pointer motion'

"$UMBRIEL" msg window-focus "$(field always-right id)" > /dev/null
"$UMBRIEL" settle
wait_for_active always-right 'keyboard focus while the pointer is still'
pointer move "$((left_x + 1))" 300
wait_for_active always-left 'motion inside the window under the pointer after keyboard focus'

spawn always-away
"$UMBRIEL" settle
wait_for_active always-away 'a window opening away from a still pointer'
pointer move "$left_x" 300
wait_for_active always-left 'motion inside the window under the pointer after a new window'

# A swap brings always-right under the still pointer.
"$UMBRIEL" msg window-swap-right > /dev/null
"$UMBRIEL" settle
wait_for_active always-right 'a swap that moves another window under a still pointer'

# The second window maps beside the first and keeps focus, then a return to this workspace restores that remembered
# focus and hands it to the window under the pointer.
"$UMBRIEL" msg workspace-switch 2 > /dev/null
spawn always-ws-a
spawn always-ws-b
"$UMBRIEL" settle
wait_for_active always-ws-b 'a window opening beside the still pointer'
"$UMBRIEL" msg workspace-switch 1 > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" msg workspace-switch 2 > /dev/null
"$UMBRIEL" settle
wait_for_active always-ws-a 'a workspace switch that leaves another window under a still pointer'
"$UMBRIEL" msg workspace-switch 1 > /dev/null
"$UMBRIEL" settle

"$UMBRIEL" config set input.cursor.follows_focus true > /dev/null
"$UMBRIEL" msg window-focus "$(field always-right id)" > /dev/null
"$UMBRIEL" settle
pointer relative 1 0
"$UMBRIEL" settle
wait_for_active always-right 'motion after follows_focus carried the pointer'

echo "always follows_mouse refocuses on every motion and leaves a still pointer's focus alone"
