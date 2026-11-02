#!/usr/bin/env bash
# harness: outputs=3
# Dynamic declarations cannot absorb named refugees, even when two lost outputs
# and a native declaration use the same name. Saved homes disambiguate them.
set -euo pipefail

readonly BASELINE="$(< "$UMBRIEL_CONFIG")"
readonly SETTINGS='
[animation]
enabled = false

[output.HEADLESS-1]
position = [0, 0]
workspaces = ["shared", "unique"]

[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["shared"]

[output.HEADLESS-3]
position = [2560, 0]
workspaces = "dynamic"

[[workspace]]
output = "HEADLESS-3"
name = "local"

[[window_rule]]
match.title = "^route-shared$"
default_output = "HEADLESS-1"
default_workspace = "shared"

[[window_rule]]
match.title = "^route-unique$"
default_workspace = "unique"'

write_config() {
  printf '%s\n%s\n' "$BASELINE" "$SETTINGS" > "$UMBRIEL_CONFIG"
  if [[ $1 == native ]]; then
    cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[workspace]]
output = "HEADLESS-3"
name = "shared"
EOF
  fi
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" settle
}

field() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" \
    '.[] | select(.title == $title) | .[$field]'
}

spawn() {
  local title=$1 selector=$2 count=$3 place=${4:-true} id
  local -a words
  read -ra words <<< "$selector"
  "$UMBRIEL" msg workspace-switch "${words[@]}" > /dev/null
  "$UMBRIEL_UNMAP_CLIENT" "$title" 800 600 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') == "$count" ]] && break
    sleep 0.05
  done
  id=$(field "$title" id)
  if [[ -z $id ]]; then
    echo "window '$title' did not map: $(< "$UMBRIEL_RUNTIME_DIR/$title.log")"
    return 1
  fi
  if [[ $place == true ]]; then
    "$UMBRIEL" msg window-focus "$id" > /dev/null
    "$UMBRIEL" msg window-move-to-workspace "${words[@]}" > /dev/null
  fi
}

expect_selected() {
  local selector=$1 expected=$2 workspaces
  local -a words
  read -ra words <<< "$selector"
  "$UMBRIEL" msg workspace-switch "${words[@]}" > /dev/null
  workspaces=$("$UMBRIEL" workspaces --json)
  if ! jq -e --arg expected "$expected" 'any(.[]; .id == $expected and .focused)' \
      <<< "$workspaces" > /dev/null; then
    echo "selector '$selector' did not focus workspace $expected: $workspaces"
    return 1
  fi
}

expect_ambiguous() {
  local selector=$1 response
  local -a words
  read -ra words <<< "$selector"
  if response=$("$UMBRIEL" msg workspace-switch "${words[@]}" 2>&1); then
    echo "ambiguous refugee name '$selector' unexpectedly selected a workspace"
    return 1
  fi
  if [[ $response != *ambiguous* ]]; then
    echo "ambiguous refugee name '$selector' returned an unrelated error: $response"
    return 1
  fi
}

expect_refuges() {
  local phase=$1 workspaces
  workspaces=$("$UMBRIEL" workspaces --json)
  if [[ $FIRST == "$SECOND" || $FIRST == "$LOCAL" || $SECOND == "$LOCAL" ]] || \
      ! jq -e --arg first "$FIRST" --arg second "$SECOND" --arg unique "$UNIQUE" --arg local "$LOCAL" '
        any(.[]; .id == $first and .output == "HEADLESS-3" and .name == "shared" and .named) and
        any(.[]; .id == $second and .output == "HEADLESS-3" and .name == "shared" and .named) and
        any(.[]; .id == $unique and .output == "HEADLESS-3" and .name == "unique" and .named) and
        any(.[]; .id == $local and .name == "local" and .named)
      ' <<< "$workspaces" > /dev/null; then
    echo "$phase merged, renamed, or replaced a refugee or native workspace: $workspaces"
    return 1
  fi
  expect_selected 'shared --output HEADLESS-1' "$FIRST"
  expect_selected 'shared --output HEADLESS-2' "$SECOND"
  expect_selected unique "$UNIQUE"
}

