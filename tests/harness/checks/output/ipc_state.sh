#!/usr/bin/env bash
# harness: outputs=2
# The output listing reports compositor state output management cannot carry (focus, power, the active workspace, the
# usable area), and each change to it is pushed to subscribers.
set -euo pipefail

readonly LAYER_CLIENT="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly EVENTS="$UMBRIEL_RUNTIME_DIR/output-ipc-events.jsonl"

printf '\n[output.HEADLESS-1]\nworkspaces = 2\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" subscribe outputs > "$EVENTS" &

# Waits until the newest pushed outputs event satisfies a jq predicate on its data.
await_outputs() {
  local predicate=$1 reason=$2
  for _ in $(seq 60); do
    if jq -e -s "[.[] | select(.event == \"outputs\")] | last | .data | $predicate" "$EVENTS" > /dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "$reason: the newest outputs event is $(jq -c -s '[.[] | select(.event == "outputs")] | last | .data' \
    "$EVENTS" 2> /dev/null)"
  exit 1
}

action() {
  "$UMBRIEL" msg "$@" > /dev/null
}

active_workspace() {
  "$UMBRIEL" workspaces --json | jq -r --arg output "$1" '.[] | select(.output == $output and .active) | .id'
}

first_workspace=$(active_workspace HEADLESS-1)
await_outputs "
  length == 2
  and ([.[] | select(.focused)] | length == 1)
  and all(.[]; .enabled and .powered and (.hdr_active | not)
    and .usable_area == {x: .position.x, y: .position.y, width: .logical_size.width, height: .logical_size.height})
  and (.[] | select(.name == \"HEADLESS-1\") | .active_workspace == \"$first_workspace\")
" "the initial outputs lack focus, power, workspace, or area state"

# The listing answers the query with the same payload the stream carries.
if ! diff <("$UMBRIEL" outputs --json | jq -S .) \
  <(jq -s -S '[.[] | select(.event == "outputs")] | last | .data' "$EVENTS") > /dev/null; then
  echo "outputs --json differs from the pushed snapshot: $("$UMBRIEL" outputs --json)"
  exit 1
fi

focused=$("$UMBRIEL" outputs --json | jq -r '.[] | select(.focused) | .name')
action output-focus-next
await_outputs "[.[] | select(.focused) | .name] == [(.[] | select(.name != \"$focused\") | .name)]" \
  "focusing the next output"

action workspace-switch:2/HEADLESS-1
second_workspace=$(active_workspace HEADLESS-1)
if [[ $second_workspace == "$first_workspace" ]]; then
  echo "workspace-switch did not change the active workspace of HEADLESS-1"
  exit 1
fi
await_outputs ".[] | select(.name == \"HEADLESS-1\") | .active_workspace == \"$second_workspace\"" \
  "switching workspaces"

# An exclusive panel shrinks the usable area.
"$LAYER_CLIENT" HEADLESS-1 40 > "$UMBRIEL_RUNTIME_DIR/output-ipc-panel.log" 2>&1 &
await_outputs '
  .[] | select(.name == "HEADLESS-1")
    | .usable_area.y == .position.y + 40 and .usable_area.height == .logical_size.height - 40
' "an exclusive panel did not shrink the usable area"

action dpms-off:HEADLESS-2
await_outputs '.[] | select(.name == "HEADLESS-2") | .enabled and (.powered | not)' "powering an output off"
action dpms-on:HEADLESS-2
await_outputs '.[] | select(.name == "HEADLESS-2") | .powered' "powering an output on"

action output-disable:HEADLESS-2
await_outputs '
  .[] | select(.name == "HEADLESS-2")
    | (.enabled | not) and (.powered | not) and .logical_size == {width: 0, height: 0}
' "disabling an output"
action output-enable:HEADLESS-2
await_outputs '.[] | select(.name == "HEADLESS-2") | .enabled and .logical_size.width > 0' "enabling an output"

# A virtual output appears with its custom mode as the current one, and leaves when destroyed.
"$UMBRIEL" output-create ipc-virtual 800x600@30 > /dev/null
await_outputs '
  .[] | select(.name == "ipc-virtual")
    | [.modes[] | select(.current) | [.width, .height, .refresh_mhz]] == [[800, 600, 30000]]
' "the virtual output did not appear with its mode"
"$UMBRIEL" output-destroy ipc-virtual > /dev/null
await_outputs 'all(.[]; .name != "ipc-virtual")' "the destroyed virtual output stayed listed"

echo "output focus, power, workspace, area, and inventory are listed and pushed to subscribers"
