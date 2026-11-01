#!/usr/bin/env bash
# harness: outputs=2
# A transferred name and layout must be recreated when its new output returns.
set -euo pipefail

readonly BASELINE="$(< "$UMBRIEL_CONFIG")"
readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}"
readonly FOREIGN="${UMBRIEL_FOREIGN_TOPLEVEL_CLIENT:-./build-debug/tests/foreign-toplevel-client}"
msg() { "$UMBRIEL" msg "$@" > /dev/null; }
state() { "$UMBRIEL" workspaces --json; }
window_id() { "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | .id'; }
spawn() {
  "$UMBRIEL_UNMAP_CLIENT" "$1" 300 200 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 60); do
    [[ -n $(window_id "$1") ]] && return
    sleep 0.05
  done
  echo "window $1 did not map"; exit 1
}
geometry() {
  "$UMBRIEL" windows --json | jq -c '[.[] | {title, x, y, w, h, floating, pinned}] | sort_by(.title)'
}

for inventory in fixed dynamic fixed-anonymous dynamic-anonymous; do
  printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
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
[[workspace]]
name = "WORK"
output = "HEADLESS-1"
layout.mode = "dwindle"
CONFIG
  if [[ $inventory == dynamic* ]]; then
    sed -i '/^workspaces = /d' "$UMBRIEL_CONFIG"
  fi
  selector=WORK
  named=true
  if [[ $inventory == *-anonymous ]]; then
    named=false
    selector=2
    [[ $inventory == dynamic* ]] && selector=1
    sed -i -e 's/workspaces = \["BEFORE", "WORK", "AFTER"\]/workspaces = 3/' \
      -e "s/name = \"WORK\"/index = $selector/" "$UMBRIEL_CONFIG"
  fi
  msg config-reload
  msg workspace-switch "$selector" --output "HEADLESS-1"
  spawn reconnect-tile-one
  spawn reconnect-tile-two
  spawn reconnect-float
  msg window-toggle-floating
  spawn reconnect-pin
  msg window-toggle-floating
  msg window-toggle-pinned
  msg window-focus "$(window_id reconnect-tile-one)"
  msg workspace-move-to-output-next
  "$UMBRIEL" settle
  expected=$(geometry)
  original=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "reconnect-tile-one") | .workspace')
  index=$(state | jq --arg id "$original" '.[] | select(.id == $id) | .index')

  layout=dwindle
  override=false
  # Reconnect without reload, with unchanged config, with unrelated settings, and with a layout change.
  for cycle in 1 2 3 4; do
    if [[ $cycle == 3 ]]; then
      msg window-focus-warp "$(window_id reconnect-tile-one)"
      msg workspace-set-layout master
      "$UMBRIEL" settle
      layout=master
      override=true
      expected=$(geometry)
    fi
    "$UMBRIEL" output-destroy HEADLESS-2 > /dev/null
    "$UMBRIEL" settle
    if [[ $cycle == 3 ]]; then
      printf '\n[animation.windows_move]\nduration_ms = 123\n' >> "$UMBRIEL_CONFIG"
    elif [[ $cycle == 4 ]]; then
      printf '\n[layout]\ngap = 12\n' >> "$UMBRIEL_CONFIG"
      layout=scrolling
      override=false
    fi
    if (( cycle > 1 )); then msg config-reload; fi
    "$UMBRIEL" output-create HEADLESS-2 > /dev/null
    "$UMBRIEL" settle
    restored=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "reconnect-tile-one") | .workspace')
    if ! state | jq -e --arg id "$restored" --argjson index "$index" --argjson named "$named" \
      --arg layout "$layout" --argjson override "$override" '
      any(.[]; .id == $id and .output == "HEADLESS-2" and .named == $named
          and (if $named then .name == "WORK" else true end)
          and .active and .index == $index and .layout == $layout and .layout_override == $override)
      and (map(.id) | length == (unique | length))' > /dev/null; then
      echo "$inventory reconnect $cycle did not restore the expected workspace and layout"
      state
      exit 1
    fi
    # The pinned window stayed on its output when the workspace moved.
    "$UMBRIEL" windows --json | jq -e --arg id "$restored" '
      length == 4
      and all(.[]; if .title == "reconnect-pin" then .pinned and .output == "HEADLESS-1"
                   else .workspace == $id and .output == "HEADLESS-2" end)' > /dev/null
    if (( cycle < 4 )); then
      [[ $(geometry) == "$expected" ]] || { echo "reconnect changed window geometry"; exit 1; }
    fi
    [[ $("$WORKSPACE" --groups | awk -F '\t' -v id="$restored" '$1 == id {print $3}') == HEADLESS-2 ]]
    for title in reconnect-tile-one reconnect-tile-two reconnect-float; do
      "$FOREIGN" "$title" HEADLESS-2
    done
    "$FOREIGN" reconnect-pin HEADLESS-1
  done
  for title in reconnect-tile-one reconnect-tile-two reconnect-float reconnect-pin; do
    msg window-close "$(window_id "$title")"
  done
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq length) == 0 ]] && break
    sleep 0.05
  done
  [[ $("$UMBRIEL" windows --json | jq length) == 0 ]]
done
echo "transferred names, layouts and windows survive repeated output recreation; pinned windows stay on their output"
