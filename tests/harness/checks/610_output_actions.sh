#!/usr/bin/env bash
# The new output, workspace, and window actions: parse + dispatch reach the handlers (an unregistered action would say "unknown action"), and the behaviors that
# work on a single headless output are asserted by their observable transitions.
set -euo pipefail

accepts() {
  if ! out=$("$UMBRIEL" msg "$1" 2>&1); then
    echo "expected '$1' to be accepted, got: $out"
    return 1
  fi
}

rejects_with() {
  local action=$1 expected=$2
  if out=$("$UMBRIEL" msg "$action" 2>&1); then
    echo "expected '$action' to be rejected, but it succeeded"
    return 1
  fi
  if [[ $out != *"$expected"* ]]; then
    echo "expected '$action' to mention '$expected', got: $out"
    return 1
  fi
}

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_windows() {
  local want=$1
  for _ in $(seq 40); do
    if [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $want window(s), got $("$UMBRIEL" windows --json | jq 'length')"
  return 1
}

# Single-output rejection: this check's instance boots exactly one headless output, so every directional output action must fail with a "no output" message. This
# also proves parse + dispatch reach the handler.
for action in \
  output-focus-left output-focus-right output-focus-up output-focus-down \
  window-focus-or-output-left window-focus-or-output-right window-focus-or-output-up window-focus-or-output-down \
  window-move-or-output-left window-move-or-output-right window-move-or-output-up window-move-or-output-down \
  window-move-to-output-left window-move-to-output-right window-move-to-output-up window-move-to-output-down \
  column-move-to-output-left column-move-to-output-right column-move-to-output-up column-move-to-output-down \
  workspace-move-to-output-left workspace-move-to-output-right workspace-move-to-output-up workspace-move-to-output-down; do
  rejects_with "$action" "no output"
done

# workspace-next / workspace-previous
spawn_client ws-flip
wait_for_windows 1

if [[ $("$UMBRIEL" windows --json | jq -r '.[0].active') != true ]]; then
  echo "expected the window to be active before the switch"
  exit 1
fi
accepts "workspace-next"
for _ in $(seq 40); do
  active_now=$("$UMBRIEL" windows --json | jq -r '.[0].active')
  [[ $active_now == false ]] && break
  sleep 0.1
done
if [[ $active_now != false ]]; then
  echo "expected the window to deactivate after workspace-next, still active"
  exit 1
fi
accepts "workspace-previous"
for _ in $(seq 40); do
  active_now=$("$UMBRIEL" windows --json | jq -r '.[0].active')
  [[ $active_now == true ]] && break
  sleep 0.1
done
if [[ $active_now != true ]]; then
  echo "expected the window to reactivate after workspace-previous"
  exit 1
fi

# Moving to an adjacent workspace follows the focused window. At either end
# the corresponding action remains a silent no-op.
start_workspace=$("$UMBRIEL" windows --json | jq -r '.[0].workspace')
accepts "window-move-to-workspace-next"
moved_workspace=$start_workspace
for _ in $(seq 40); do
  moved_workspace=$("$UMBRIEL" windows --json | jq -r '.[0].workspace')
  [[ $moved_workspace != "$start_workspace" ]] && break
  sleep 0.1
done
if [[ $moved_workspace == "$start_workspace" ]]; then
  echo "expected the focused window to move to the next workspace"
  exit 1
fi
if [[ $("$UMBRIEL" windows --json | jq -r '.[0].active') != true ]]; then
  echo "expected the moved window to remain active on the next workspace"
  exit 1
fi

accepts "window-move-to-workspace-previous"
returned_workspace=$moved_workspace
for _ in $(seq 40); do
  returned_workspace=$("$UMBRIEL" windows --json | jq -r '.[0].workspace')
  [[ $returned_workspace == "$start_workspace" ]] && break
  sleep 0.1
done
if [[ $returned_workspace != "$start_workspace" ]]; then
  echo "expected the focused window to return to $start_workspace, got $returned_workspace"
  exit 1
fi

# The cross-workspace variants first use an available vertical neighbor. Only
# the workspace boundary falls through to workspace navigation.
spawn_client vertical-local
wait_for_windows 2
local_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "vertical-local") | .id')
accepts "window-focus:$local_id"
accepts "window-consume-left"
stacked=false
for _ in $(seq 40); do
  if "$UMBRIEL" windows --json | jq -e \
      'length == 2 and (.[0].workspace == .[1].workspace) and ([.[].y] | unique | length == 2)' > /dev/null; then
    stacked=true
    break
  fi
  sleep 0.1
