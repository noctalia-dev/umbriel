#!/usr/bin/env bash
# harness: outputs=3
# A transfer moves a stable workspace identity and every member across output groups.
set -euo pipefail

readonly BASELINE="$(< "$UMBRIEL_CONFIG")"
readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}"
readonly FOREIGN="${UMBRIEL_FOREIGN_TOPLEVEL_CLIENT:-./build-debug/tests/foreign-toplevel-client}"

msg() { "$UMBRIEL" msg "$@" > /dev/null; }
state() { "$UMBRIEL" workspaces --json; }
window_id() { "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | .id'; }
spawn() {
  foot --title="$1" sleep 120 > /dev/null 2>&1 &
  for _ in $(seq 60); do
    [[ -n $(window_id "$1") ]] && return
    sleep 0.05
  done
  echo "window $1 did not map"; exit 1
}
check() {
  local snapshot
  snapshot=$(state)
  if ! jq -e "$@" <<< "$snapshot" > /dev/null; then
    echo "workspace assertion failed: $snapshot"; exit 1
  fi
}
reject() {
  local before after
  "$UMBRIEL" settle
  before=$(state)
  if msg "$1" 2> /dev/null; then echo "unexpectedly accepted $1"; exit 1; fi
  after=$(state)
  [[ $before == "$after" ]] || { echo "rejected transfer changed inventory"; exit 1; }
}

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[output.HEADLESS-1]
position = [0, 0]
workspaces = ["BEFORE", "WORK", "AFTER"]
[output.HEADLESS-2]
position = [1280, 0]
scale = 1.5
workspaces = ["TARGET", "TAIL"]
[output.HEADLESS-3]
position = [2560, 0]
workspaces = ["THIRD"]
[input.cursor]
follows_focus = true
CONFIG
msg config-reload
msg workspace-switch WORK --output HEADLESS-1
work=$(state | jq -r '.[] | select(.name == "WORK") | .id')
spawn transfer-tile
spawn transfer-float
msg window-toggle-floating
spawn transfer-pin
msg window-toggle-floating
msg window-toggle-pinned
msg window-focus "$(window_id transfer-tile)"
"$UMBRIEL" settle

# IDs and names survive the group change. Fixed inventories shrink and grow rather than reusing a slot. Pinned windows
# stay on their output with the source's newly active workspace.
msg workspace-move-to-output-next
"$UMBRIEL" settle
check --arg id "$work" '
  any(.[]; .id == $id and .name == "WORK" and .output == "HEADLESS-2" and .index == 2 and .focused)
  and ([.[] | select(.output == "HEADLESS-1") | .name] == ["BEFORE", "AFTER"])
  and ([.[] | select(.output == "HEADLESS-2") | .name] == ["TARGET", "WORK", "TAIL"])
  and any(.[]; .name == "BEFORE" and .active)'
before=$(state | jq -r '.[] | select(.name == "BEFORE") | .id')
"$UMBRIEL" windows --json | jq -e --arg id "$work" --arg before "$before" '
  length == 3
  and all(.[]; if .title == "transfer-pin" then .workspace == $before and .output == "HEADLESS-1" and .pinned
               else .workspace == $id end)' > /dev/null
[[ $("$WORKSPACE" --groups | awk -F '\t' -v id="$work" '$1 == id && $2 == "WORK" {print $3}') == HEADLESS-2 ]]
for title in transfer-tile transfer-float; do
  "$FOREIGN" "$title" HEADLESS-2
done
"$FOREIGN" transfer-pin HEADLESS-1

# Cycle through three outputs in both directions, including wraparound, and move an empty named workspace.
for step in next:3 next:1 previous:3 previous:2; do
  msg "workspace-move-to-output-${step%:*}"
  check --arg id "$work" --arg output "HEADLESS-${step#*:}" 'any(.[]; .id == $id and .output == $output and .focused)'
done
msg workspace-move-to-output-left
check --arg id "$work" 'any(.[]; .id == $id and .output == "HEADLESS-1" and .focused)'

# Attached dialogs keep following their placement root, including foreign-toplevel output membership.
for title in transfer-float transfer-pin; do msg window-close "$(window_id "$title")"; done
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq length) == 1 ]] && break
  sleep 0.05
done
[[ $("$UMBRIEL" windows --json | jq length) == 1 ]]
mkfifo "$UMBRIEL_RUNTIME_DIR/dialog-control"
exec {dialog_fd}<>"$UMBRIEL_RUNTIME_DIR/dialog-control"
TRANSIENT_SUITE=1 TRANSIENT_MODAL=1 TRANSIENT_DIALOG_ON_STDIN=1 FILL_COLOR=0xFFFF0000 \
  "$UMBRIEL_UNMAP_CLIENT" transfer-dialog 200 150 <&"$dialog_fd" > "$UMBRIEL_RUNTIME_DIR/dialog.log" 2>&1 &
for _ in $(seq 60); do
  [[ -n $(window_id transfer-dialog) ]] && break
  sleep 0.05
