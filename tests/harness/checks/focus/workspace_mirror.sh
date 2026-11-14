#!/usr/bin/env bash
# harness: outputs=2
# Focus on a window shown by a mirror follows the pointer's output. A mirror of the window's workspace on that output
# shows it there while the source output keeps its workspace; an output without one switches the window's own output.
# Background focus (a window opening or closing) never switches the receiving output.
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
workspaces = ["source", "away", "third"]
[output.HEADLESS-2]
mode = "960x720@60"
position = [1280, 0]
workspaces = ["own", "source", "third"]
[[workspace]]
output = "HEADLESS-2"
name = "source"
mirror = { output = "HEADLESS-1", name = "source" }
[[workspace]]
output = "HEADLESS-2"
name = "third"
mirror = { output = "HEADLESS-1", name = "third" }
[layout.scrolling]
default_extent_fraction = 0.3
[[window_rule]]
match.title = "^mirror-key-"
default_output = "HEADLESS-1"
default_workspace = "source"
TOML
"$UMBRIEL" msg config-reload > /dev/null

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
  for _ in $(seq 120); do
    enters=$(events "$log" keyboard-enter)
    leaves=$(events "$log" keyboard-leave)
    ((enters - leaves == 1)) && return 0
    sleep 0.05
  done
  echo "$2: expected the keyboard on $1 (enters=$enters leaves=$leaves)"
  return 1
}
window_field() { "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'; }
# Switching a workspace moves the pointer, so each step parks it where the case needs it.
park() {
  "$UMBRIEL_POINTER_CLIENT" 2240 720 move "$1" 350
  "$UMBRIEL" settle > /dev/null
}
readonly SOURCE_SIDE=640 RECEIVER_SIDE=1600

"$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
for name in a b; do
  "$UMBRIEL_SEAT_LOG_CLIENT" "mirror-key-$name" > "$UMBRIEL_RUNTIME_DIR/$name.log" 2>&1 &
  await_events "$UMBRIEL_RUNTIME_DIR/$name.log" keyboard-enter 1
done
id_a=$(window_field mirror-key-a id)
id_b=$(window_field mirror-key-b id)
park "$SOURCE_SIDE"
"$UMBRIEL" msg window-focus "$id_a" > /dev/null
"$UMBRIEL" msg window-consume-from-right > /dev/null
"$UMBRIEL" msg workspace-switch third --output HEADLESS-1 > /dev/null
"$UMBRIEL_SEAT_LOG_CLIENT" third-win > "$UMBRIEL_RUNTIME_DIR/third.log" 2>&1 &
await_events "$UMBRIEL_RUNTIME_DIR/third.log" keyboard-enter 1
"$UMBRIEL" msg workspace-switch away --output HEADLESS-1 > /dev/null
"$UMBRIEL_SEAT_LOG_CLIENT" other-away > "$UMBRIEL_RUNTIME_DIR/other.log" 2>&1 &
await_events "$UMBRIEL_RUNTIME_DIR/other.log" keyboard-enter 1
id_third=$(window_field third-win id)
id_other=$(window_field other-away id)
expect_active HEADLESS-1 away "window on the visible workspace"
expect_active HEADLESS-2 source "copy shown"
expect_seat other "new window"

# Pointer on the source's own output: focusing a window the hidden source holds shows that workspace there, and
# window-focus-last comes back.
park "$SOURCE_SIDE"
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
expect_seat b "window-focus-last to the copied window"

# Pointer on the copy: the source stays hidden and the copied window takes the keyboard.
"$UMBRIEL" msg workspace-switch away --output HEADLESS-1 > /dev/null
"$UMBRIEL" msg window-focus "$id_other" > /dev/null
expect_seat other "focus on the visible workspace"
park "$RECEIVER_SIDE"
"$UMBRIEL" msg window-focus "$id_b" > /dev/null
"$UMBRIEL" settle > /dev/null
expect_active HEADLESS-1 away "window-focus of a copied window from the copy's output"
expect_active HEADLESS-2 source "copy stays shown"
expect_seat b "window-focus from the copy's output"

# Refocusing the window already typed in through the copy leaves the source output alone, wherever the pointer rests.
park "$SOURCE_SIDE"
"$UMBRIEL" msg window-focus "$id_b" > /dev/null
"$UMBRIEL" settle > /dev/null
expect_active HEADLESS-1 away "refocus of the window holding the keyboard through the copy"
expect_seat b "refocus through the copy"

# A hidden tab of the copied column takes the keyboard from the copy without switching the source output.
park "$RECEIVER_SIDE"
"$UMBRIEL" msg window-focus "$id_a" > /dev/null
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

# Pointer on the receiver, a source it mirrors without showing: its mirror is shown there and the source output stays.
expect_active HEADLESS-1 away "source output before the receiver focus"
expect_active HEADLESS-2 source "receiver on the first mirror"
"$UMBRIEL" msg window-focus "$id_third" > /dev/null
"$UMBRIEL" settle > /dev/null
expect_active HEADLESS-2 third "window-focus of a window of another mirrored source"
expect_active HEADLESS-1 away "source output left alone"
expect_seat third "window-focus through the receiver's other mirror"
"$UMBRIEL" msg window-focus-last > /dev/null
"$UMBRIEL" settle > /dev/null
expect_active HEADLESS-2 source "window-focus-last back to the first mirror"
expect_active HEADLESS-1 away "source output left alone going back"
expect_seat b "window-focus-last to the first mirror's window"
"$UMBRIEL" msg window-focus-last > /dev/null
"$UMBRIEL" settle > /dev/null
expect_active HEADLESS-2 third "window-focus-last to the other mirror again"
expect_active HEADLESS-1 away "source output left alone going forward"
expect_seat third "window-focus-last to the other mirror's window"

# A workspace the receiver does not mirror switches its own output.
"$UMBRIEL" msg workspace-switch source --output HEADLESS-1 > /dev/null
park "$RECEIVER_SIDE"
"$UMBRIEL" msg window-focus "$id_other" > /dev/null
"$UMBRIEL" settle > /dev/null
expect_active HEADLESS-1 away "window of an unmirrored workspace"
expect_active HEADLESS-2 third "receiver unchanged for an unmirrored workspace"
expect_seat other "window-focus of an unmirrored workspace"

# A source showing on its own output still shows its mirror on the receiver when the pointer is there.
"$UMBRIEL" msg workspace-switch source --output HEADLESS-1 > /dev/null
park "$RECEIVER_SIDE"
expect_active HEADLESS-1 source "source showing before the receiver focus"
expect_active HEADLESS-2 third "receiver on the other mirror before the receiver focus"
"$UMBRIEL" msg window-focus "$id_third" > /dev/null
expect_seat third "focus on the showing mirror's window"
"$UMBRIEL" msg window-focus "$id_b" > /dev/null
"$UMBRIEL" settle > /dev/null
expect_active HEADLESS-2 source "window-focus of a window on the showing source"
expect_active HEADLESS-1 source "the showing source stays"
expect_seat b "window-focus of a window on the showing source"
"$UMBRIEL" msg window-focus-last > /dev/null
"$UMBRIEL" settle > /dev/null
expect_active HEADLESS-2 third "window-focus-last back to the other mirror"
expect_active HEADLESS-1 source "the showing source stays going back"
expect_seat third "window-focus-last back to the other mirror's window"

# Background focus leaves the receiver alone: a window mapping on the showing source, then closing again.
"$UMBRIEL_SEAT_LOG_CLIENT" mirror-key-d > "$UMBRIEL_RUNTIME_DIR/d.log" 2>&1 &
d_pid=$!
await_events "$UMBRIEL_RUNTIME_DIR/d.log" keyboard-enter 1
"$UMBRIEL" settle > /dev/null
expect_active HEADLESS-2 third "receiver after a window mapped on the source"
expect_active HEADLESS-1 source "source after a window mapped on it"
kill "$d_pid"
for _ in $(seq 120); do
  [[ -z $(window_field mirror-key-d id) ]] && break
  sleep 0.05
done
[[ -z $(window_field mirror-key-d id) ]] || { echo "mirror-key-d did not close"; exit 1; }
"$UMBRIEL" settle > /dev/null
expect_active HEADLESS-2 third "receiver after the focused window closed on the source"
expect_active HEADLESS-1 source "source after the focused window closed"

echo 'Focus through a mirror follows the pointer'"'"'s output: the receiver shows its mirror, the source output keeps its workspace, and background focus never switches the receiver.'