done
if [[ $stacked != true ]]; then
  echo "expected two rows in one column before vertical navigation"
  exit 1
fi
read -r top_id bottom_id <<< "$("$UMBRIEL" windows --json | jq -r 'sort_by(.y) | "\(.[0].id) \(.[1].id)"')"
accepts "window-focus:$top_id"
accepts "window-focus-or-workspace-down"
bottom_active=false
for _ in $(seq 40); do
  bottom_active=$("$UMBRIEL" windows --json | jq -r --arg id "$bottom_id" '.[] | select(.id == $id) | .active')
  [[ $bottom_active == true ]] && break
  sleep 0.1
done
if [[ $bottom_active != true ]]; then
  echo "expected focus-down variant to use the lower row before changing workspaces"
  exit 1
fi
if ! "$UMBRIEL" windows --json | jq -e --arg workspace "$start_workspace" \
    'all(.[]; .workspace == $workspace)' > /dev/null; then
  echo "vertical neighbor focus unexpectedly changed workspaces"
  exit 1
fi

accepts "window-move-or-workspace-up"
local_moved=false
for _ in $(seq 40); do
  local_moved=$("$UMBRIEL" windows --json | jq -r --arg id "$bottom_id" --arg other "$top_id" \
    '([.[] | select(.id == $id) | .y][0]) < ([.[] | select(.id == $other) | .y][0])')
  [[ $local_moved == true ]] && break
  sleep 0.1
done
if [[ $local_moved != true ]]; then
  echo "expected move-up variant to reorder rows before changing workspaces"
  exit 1
fi
accepts "window-close"
wait_for_windows 1

accepts "window-focus-or-workspace-down"
active_now=true
for _ in $(seq 40); do
  active_now=$("$UMBRIEL" windows --json | jq -r '.[0].active')
  [[ $active_now == false ]] && break
  sleep 0.1
done
if [[ $active_now != false ]]; then
  echo "expected focus-down variant to switch at the workspace boundary"
  exit 1
fi
accepts "window-focus-or-workspace-up"
for _ in $(seq 40); do
  active_now=$("$UMBRIEL" windows --json | jq -r '.[0].active')
  [[ $active_now == true ]] && break
  sleep 0.1
done
if [[ $active_now != true ]]; then
  echo "expected focus-up variant to return and restore focus"
  exit 1
fi

accepts "window-move-or-workspace-down"
for _ in $(seq 40); do
  returned_workspace=$("$UMBRIEL" windows --json | jq -r '.[0].workspace')
  [[ $returned_workspace == "$moved_workspace" ]] && break
  sleep 0.1
done
if [[ $returned_workspace != "$moved_workspace" ]]; then
  echo "expected move-down variant to cross the workspace boundary"
  exit 1
fi
accepts "window-move-or-workspace-up"
for _ in $(seq 40); do
  returned_workspace=$("$UMBRIEL" windows --json | jq -r '.[0].workspace')
  [[ $returned_workspace == "$start_workspace" ]] && break
  sleep 0.1
done
if [[ $returned_workspace != "$start_workspace" ]]; then
  echo "expected move-up variant to return to $start_workspace"
  exit 1
fi

# window-modify-width: Headless output is 1280x720 with the shipped defaults (gap 8, border 2): viewport 1260, so -0.2 shrinks a column by about 252px.
# The exact geometry math lives in 110_scrolling_layout.sh (624 wide at 0.5).
before_w=$(jq -r '.[0].w' <<< "$("$UMBRIEL" windows --json)")
accepts "window-modify-width:-0.2"
after_w=$before_w
for _ in $(seq 40); do
  after_w=$(jq -r '.[0].w' <<< "$("$UMBRIEL" windows --json)")
  [[ $after_w -le $((before_w - 200)) ]] && break
  sleep 0.1