done
[[ -n $(window_id transfer-dialog) ]]
msg window-focus "$(window_id transient-parent)"
msg workspace-move-to-output-next
"$UMBRIEL" settle
"$UMBRIEL" windows --json | jq -e --arg id "$work" 'any(.[]; .title == "transfer-dialog" and .active and .workspace == $id and .output == "HEADLESS-2")' > /dev/null
"$FOREIGN" transfer-dialog HEADLESS-2
grim -o HEADLESS-1 "$UMBRIEL_RUNTIME_DIR/transfer-source.png"
grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/transfer-destination.png"
[[ $("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/transfer-source.png" count 'r > 0.7 && g < 0.2 && b < 0.2') == 0 ]]
(( $("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/transfer-destination.png" count 'r > 0.7 && g < 0.2 && b < 0.2') > 1000 ))
msg workspace-move-to-output-previous
msg workspace-switch THIRD --output HEADLESS-3
empty=$(state | jq -r '.[] | select(.name == "THIRD") | .id')
msg workspace-move-to-output-next
check --arg id "$empty" '
  any(.[]; .id == $id and .output == "HEADLESS-1" and .focused and .window_count == 0)
  and ([.[] | select(.output == "HEADLESS-3")] | length == 1)'

# The layout travels even when the destination has another configured layout.
msg workspace-switch WORK --output HEADLESS-1
for layout in dwindle master scrolling stacking; do
  msg workspace-set-layout "$layout"
  msg workspace-move-to-output-right
  "$UMBRIEL" settle
  check --arg id "$work" --arg layout "$layout" 'any(.[]; .id == $id and .layout == $layout and .output == "HEADLESS-2")'
  msg workspace-move-to-output-left
done

# Fullscreen roots and overview cards follow the workspace.
msg workspace-set-layout scrolling
msg window-focus "$(window_id transfer-tile)"
msg window-toggle-fullscreen
msg workspace-move-to-output-right
"$UMBRIEL" settle
"$UMBRIEL" windows --json | jq -e '.[] | select(.title == "transfer-tile") | .fullscreen and .output == "HEADLESS-2"' > /dev/null
msg window-toggle-fullscreen
msg overview-toggle
msg workspace-move-to-output-left
"$UMBRIEL" settle
check --arg id "$work" 'any(.[]; .id == $id and .output == "HEADLESS-1")'
msg overview-toggle

# A changed configured inventory is reapplied on reload after session-only transfers.
sed -i 's/"BEFORE", "WORK", "AFTER"/"BEFORE", "WORK", "AFTER", "NEW"/' "$UMBRIEL_CONFIG"
msg config-reload
check '([.[] | select(.output == "HEADLESS-1") | .name] == ["BEFORE", "WORK", "AFTER", "NEW"])
  and ([.[] | select(.output == "HEADLESS-2") | .name] == ["TARGET", "TAIL"])'

# Transfer an incoming workspace while its source is still showing a workspace slide.
sed -i 's/enabled = false/enabled = true/' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation.workspaces]
duration_ms = 1000
curve = "linear"
CONFIG
msg config-reload
msg workspace-switch BEFORE --output HEADLESS-1
"$UMBRIEL" settle
"$UMBRIEL" clock-freeze
msg workspace-switch WORK --output HEADLESS-1
"$UMBRIEL" clock-advance 250
msg workspace-move-to-output-right
"$UMBRIEL" clock-advance 1200
"$UMBRIEL" clock-resume
"$UMBRIEL" settle
check --arg id "$work" 'any(.[]; .id == $id and .output == "HEADLESS-2" and .active)'

# Dynamic reconciliation must neither recreate the source name nor anonymize the transferred workspace.
printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[output.HEADLESS-1]
position = [0, 0]
[output.HEADLESS-2]
position = [1280, 0]
[output.HEADLESS-3]
position = [2560, 0]
[[workspace]]
name = "DYNAMIC"
output = "HEADLESS-1"
layout.mode = "dwindle"
[[workspace]]
name = "DUPLICATE"
CONFIG
msg config-reload
msg workspace-switch DYNAMIC --output HEADLESS-1
dynamic=$(state | jq -r '.[] | select(.name == "DYNAMIC") | .id')
spawn dynamic-member
msg workspace-move-to-output-next
spawn dynamic-after-transfer
msg window-close
"$UMBRIEL" settle
check --arg id "$dynamic" '
  ([.[] | select(.name == "DYNAMIC")] | length == 1)
  and any(.[]; .id == $id and .output == "HEADLESS-2" and .layout == "dwindle" and .named)'
msg workspace-switch DUPLICATE --output HEADLESS-1
reject workspace-move-to-output-next

# Repeated moves of an anonymous workspace cannot invalidate it during dynamic pruning.
msg workspace-switch DYNAMIC --output HEADLESS-2
msg workspace-next
anonymous=$(state | jq -r '.[] | select(.focused) | .id')
msg workspace-move-to-output-previous
check --arg id "$anonymous" 'any(.[]; .id == $id and .output == "HEADLESS-1" and .focused)'
msg workspace-move-to-output-next
check --arg id "$anonymous" 'any(.[]; .id == $id and .output == "HEADLESS-2" and .focused)'

echo "whole workspaces retain identity, members, layouts and names across output transfers"