scratchpad_round_trip() {
  local title=$1 expected=$2 pruned=${3:-false} actual workspaces
  "$UMBRIEL" msg window-focus "$(field "$title" id)" > /dev/null
  "$UMBRIEL" msg window-move-to-scratchpad > /dev/null
  "$UMBRIEL" settle
  if [[ $pruned == true ]] && "$UMBRIEL" workspaces --json | \
      jq -e --arg expected "$expected" 'any(.[]; .id == $expected)' > /dev/null; then
    echo "last-view scratchpad admission did not prune its emptied refugee"
    return 1
  fi
  "$UMBRIEL" msg window-focus "$(field "$title" id)" > /dev/null
  "$UMBRIEL" msg window-restore-from-scratchpad > /dev/null
  "$UMBRIEL" settle
  actual=$(field "$title" workspace)
  workspaces=$("$UMBRIEL" workspaces --json)
  if [[ $pruned == false && $actual != "$expected" ]] || \
      [[ $actual == "$native" ]] || ! jq -e --arg id "$actual" \
        'any(.[]; .id == $id and .output == "HEADLESS-3" and .name == "shared" and .named)' \
        <<< "$workspaces" > /dev/null; then
    echo "scratchpad return for '$title' chose a same-name native or unrelated refugee: $("$UMBRIEL" windows --json) $workspaces"
    return 1
  fi
}

write_config refugees
spawn first 'shared --output HEADLESS-1' 1
spawn first-peer 'shared --output HEADLESS-1' 2
spawn unique 'unique --output HEADLESS-1' 3
spawn second 'shared --output HEADLESS-2' 4
spawn local 'local --output HEADLESS-3' 5
readonly LOCAL="$(field local workspace)"

"$UMBRIEL" output-destroy HEADLESS-2 > /dev/null
"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
"$UMBRIEL" settle
readonly FIRST="$(field first workspace)"
SECOND="$(field second workspace)"
readonly UNIQUE="$(field unique workspace)"
expect_refuges removal
expect_ambiguous shared
expect_ambiguous 'shared --output HEADLESS-3'

write_config refugees
expect_refuges dynamic-reload
expect_ambiguous shared

# Both scoped and global opening rules must resolve to the evacuated container,
# rather than a missing output or a newly created same-name native workspace.
spawn route-shared 'local --output HEADLESS-3' 6 false
spawn route-unique 'local --output HEADLESS-3' 7 false
if [[ $(field route-shared workspace) != "$FIRST" || $(field route-unique workspace) != "$UNIQUE" ]]; then
  echo "opening rules did not resolve the displaced named workspaces: $("$UMBRIEL" windows --json)"
  exit 1
fi

write_config native
expect_refuges add-native-declaration
native=$("$UMBRIEL" workspaces --json | jq -r --arg first "$FIRST" --arg second "$SECOND" \
  '.[] | select(.output == "HEADLESS-3" and .name == "shared" and .id != $first and .id != $second) | .id')
if [[ -z $native || $native == *$'\n'* ]]; then
  echo "same-name native declaration did not get one independent workspace: $("$UMBRIEL" workspaces --json)"
  exit 1
fi
expect_selected shared "$native"
expect_selected 'shared --output HEADLESS-3' "$native"
scratchpad_round_trip first "$FIRST"
scratchpad_round_trip second "$SECOND" true
SECOND=$(field second workspace)
expect_refuges scratchpad-return

write_config refugees
expect_refuges remove-native-declaration
expect_ambiguous shared
expect_ambiguous 'shared --output HEADLESS-3'
if "$UMBRIEL" workspaces --json | jq -e --arg native "$native" 'any(.[]; .id == $native)' > /dev/null; then
  echo "removed native declaration survived as a duplicate named member"
  exit 1
fi

"$UMBRIEL" msg window-focus "$(field first id)" > /dev/null
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
"$UMBRIEL" output-create HEADLESS-1 > /dev/null
"$UMBRIEL" msg window-focus "$(field first id)" > /dev/null
"$UMBRIEL" msg window-restore-from-scratchpad > /dev/null
if [[ $(field first workspace) != "$(field first-peer workspace)" ]]; then
  echo "scratchpad return ignored the original workspace after its output reconnected: $("$UMBRIEL" windows --json)"
  exit 1
fi
"$UMBRIEL" output-create HEADLESS-2 > /dev/null
"$UMBRIEL" settle
windows=$("$UMBRIEL" windows --json)
if ! jq -e --arg local "$LOCAL" '
  all(.[];
    if .title == "local" then .workspace == $local
    elif .title == "second" then .workspace | startswith("HEADLESS-2:")
    else .workspace | startswith("HEADLESS-1:") end)
' <<< "$windows" > /dev/null; then
  echo "same-name workspaces or rule-routed arrivals returned to the wrong origin: $windows"
  exit 1
fi

echo "dynamic declarations, selectors, opening rules, and scratchpad returns kept same-name refugees independent"