done
if [[ $after_w -gt $((before_w - 200)) || $after_w -lt $((before_w - 320)) ]]; then
  echo "expected width to shrink by roughly 20% of the viewport (252), got $before_w -> $after_w"
  exit 1
fi

# A moved view from a stacked full-width column must carry the saved width that
# window-toggle-maximize restores, not the destination's default width.
narrow_w=$after_w
spawn_client stacked-move
wait_for_windows 2
stacked_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "stacked-move") | .id')
accepts "window-focus:$stacked_id"
accepts "window-consume-left"
stacked=false
for _ in $(seq 40); do
  if "$UMBRIEL" windows --json | jq -e --argjson want "$narrow_w" \
      'length == 2 and all(.[]; .w == $want) and ([.[].y] | unique | length == 2)' > /dev/null; then
    stacked=true
    break
  fi
  sleep 0.1
done
if [[ $stacked != true ]]; then
  echo "expected two rows in a stacked column of width $narrow_w"
  exit 1
fi

accepts "window-toggle-maximize"
maximized_w=$narrow_w
for _ in $(seq 40); do
  maximized_w=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "stacked-move") | .w')
  [[ $maximized_w -ge $((narrow_w + 400)) ]] && break
  sleep 0.1
done
if [[ $maximized_w -lt $((narrow_w + 400)) ]]; then
  echo "expected stacked column to become full width, got $maximized_w"
  exit 1
fi

accepts "window-move-to-workspace-next"
current_workspace=$start_workspace
moved_w=0
for _ in $(seq 40); do
  read -r current_workspace moved_w <<< "$("$UMBRIEL" windows --json \
    | jq -r '.[] | select(.title == "stacked-move") | "\(.workspace) \(.w)"')"
  [[ $current_workspace == "$moved_workspace" && $moved_w -eq $maximized_w ]] && break
  sleep 0.1
done
if [[ $current_workspace != "$moved_workspace" || $moved_w -ne $maximized_w ]]; then
  echo "expected full-width state $maximized_w on $moved_workspace, got $moved_w on $current_workspace"
  exit 1
fi

accepts "window-toggle-maximize"
restored_narrow_w=$moved_w
for _ in $(seq 40); do
  restored_narrow_w=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "stacked-move") | .w')
  [[ $restored_narrow_w -eq $narrow_w ]] && break
  sleep 0.1
done
if [[ $restored_narrow_w -ne $narrow_w ]]; then
  echo "expected stacked column's saved width $narrow_w, got $restored_narrow_w"
  exit 1
fi

accepts "window-move-to-workspace-previous"
for _ in $(seq 40); do
  current_workspace=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "stacked-move") | .workspace')
  [[ $current_workspace == "$start_workspace" ]] && break
  sleep 0.1
done
if [[ $current_workspace != "$start_workspace" ]]; then
  echo "expected stacked-move back on $start_workspace, got $current_workspace"
  exit 1
fi
accepts "window-close"
wait_for_windows 1

# The source column remained full width when one stacked row moved away.
accepts "window-toggle-maximize"
source_w=0
for _ in $(seq 40); do
  source_w=$("$UMBRIEL" windows --json | jq -r '.[0].w')
  [[ $source_w -eq $narrow_w ]] && break
  sleep 0.1
done
if [[ $source_w -ne $narrow_w ]]; then
  echo "expected source column to restore width $narrow_w, got $source_w"
  exit 1
fi
accepts "window-modify-width:+0.2"
restored_w=0
for _ in $(seq 40); do
  restored_w=$(jq -r '.[0].w' <<< "$("$UMBRIEL" windows --json)")
  [[ $restored_w -eq $before_w ]] && break
  sleep 0.1
done
if [[ $restored_w -ne $before_w ]]; then
  echo "expected width to restore to $before_w, got $restored_w"
  exit 1
fi

