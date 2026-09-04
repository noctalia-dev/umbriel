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
  if ! out=$("$UMBRIEL" msg "$1" 2>&1); then
    echo "expected '$1' to be accepted, got: $out"
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
accepts "workspace-switch:ONE/HEADLESS-1"

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
accepts "window-focus:$bottom_id"
accepts window-set-width:0.667
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

accepts "column-move-to-workspace:THREE/HEADLESS-1"
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
accepts "column-move-to-workspace:RIGHT_TWO/HEADLESS-2"
wait_for_workspace column-top "$right_two_id"
wait_for_workspace column-bottom "$right_two_id"
wait_for_workspace source-anchor "$one_id"
wait_for_column_geometry "$normal_width"
sleep 1

# The focused bottom member is not under the target output's center. A
# focus-only detour to the top member followed by an unmoved click therefore
# proves the cross-output transfer warped to the moved focused window.
top_id=$(field_of column-top id)
accepts "window-focus:$top_id"
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

echo "direct and adjacent actions moved a focused two-row column within and across outputs"
