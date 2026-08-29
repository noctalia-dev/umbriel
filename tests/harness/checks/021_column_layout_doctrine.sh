#!/usr/bin/env bash
# Column actions remain available through each layout's projection, while column-center reports the one layout-specific rejection over IPC.
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

accepts "column-center"

accepts "workspace-set-layout:dwindle"
rejects_with "column-center" "requires the scrolling layout"

accepts "workspace-set-layout:master"
rejects_with "column-center" "requires the scrolling layout"
for action in \
  column-move-left column-move-right column-move-to-first column-move-to-last \
  column-focus-first column-focus-last column-move-to-workspace-next; do
  accepts "$action"
done
accepts "workspace-set-layout:scrolling"

accepts "workspace-set-layout:dwindle"
spawn_client col-a
wait_for_windows 1
spawn_client col-b
wait_for_windows 2

read -r a_x a_y <<< "$("$UMBRIEL" windows --json | jq -er '.[] | select(.title == "col-a") | "\(.x) \(.y)"')"
read -r b_x b_y <<< "$("$UMBRIEL" windows --json | jq -er '.[] | select(.title == "col-b") | "\(.x) \(.y)"')"

move_action=column-move-right
if (( b_x > a_x || (b_x == a_x && b_y > a_y) )); then
  move_action=column-move-left
fi
accepts "$move_action"

swapped=false
for _ in $(seq 40); do
  windows=$("$UMBRIEL" windows --json)
  if jq -e \
      --argjson ax "$a_x" --argjson ay "$a_y" --argjson bx "$b_x" --argjson by "$b_y" \
      '([.[] | select(.title == "col-a") | [.x, .y]][0] == [$bx, $by]) and
       ([.[] | select(.title == "col-b") | [.x, .y]][0] == [$ax, $ay])' \
      <<< "$windows" > /dev/null; then
    swapped=true
    break
  fi
  sleep 0.1
done

if [[ $swapped != true ]]; then
  echo "expected $move_action to exchange the dwindle window positions"
  "$UMBRIEL" windows --json
  exit 1
fi
