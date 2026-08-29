#!/usr/bin/env bash
# Directional consume actions change column membership, height actions resize
# stacked windows, and focus-last follows global MRU order across workspaces.
set -euo pipefail

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

wait_for_stacked() {
  local windows=
  for _ in $(seq 50); do
    windows=$("$UMBRIEL" windows --json)
    if jq -e '
      [.[] | select(.title == "harness-a")] as $a
      | [.[] | select(.title == "harness-b")] as $b
      | ($a | length == 1)
        and ($b | length == 1)
        and ($a[0].x == $b[0].x)
        and ($a[0].y != $b[0].y)
    ' <<< "$windows" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected harness-a and harness-b to share one stacked column: $windows"
  return 1
}

wait_for_separate_columns() {
  local windows=
  for _ in $(seq 50); do
    windows=$("$UMBRIEL" windows --json)
    if jq -e '
      [.[] | select(.title == "harness-a")] as $a
      | [.[] | select(.title == "harness-b")] as $b
      | ($a | length == 1)
        and ($b | length == 1)
        and ($a[0].x != $b[0].x)
    ' <<< "$windows" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected harness-a and harness-b in separate columns: $windows"
  return 1
}

wait_for_b_height_percent() {
  local minimum=$1 maximum=$2 windows=
  for _ in $(seq 50); do
    windows=$("$UMBRIEL" windows --json)
    if jq -e --argjson minimum "$minimum" --argjson maximum "$maximum" '
      [.[] | select(.title == "harness-a")] as $a
      | [.[] | select(.title == "harness-b")] as $b
      | ($a | length == 1)
        and ($b | length == 1)
        and ($b[0].h * 100 >= $minimum * ($a[0].h + $b[0].h))
        and ($b[0].h * 100 <= $maximum * ($a[0].h + $b[0].h))
    ' <<< "$windows" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected harness-b height between $minimum% and $maximum% of its stack: $windows"
  return 1
}

wait_for_focus() {
  local title=$1 active=
  for _ in $(seq 50); do
    active=$(field_of "$title" active)
    [[ $active == true ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' to become active, got $active"
  return 1
}

spawn_client harness-a
wait_for_windows 1
spawn_client harness-b
wait_for_windows 2

accepts window-consume-or-expel-left
wait_for_stacked

accepts window-set-height:0.7
wait_for_b_height_percent 66 74

accepts window-modify-height:-0.2
wait_for_b_height_percent 46 54

accepts window-consume-or-expel-left
wait_for_separate_columns

accepts window-consume-right
wait_for_stacked

accepts window-consume-or-expel-right
wait_for_separate_columns
b_workspace=$(field_of harness-b workspace)

accepts workspace-switch:2
spawn_client harness-c
wait_for_windows 3
c_workspace=$(field_of harness-c workspace)
if [[ -z $b_workspace || -z $c_workspace || $b_workspace == "$c_workspace" ]]; then
  echo "expected harness-b and harness-c on distinct workspaces, got '$b_workspace' and '$c_workspace'"
  exit 1
fi

accepts window-focus-last
wait_for_focus harness-b
if [[ $(field_of harness-b workspace) != "$b_workspace" ]]; then
  echo "expected focus-last to return to harness-b's original workspace"
  exit 1
fi

accepts window-focus-last
wait_for_focus harness-c
if [[ $(field_of harness-c workspace) != "$c_workspace" ]]; then
  echo "expected repeated focus-last to return to harness-c's workspace"
  exit 1
fi

echo "height, directional consume, and global focus-last actions behaved end to end"
