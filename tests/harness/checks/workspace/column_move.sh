#!/usr/bin/env bash
# harness: outputs=2
# A workspace column move carries every stacked member as one unit, follows the
# focused member, and retains scrolling geometry across direct and adjacent moves.
set -euo pipefail

readonly BTN_LEFT=272
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}"

if [[ ! -x $POINTER ]]; then
  echo "pointer client not built at $POINTER"
  exit 1
fi

accepts() {
  if ! out=$("$UMBRIEL" msg "$@" 2>&1); then
    echo "expected '$*' to be accepted, got: $out"
    return 1
  fi
}

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_windows() {
  local expected=$1 count=
  for _ in $(seq 40); do
    count=$("$UMBRIEL" windows --json | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected window(s), got $count"
  return 1
}

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" \
    '.[] | select(.title == $title) | .[$field]'
}

wait_for_workspace() {
  local title=$1 expected=$2 actual=
  for _ in $(seq 50); do
    actual=$(field_of "$title" workspace)
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' on $expected, got $actual"
  return 1
}

wait_for_output() {
  local title=$1 output=$2 actual= workspace=
  for _ in $(seq 50); do
    workspace=$(field_of "$title" workspace)
    actual=$("$UMBRIEL" workspaces --json | jq -r --arg id "$workspace" ' .[] | select(.id == $id) | .output')
    [[ $actual == "$output" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' on $output, got $actual"
  return 1
}

workspace_id_named() {
  "$WORKSPACE" --all | awk -F'\t' -v name="$1" '$2 == name { print $1; exit }'
}

wait_for_column_geometry() {
  local expected_width=${1:-} windows=
  for _ in $(seq 50); do
    windows=$("$UMBRIEL" windows --json)
    if jq -e --arg width "$expected_width" '
      [.[] | select(.title == "column-top")] as $top
      | [.[] | select(.title == "column-bottom")] as $bottom
      | ($top | length == 1)
        and ($bottom | length == 1)
        and ($top[0].x == $bottom[0].x)
        and ($top[0].y < $bottom[0].y)
        and ($top[0].w == $bottom[0].w)
        and (($width == "") or ($top[0].w == ($width | tonumber)))
    ' <<< "$windows" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "column geometry did not settle as expected: $windows"
  return 1
}

printf '\n[output.HEADLESS-1]\nposition = [0, 0]\nworkspaces = ["ONE", "TWO", "THREE"]\n\n[output.HEADLESS-2]\nposition = [1280, 0]\nworkspaces = ["RIGHT_ONE", "RIGHT_TWO"]\n\n[input.cursor]\nfollows_focus = true\n' \
  >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
one_id=$(workspace_id_named ONE)
two_id=$(workspace_id_named TWO)
three_id=$(workspace_id_named THREE)
right_one_id=$(workspace_id_named RIGHT_ONE)
right_two_id=$(workspace_id_named RIGHT_TWO)
if [[ -z $one_id || -z $two_id || -z $three_id || -z $right_one_id || -z $right_two_id ]]; then
  echo "expected ids for all five configured workspaces"
  exit 1
fi
accepts workspace-switch ONE --output HEADLESS-1

# Build one two-row column plus an independent source column.
spawn_client column-top
wait_for_windows 1
spawn_client column-bottom
wait_for_windows 2
bottom_id=$(field_of column-bottom id)
accepts window-consume-left
wait_for_column_geometry

spawn_client source-anchor
wait_for_windows 3
accepts window-focus "$bottom_id"
accepts window-set-primary-extent 0.667
for _ in $(seq 50); do
  normal_width=$(field_of column-bottom w)
  [[ $normal_width -ge 800 && $normal_width -le 870 ]] && break
  sleep 0.1
done
if [[ $normal_width -lt 800 || $normal_width -gt 870 ]]; then
  echo "expected a nondefault column width near two thirds, got $normal_width"
  exit 1
fi
wait_for_column_geometry "$normal_width"

# Full-width state carries its normal restore width to the selected workspace.
accepts window-toggle-maximize
for _ in $(seq 50); do
  full_width=$(field_of column-bottom w)
  [[ $full_width -ge 1200 ]] && break
  sleep 0.1
done
if [[ $full_width -lt 1200 ]]; then
  echo "expected the source column to become full width, got $full_width"
  exit 1
fi

accepts column-move-to-workspace THREE --output HEADLESS-1
wait_for_workspace column-top "$three_id"
wait_for_workspace column-bottom "$three_id"
wait_for_workspace source-anchor "$one_id"
wait_for_column_geometry "$full_width"
if [[ $(field_of column-bottom active) != true ]]; then
  echo "expected focus to follow the moved column to THREE"
  exit 1
fi

accepts window-toggle-maximize
wait_for_column_geometry "$normal_width"

# Previous and next retain the whole column, member order, width, and focus.
accepts column-move-to-workspace-previous
wait_for_workspace column-top "$two_id"
wait_for_workspace column-bottom "$two_id"
wait_for_workspace source-anchor "$one_id"
wait_for_column_geometry "$normal_width"
if [[ $(field_of column-bottom active) != true ]]; then
  echo "expected focus to follow the moved column to TWO"
  exit 1
fi

accepts column-move-to-workspace-next
wait_for_workspace column-top "$three_id"
wait_for_workspace column-bottom "$three_id"
wait_for_workspace source-anchor "$one_id"
wait_for_column_geometry "$normal_width"

# The next action at the final workspace is a silent no-op.
accepts column-move-to-workspace-next
wait_for_workspace column-top "$three_id"
wait_for_workspace column-bottom "$three_id"

# A qualified selector follows the column across outputs. The immediately
# following adjacent action must therefore resolve on the destination output.
accepts column-move-to-workspace RIGHT_TWO --output HEADLESS-2
wait_for_workspace column-top "$right_two_id"
wait_for_workspace column-bottom "$right_two_id"
wait_for_workspace source-anchor "$one_id"
wait_for_column_geometry "$normal_width"
"$UMBRIEL" settle

# The focused bottom member is not under the target output's center. Turn off
# follow-warp without moving the cursor, then use a focus-only detour to the top
# member. An unmoved click proves the cross-output transfer warped to the moved
# focused window.
sed -i 's/follows_focus = true/follows_focus = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
top_id=$(field_of column-top id)
accepts window-focus "$top_id"
"$POINTER" 2560 720 click "$BTN_LEFT"
for _ in $(seq 40); do
  [[ $(field_of column-bottom focused) == true ]] && break
  sleep 0.1
done
if [[ $(field_of column-bottom focused) != true ]]; then
  echo "expected the cursor to follow the focused column member across outputs"
  exit 1
fi

accepts column-move-to-workspace-previous
wait_for_workspace column-top "$right_one_id"
wait_for_workspace column-bottom "$right_one_id"
wait_for_workspace source-anchor "$one_id"
wait_for_column_geometry "$normal_width"

# A reconstructed column retains multiple groups, including the selection of the
# group that does not hold workspace focus, and each group's bar override.
spawn_client transfer-c
wait_for_windows 4
c_id=$(field_of transfer-c id)
accepts window-consume-left
accepts column-set-display tabbed
accepts window-move-down
accepts window-focus "$bottom_id"
accepts column-hide-tab-bar
accepts window-focus "$c_id"
accepts column-set-display tabbed
accepts column-show-tab-bar
spawn_client transfer-d
wait_for_windows 5
d_id=$(field_of transfer-d id)
accepts window-consume-left
"$UMBRIEL" settle

transfer_state() {
  "$UMBRIEL" windows --json | jq -c '
    [.[] | select(.title == "column-top" or .title == "column-bottom"
      or .title == "transfer-c" or .title == "transfer-d")]
    | sort_by(.title)
    | map({title, y, h, tabbed, tab_index, tab_hidden})'
}

assert_transfer_groups() {
  local windows
  "$UMBRIEL" settle
  windows=$("$UMBRIEL" windows --json)
  if ! jq -e '
    [.[] | select(.title == "column-top")][0] as $a
    | [.[] | select(.title == "column-bottom")][0] as $b
    | [.[] | select(.title == "transfer-c")][0] as $c
    | [.[] | select(.title == "transfer-d")][0] as $d
    | $a.tabbed and $b.tabbed and $c.tabbed and $d.tabbed
      and $a.tab_hidden and ($b.tab_hidden | not)
      and $c.tab_hidden and ($d.tab_hidden | not) and $d.active
      and $a.tab_index == 0 and $b.tab_index == 1
      and $c.tab_index == 0 and $d.tab_index == 1
      and ([$a.x, $a.y, $a.w, $a.h] == [$b.x, $b.y, $b.w, $b.h])
      and ([$c.x, $c.y, $c.w, $c.h] == [$d.x, $d.y, $d.w, $d.h])
      and $b.x == $d.x and $b.y < $d.y
  ' <<< "$windows" > /dev/null; then
    echo "expected two transferred groups showing column-bottom and transfer-d: $windows"
    exit 1
  fi
  if [[ $(transfer_state) != "$groups_before" ]]; then
    echo "transfer changed group selections, bar reserves, or row geometry: $(transfer_state)"
    exit 1
  fi
}

groups_before=$(transfer_state)
# Reloading the default affects new columns only. Rebuilding must replace the
# destination's seeded one-tab group with the source's display, not join it.
printf '\n[layout.tabs]\ndefault_display = "tabbed"\n' >> "$UMBRIEL_CONFIG"
accepts config-reload
accepts column-move-to-workspace TWO --output HEADLESS-1
wait_for_workspace transfer-d "$two_id"
assert_transfer_groups
accepts column-move-to-output-right
wait_for_workspace transfer-d "$right_one_id"
assert_transfer_groups

# Whole-workspace output moves preserve all columns on the requested output.
accepts workspace-move-to-output-left
wait_for_output transfer-d HEADLESS-1
transferred_workspace=$(field_of transfer-d workspace)
for title in column-top column-bottom transfer-c; do
  wait_for_workspace "$title" "$transferred_workspace"
done
assert_transfer_groups
wait_for_workspace source-anchor "$one_id"
accepts column-move-to-workspace THREE --output HEADLESS-1
wait_for_workspace transfer-d "$three_id"

# A master area can retain a whole-column group and its hidden bar, but not
# separate groups inside one area. Remove the second group to make it representable.
accepts window-focus "$d_id"
accepts window-close
wait_for_windows 4
accepts window-focus "$c_id"
accepts window-close
wait_for_windows 3
accepts window-focus "$bottom_id"
"$UMBRIEL" settle
whole_group_top=$(field_of column-bottom y)
accepts workspace-switch TWO --output HEADLESS-1
accepts workspace-set-layout master
accepts workspace-switch THREE --output HEADLESS-1
accepts column-move-to-workspace TWO --output HEADLESS-1
wait_for_workspace column-bottom "$two_id"
"$UMBRIEL" settle
if ! "$UMBRIEL" windows --json | jq -e --argjson top "$whole_group_top" '
  [.[] | select(.title == "column-top")][0] as $a
  | [.[] | select(.title == "column-bottom")][0] as $b
  | $a.tabbed and $b.tabbed and $a.tab_hidden and ($b.tab_hidden | not)
    and $b.active and $a.tab_index == 0 and $b.tab_index == 1
    and $b.y == $top
    and ([$a.x, $a.y, $a.w, $a.h] == [$b.x, $b.y, $b.w, $b.h])
' > /dev/null; then
  echo "master transfer lost the whole-area group, selected tab, or hidden bar"
  exit 1
fi

# Rebuilding a whole master workspace preserves both areas, rather than letting
# new-window insertion policy demote a row out of an already rebuilt group.
printf '\n[layout.master]\nnew_becomes_master = false\n' >> "$UMBRIEL_CONFIG"
accepts config-reload
spawn_client master-extra
wait_for_windows 4
extra_id=$(field_of master-extra id)
accepts window-focus "$bottom_id"
"$UMBRIEL" settle
master_state() {
  "$UMBRIEL" windows --json | jq -c '
    [.[] | select(.title == "column-top" or .title == "column-bottom" or .title == "master-extra")]
    | sort_by(.title) | map({title, y, w, h, tabbed, tab_index, tab_hidden, active})'
}
master_before=$(master_state)
accepts workspace-switch RIGHT_TWO --output HEADLESS-2
accepts workspace-set-layout master
accepts workspace-switch TWO --output HEADLESS-1
accepts workspace-move-to-output-right
wait_for_output column-bottom HEADLESS-2
transferred_workspace=$(field_of column-bottom workspace)
wait_for_workspace column-top "$transferred_workspace"
wait_for_workspace master-extra "$transferred_workspace"
"$UMBRIEL" settle
if [[ $(master_state) != "$master_before" ]]; then
  echo "whole-workspace output transfer changed master areas, tabs, bar reserve, or selection: $(master_state)"
  exit 1
fi
# The reverse transfer preserves the master layout on a scrolling output.
accepts workspace-switch THREE --output HEADLESS-1
accepts workspace-set-layout scrolling
accepts window-focus-warp "$bottom_id"
accepts workspace-move-to-output-left
wait_for_output column-bottom HEADLESS-1
transferred_workspace=$(field_of column-bottom workspace)
wait_for_workspace column-top "$transferred_workspace"
wait_for_workspace master-extra "$transferred_workspace"
accepts window-focus "$extra_id"
accepts window-close
wait_for_windows 3
accepts window-focus "$bottom_id"
accepts column-move-to-workspace THREE --output HEADLESS-1
wait_for_workspace column-bottom "$three_id"
accepts workspace-switch RIGHT_TWO --output HEADLESS-2
accepts workspace-set-layout scrolling
accepts workspace-switch THREE --output HEADLESS-1

# A normal source stays normal even when the destination creates tabbed areas.
accepts column-set-display normal
accepts column-move-to-output-right
wait_for_workspace column-bottom "$right_two_id"
"$UMBRIEL" settle
if [[ $(field_of column-top tabbed) != false || $(field_of column-bottom tabbed) != false ]]; then
  echo "destination tabbed default changed a normal source column's display"
  exit 1
fi
accepts workspace-switch TWO --output HEADLESS-1
accepts workspace-set-layout master
accepts workspace-switch RIGHT_TWO --output HEADLESS-2
accepts column-move-to-workspace TWO --output HEADLESS-1
wait_for_workspace column-bottom "$two_id"
"$UMBRIEL" settle
if [[ $(field_of column-top tabbed) != false || $(field_of column-bottom tabbed) != false ]]; then
  echo "destination tabbed default changed a normal source master area"
  exit 1
fi

# Dwindle cannot represent groups and keeps every transferred member visible.
accepts column-set-display tabbed
accepts workspace-switch RIGHT_TWO --output HEADLESS-2
accepts workspace-set-layout dwindle
accepts workspace-switch TWO --output HEADLESS-1
accepts column-move-to-workspace RIGHT_TWO --output HEADLESS-2
wait_for_workspace column-top "$right_two_id"
wait_for_workspace column-bottom "$right_two_id"
"$UMBRIEL" settle
if [[ $(field_of column-top tabbed) != false || $(field_of column-bottom tabbed) != false \
  || $(field_of column-top tab_hidden) != false || $(field_of column-bottom tab_hidden) != false ]]; then
  echo "dwindle did not flatten a transferred group into visible leaves"
  exit 1
fi

echo "column and workspace transfers retained geometry, groups, selections, and display overrides"
