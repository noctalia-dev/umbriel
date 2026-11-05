#!/usr/bin/env bash
# harness: outputs=3
# Transfers address a copy's source, never its passive layout or the receiving output's own fallback.
set -Eeuo pipefail
source "$UMBRIEL_HARNESS_LIB"
trap 'echo "Transfer failure at line $LINENO: $BASH_COMMAND"; "$UMBRIEL" windows --json; "$UMBRIEL" workspaces --json' ERR
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
workspaces = ["own", "source@HEADLESS-1", "target@HEADLESS-3"]
[output.HEADLESS-3]
mode = "1280x720@60"
position = [0, 720]
workspaces = ["target", "away"]
[[window_rule]]
match.title = "^mirror-transfer-"
default_output = "HEADLESS-1"
default_workspace = "source"
TOML
"$UMBRIEL" msg config-reload > /dev/null
for name in a b; do
  "$UMBRIEL_SEAT_LOG_CLIENT" "mirror-transfer-$name" > "$UMBRIEL_RUNTIME_DIR/$name.log" 2>&1 &
  await_events "$UMBRIEL_RUNTIME_DIR/$name.log" keyboard-enter 1
done
id_a=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "mirror-transfer-a") | .id')
"$UMBRIEL_POINTER_CLIENT" 2240 1440 move 640 350
"$UMBRIEL" msg window-focus "$id_a" > /dev/null
"$UMBRIEL" msg window-consume-from-right > /dev/null
"$UMBRIEL" msg workspace-switch away --output HEADLESS-1 > /dev/null
"$UMBRIEL" msg workspace-switch away --output HEADLESS-3 > /dev/null
"$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
"$UMBRIEL_POINTER_CLIENT" 2240 1440 move 1600 350
"$UMBRIEL" msg window-focus "$id_a" > /dev/null

assert_destination() {
  local output=$1 name=$2 actual y
  "$UMBRIEL" settle > /dev/null
  "$UMBRIEL" windows --json | jq -e --arg workspace "$output:1" --arg output "$output" '
    all(.[]; .workspace == $workspace and .output == $output and .column == 0)
    and any(.[]; .title == "mirror-transfer-a" and .row == 0 and .focused)
    and any(.[]; .title == "mirror-transfer-b" and .row == 1)' > /dev/null
  "$UMBRIEL" workspaces --json | jq -e --arg name "$name" '
    any(.[]; .output == "HEADLESS-1" and .name == "away" and .active)
    and any(.[]; .output == "HEADLESS-3" and .name == "away" and .active)
    and any(.[]; .output == "HEADLESS-2" and .name == $name and .active and .focused)
    and all(.[] | select(.mirror or .name == "own"); .window_count == 0)' > /dev/null
  grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/transfer.png"
  for y in 200 500; do
    actual=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/transfer.png" pixel 480 "$y")
    if [[ $actual != "51 136 204" ]]; then
      echo "Transferred row missing from the copy at 480,$y: got $actual"
      return 1
    fi
  done
  for output in HEADLESS-1 HEADLESS-3; do
    grim -o "$output" "$UMBRIEL_RUNTIME_DIR/origin.png"
    actual=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/origin.png" pixel 640 200)
    if [[ $actual != "0 0 0" ]]; then
      echo "A hidden original leaked onto $output: got $actual"
      return 1
    fi
  done
}

# Same-source target is a no-op; both rows remain attached to their real source.
"$UMBRIEL" msg column-move-to-workspace 2 > /dev/null
assert_destination HEADLESS-1 source
# This is the action dispatched by Mod+Shift+O on the receiving output.
"$UMBRIEL" msg column-move-to-workspace 3 > /dev/null
assert_destination HEADLESS-3 target
"$UMBRIEL" msg column-move-to-workspace 2 > /dev/null
assert_destination HEADLESS-1 source

# An own workspace remains a real destination, and the column can return to a copy.
"$UMBRIEL" msg column-move-to-workspace 1 > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" windows --json | jq -e '
  all(.[]; .workspace == "HEADLESS-2:1" and .column == 0)
  and any(.[]; .title == "mirror-transfer-a" and .row == 0 and .focused)
  and any(.[]; .title == "mirror-transfer-b" and .row == 1)' > /dev/null
"$UMBRIEL" msg column-move-to-workspace 2 > /dev/null
assert_destination HEADLESS-1 source

# Individual transfers must resolve the same destination as whole-column transfers.
"$UMBRIEL" msg window-move-to-workspace 3 > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" windows --json | jq -e '
  any(.[]; .title == "mirror-transfer-a" and .workspace == "HEADLESS-3:1" and .column == 0 and .row == 0 and .focused)
  and any(.[]; .title == "mirror-transfer-b" and .workspace == "HEADLESS-1:1" and .column == 0 and .row == 0)' > /dev/null
"$UMBRIEL" msg window-move-to-workspace-silent 2 > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" windows --json | jq -e '
  all(.[]; .workspace == "HEADLESS-1:1" and .row == 0)
  and any(.[]; .title == "mirror-transfer-a" and .column == 1)
  and any(.[]; .title == "mirror-transfer-b" and .column == 0)' > /dev/null
"$UMBRIEL" workspaces --json | jq -e '
  any(.[]; .output == "HEADLESS-1" and .name == "away" and .active)
  and any(.[]; .output == "HEADLESS-3" and .name == "away" and .active)
  and any(.[]; .output == "HEADLESS-2" and .name == "target" and .active and .focused)
  and all(.[] | select(.mirror or .name == "own"); .window_count == 0)' > /dev/null
# Revealing the silent destination must apply positions accumulated while it was hidden.
"$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/silent-destination.png"
for x in 240 720; do
  actual=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/silent-destination.png" pixel "$x" 360)
  if [[ $actual != "51 136 204" ]]; then
    echo "Silently transferred window missing at $x,360: got $actual"
    exit 1
  fi
done
echo 'Column and individual transfers reach the real destination; stacked rows remain drawn in the copy and both origins stay hidden.'