# window-center
accepts "window-toggle-floating"
for _ in $(seq 40); do
  floating=$(jq -r '.[0].floating' <<< "$("$UMBRIEL" windows --json)")
  [[ $floating == true ]] && break
  sleep 0.1
done
if [[ $floating != true ]]; then
  echo "window-toggle-floating did not float the window"
  exit 1
fi
w=$(jq -r '.[0].w' <<< "$("$UMBRIEL" windows --json)")
h=$(jq -r '.[0].h' <<< "$("$UMBRIEL" windows --json)")
accepts "window-center"
# No layers on the headless output, so the usable area is the whole 1280x720
# output and centering is exact modulo integer rounding and animation settle.
want_x=$(( (1280 - w) / 2 ))
want_y=$(( (720 - h) / 2 ))
cur_x=0
cur_y=0
for _ in $(seq 40); do
  read -r cur_x cur_y <<< "$(jq -r '.[0] | "\(.x) \(.y)"' <<< "$("$UMBRIEL" windows --json)")"
  dx=$((cur_x - want_x))
  dy=$((cur_y - want_y))
  [[ $dx -ge -3 && $dx -le 3 && $dy -ge -3 && $dy -le 3 ]] && break
  sleep 0.1
done
if [[ $dx -lt -3 || $dx -gt 3 || $dy -lt -3 || $dy -gt 3 ]]; then
  echo "expected the window centered at ($want_x, $want_y), got ($cur_x, $cur_y)"
  exit 1
fi
accepts "window-toggle-floating" # restore tiled

# workspace-set-layout
spawn_client dwindle-b
spawn_client dwindle-c
wait_for_windows 3

accepts "workspace-set-layout:dwindle"
min_h=720
for _ in $(seq 40); do
  min_h=$(jq '[.[].h] | min' <<< "$("$UMBRIEL" windows --json)")
  [[ $min_h -lt 600 ]] && break
  sleep 0.1
done
if [[ $min_h -ge 600 ]]; then
  echo "expected a vertical split under dwindle, min height $min_h"
  exit 1
fi

# column-center rejects over IPC outside scrolling without changing geometry.
sleep 0.5
dwindle_geometry=$("$UMBRIEL" windows --json | jq -c 'sort_by(.id) | map({id, x, y, w, h})')
rejects_with "column-center" "requires the scrolling layout"
sleep 0.2
after_center=$("$UMBRIEL" windows --json | jq -c 'sort_by(.id) | map({id, x, y, w, h})')
if [[ $after_center != "$dwindle_geometry" ]]; then
  echo "column-center changed dwindle geometry: $dwindle_geometry -> $after_center"
  exit 1
fi

# The runtime switch must survive a window open: reconcileDynamic re-resolves
# the configured layout, and the override keeps dwindle in force.
spawn_client dwindle-d
wait_for_windows 4
min_h=720
for _ in $(seq 40); do
  min_h=$(jq '[.[].h] | min' <<< "$("$UMBRIEL" windows --json)")
  [[ $min_h -lt 600 ]] && break
  sleep 0.1
done
if [[ $min_h -ge 600 ]]; then
  echo "expected dwindle to persist across a window open, min height $min_h"
  exit 1
fi

accepts "workspace-set-layout:scrolling"
min_h=0
for _ in $(seq 40); do
  min_h=$(jq '[.[].h] | min' <<< "$("$UMBRIEL" windows --json)")
  [[ $min_h -ge 690 ]] && break
  sleep 0.1
done
if [[ $min_h -lt 690 ]]; then
  echo "expected all windows back at full height under scrolling, min height $min_h"
  exit 1
fi

accepts "workspace-set-layout:toggle"
min_h=720
for _ in $(seq 40); do
  min_h=$(jq '[.[].h] | min' <<< "$("$UMBRIEL" windows --json)")
  [[ $min_h -lt 600 ]] && break
  sleep 0.1
done
if [[ $min_h -ge 600 ]]; then
  echo "expected toggle to flip back to dwindle, min height $min_h"
  exit 1
fi

echo "local and cross-workspace actions, width preservation, centering, and layout switching behave"
