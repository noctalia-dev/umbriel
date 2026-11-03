#!/usr/bin/env bash
# harness: outputs=2
# Layout edits made while an output is absent belong to its temporary workspace,
# including column topology, widths, and a runtime switch to another layout.
set -euo pipefail

readonly CLIENT="$UMBRIEL_UNMAP_CLIENT"
readonly OFFSET=1280

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[output.HEADLESS-1]
position = [0, 0]
workspaces = ["origin"]

[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["local"]
EOF
"$UMBRIEL" msg config-reload > /dev/null

field() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" \
    '.[] | select(.title == $title) | .[$field]'
}

focus() {
  "$UMBRIEL" msg window-focus "$(field "$1" id)" > /dev/null
}

spawn() {
  local title=$1 count=$2 id
  "$UMBRIEL" msg workspace-switch origin --output HEADLESS-1 > /dev/null
  RESIZE_FILL_COLOR=0xFF5577AA "$CLIENT" "$title" 800 600 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') == "$count" ]] && break
    sleep 0.05
  done
  id=$(field "$title" id)
  if [[ -z $id ]]; then
    echo "window '$title' did not map: $(< "$UMBRIEL_RUNTIME_DIR/$title.log")"
    return 1
  fi
  "$UMBRIEL" msg window-focus "$id" > /dev/null
  "$UMBRIEL" msg window-move-to-workspace origin --output HEADLESS-1 > /dev/null
}

snapshot() {
  "$UMBRIEL" windows --json | jq -c --argjson offset "$1" '
    [.[] | {title, column, row, x: (.x - $offset), y, w, h}] | sort_by(.title)
  '
}

select_refuge() {
  local workspace index
  workspace=$(field edit-c workspace)
  index=$("$UMBRIEL" workspaces --json | jq -r --arg id "$workspace" '.[] | select(.id == $id) | .index')
  if [[ $workspace != HEADLESS-2:* || -z $index ]]; then
    echo "source windows did not enter a temporary workspace: $("$UMBRIEL" windows --json)"
    return 1
  fi
  "$UMBRIEL" msg workspace-switch "$index" --output HEADLESS-2 > /dev/null
  focus edit-c
  "$UMBRIEL" settle
}

assert_returned_layout() {
  local label=$1 expected=$2 mode=$3 windows workspace state actual
  "$UMBRIEL" msg workspace-switch origin --output HEADLESS-1 > /dev/null
  focus edit-c
  "$UMBRIEL" settle
  windows=$("$UMBRIEL" windows --json)
  workspace=$(field edit-c workspace)
  state=$("$UMBRIEL" workspaces --json)
  if ! jq -e --arg workspace "$workspace" '
    length == 3 and all(.[]; .workspace == $workspace and (.workspace | startswith("HEADLESS-1:")))
  ' <<< "$windows" > /dev/null || \
      ! jq -e --arg workspace "$workspace" --arg mode "$mode" '
        any(.[]; .id == $workspace and .layout == $mode)
      ' <<< "$state" > /dev/null; then
    echo "$label did not return its windows and current layout mode: $windows $state"
    return 1
  fi
  actual=$(snapshot 0)
  if [[ $actual != "$expected" ]]; then
    echo "$label lost the revised temporary layout: expected $expected, got $actual"
    return 1
  fi
}

spawn edit-a 1
spawn edit-b 2
spawn edit-c 3
"$UMBRIEL" settle
readonly ORIGINAL="$(snapshot 0)"

"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
select_refuge

# Turn three independent columns into c,[b,a] after the home is already absent.
"$UMBRIEL" msg column-move-to-first > /dev/null
focus edit-b
"$UMBRIEL" msg window-consume-left > /dev/null
"$UMBRIEL" msg window-move-up > /dev/null
focus edit-c
"$UMBRIEL" msg window-set-primary-extent 0.67 > /dev/null
"$UMBRIEL" settle
readonly REVISED="$(snapshot "$OFFSET")"
if [[ $REVISED == "$ORIGINAL" ]] || ! jq -e '
  first(.[] | select(.title == "edit-a")) as $a |
  first(.[] | select(.title == "edit-b")) as $b |
  first(.[] | select(.title == "edit-c")) as $c |
  $c.column == 0 and $a.column == 1 and $b.column == 1 and
  $c.x < $a.x and $a.x == $b.x and $b.y < $a.y and $c.w > $a.w
' <<< "$REVISED" > /dev/null; then
  echo "temporary scrolling edits did not establish revised topology and width: $REVISED"
  exit 1
fi

"$UMBRIEL" output-create HEADLESS-1 > /dev/null
assert_returned_layout scrolling "$REVISED" scrolling

for mode in master dwindle; do
  "$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
  select_refuge
  "$UMBRIEL" msg workspace-set-layout "$mode" > /dev/null
  "$UMBRIEL" msg window-set-primary-extent 0.63 > /dev/null
  "$UMBRIEL" settle
  workspace=$(field edit-c workspace)
  state=$("$UMBRIEL" workspaces --json)
  if ! jq -e --arg workspace "$workspace" --arg mode "$mode" '
    any(.[]; .id == $workspace and .layout == $mode and .layout_override)
  ' <<< "$state" > /dev/null; then
    echo "temporary workspace did not accept the $mode runtime override: $state"
    exit 1
  fi
  revised=$(snapshot "$OFFSET")
  "$UMBRIEL" output-create HEADLESS-1 > /dev/null
  assert_returned_layout "$mode runtime override" "$revised" "$mode"
done

echo "temporary workspace topology, widths, and runtime master and dwindle edits survived reconnection"
