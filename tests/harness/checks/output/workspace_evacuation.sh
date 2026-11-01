#!/usr/bin/env bash
# harness: outputs=2
# Output loss preserves each populated workspace as its own temporary workspace,
# including its layout, without changing an occupied or empty native workspace.
set -euo pipefail

readonly CLIENT="$UMBRIEL_UNMAP_CLIENT"
readonly OFFSET=1280

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[output.HEADLESS-1]
position = [0, 0]
workspaces = ["code", "web"]

[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["code", "local"]

[[workspace]]
output = "HEADLESS-1"
name = "web"
layout.mode = "dwindle"
EOF
readonly CONFIG="$(< "$UMBRIEL_CONFIG")"
"$UMBRIEL" msg config-reload > /dev/null

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" \
    '.[] | select(.title == $title) | .[$field]'
}

spawn() {
  local title=$1 selector=$2 expected=$3 id=
  local -a words
  read -ra words <<< "$selector"
  "$UMBRIEL" msg workspace-switch "${words[@]}" > /dev/null
  RESIZE_FILL_COLOR=0xFF5577AA "$CLIENT" "$title" 800 600 \
    > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 50); do
    [[ $("$UMBRIEL" windows --json | jq 'length') == "$expected" ]] && break
    sleep 0.1
  done
  id=$(field_of "$title" id)
  if [[ -z $id ]]; then
    echo "client '$title' did not map: $(< "$UMBRIEL_RUNTIME_DIR/$title.log")"
    return 1
  fi
  "$UMBRIEL" msg window-focus "$id" > /dev/null
  "$UMBRIEL" msg window-move-to-workspace "${words[@]}" > /dev/null
}

snapshot() {
  "$UMBRIEL" windows --json | jq -c --arg prefix "$1" --argjson offset "$2" \
    '[.[] | select(.title | startswith($prefix)) | {title, x: (.x - $offset), y, w, h}] | sort_by(.title)'
}

refuge_inventory() {
  "$UMBRIEL" workspaces --json | jq -c --argjson native "$REFUGE_IDS" \
    '[.[] | select(.id as $id | $native | index($id)) |
      {id, name, named, index, layout, layout_override}] | sort_by(.index)'
}

refuge_configuration() {
  refuge_inventory | jq -c 'map(del(.id))'
}

assert_same() {
  local label=$1 expected=$2 actual=$3
  if [[ $actual != "$expected" ]]; then
    echo "$label: expected $expected, got $actual"
    return 1
  fi
}

visit_window_workspace() {
  local workspace index
  workspace=$(field_of "$1" workspace)
  index=$("$UMBRIEL" workspaces --json | jq -r --arg id "$workspace" '.[] | select(.id == $id) | .index')
  "$UMBRIEL" msg workspace-switch "$index" --output "${workspace%%:*}" > /dev/null
  "$UMBRIEL" settle
}

assert_evacuated() {
  local phase=$1 preserve_ids=$2 windows workspaces code web
  "$UMBRIEL" settle
  windows=$("$UMBRIEL" windows --json)
  if ! jq -e '
    ([.[] | select(.title | startswith("evac-code-")) | .workspace] | unique) as $code |
    ([.[] | select(.title | startswith("evac-web-")) | .workspace] | unique) as $web |
    ([.[] | select(.title == "refuge-code") | .workspace][0]) as $refuge |
    ($code | length) == 1 and ($web | length) == 1 and
    $code[0] != $web[0] and $code[0] != $refuge and $web[0] != $refuge and
    all(.[]; .workspace | startswith("HEADLESS-2:"))
  ' <<< "$windows" > /dev/null; then
    echo "$phase merged evacuated workspaces with one another or the occupied refuge: $windows"
    return 1
  fi
  code=$(field_of evac-code-top workspace)
  web=$(field_of evac-web-first workspace)
  workspaces=$("$UMBRIEL" workspaces --json)
  if ! jq -e --arg code "$code" --arg web "$web" '
    ([.[] | select(.output == "HEADLESS-2")] | length) == 4 and
    any(.[]; .id == $code and .named and .name == "code" and .index > 2 and .layout == "scrolling") and
    any(.[]; .id == $web and .named and .name == "web" and .index > 2 and .layout == "dwindle")
  ' <<< "$workspaces" > /dev/null; then
    echo "$phase did not keep the source layouts in separate temporary members: $workspaces"
    return 1
  fi
  assert_same "$phase changed native workspace configuration" "$REFUGE_CONFIGURATION" "$(refuge_configuration)"
  if [[ $preserve_ids == true ]]; then
    assert_same "$phase replaced native workspace identities" "$REFUGE_INVENTORY" "$(refuge_inventory)"
  fi
  assert_same "$phase changed the occupied refuge layout" "$REFUGE_BOX" "$(snapshot refuge-code 0)"
  visit_window_workspace evac-code-top
  assert_same "$phase lost scrolling widths, row weights, or scroll position" "$CODE_BOXES" "$(snapshot evac-code- "$OFFSET")"
  visit_window_workspace evac-web-first
  assert_same "$phase lost the dwindle split" "$WEB_BOXES" "$(snapshot evac-web- "$OFFSET")"
}

assert_restored() {
  local phase=$1 preserve_ids=$2 windows workspaces
  "$UMBRIEL" settle
  windows=$("$UMBRIEL" windows --json)
  workspaces=$("$UMBRIEL" workspaces --json)
  if ! jq -e --argjson workspaces "$workspaces" '
    all(.[];
      . as $view |
      if (.title | startswith("evac-")) then
        any($workspaces[];
          .id == $view.workspace and .output == "HEADLESS-1" and .named and
          .name == (if ($view.title | startswith("evac-code-")) then "code" else "web" end))
      else .workspace | startswith("HEADLESS-2:") end)
  ' <<< "$windows" > /dev/null; then
    echo "$phase did not restore each source workspace: $windows $workspaces"
    return 1
  fi
  if ! jq -e '
    ([.[] | select(.output == "HEADLESS-1")] | length) == 2 and
    ([.[] | select(.output == "HEADLESS-2")] | length) == 2 and all(.[]; .named)
  ' <<< "$workspaces" > /dev/null; then
    echo "$phase left temporary workspaces after their windows returned home: $workspaces"
    return 1
  fi
  assert_same "$phase changed native workspace configuration" "$REFUGE_CONFIGURATION" "$(refuge_configuration)"
  if [[ $preserve_ids == true ]]; then
    assert_same "$phase replaced native workspace identities" "$REFUGE_INVENTORY" "$(refuge_inventory)"
  fi
  assert_same "$phase changed the occupied refuge layout" "$REFUGE_BOX" "$(snapshot refuge-code 0)"
  visit_window_workspace evac-code-top
  assert_same "$phase did not restore the scrolling layout" "$CODE_BOXES" "$(snapshot evac-code- 0)"
  visit_window_workspace evac-web-first
  assert_same "$phase did not restore the dwindle layout" "$WEB_BOXES" "$(snapshot evac-web- 0)"
}

spawn refuge-code 'code --output HEADLESS-2' 1
spawn evac-code-top 'code --output HEADLESS-1' 2
spawn evac-code-bottom 'code --output HEADLESS-1' 3
"$UMBRIEL" msg window-consume-or-expel-left > /dev/null
"$UMBRIEL" msg window-modify-secondary-extent -0.2 > /dev/null
spawn evac-code-wide 'code --output HEADLESS-1' 4
"$UMBRIEL" msg window-modify-primary-extent 0.15 > /dev/null
spawn evac-web-first 'web --output HEADLESS-1' 5
spawn evac-web-second 'web --output HEADLESS-1' 6
"$UMBRIEL" msg window-modify-primary-extent 0.15 > /dev/null
"$UMBRIEL" settle

readonly CODE_BOXES="$(snapshot evac-code- 0)"
readonly WEB_BOXES="$(snapshot evac-web- 0)"
readonly REFUGE_BOX="$(snapshot refuge-code 0)"
REFUGE_IDS="$("$UMBRIEL" workspaces --json | jq -c '[.[] | select(.output == "HEADLESS-2") | .id]')"
readonly REFUGE_INVENTORY="$(refuge_inventory)"
readonly REFUGE_CONFIGURATION="$(refuge_configuration)"

# Physical removal must keep duplicate names isolated and leave both native
# refuge members intact, including its deliberately empty second workspace.
"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
assert_evacuated destruction true
"$UMBRIEL" output-create HEADLESS-1 > /dev/null
assert_restored recreation true

# A config reload applies static inventories again while one output is absent.
# Temporary members survive that reload and disappear after re-enablement.
sed '/^\[output.HEADLESS-1\]$/a enabled = false' <<< "$CONFIG" > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
assert_evacuated disable true
"$UMBRIEL" msg config-reload > /dev/null
assert_evacuated disabled-reload true
printf '%s\n' "$CONFIG" > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
assert_restored reenable true

# Suspend can remove the refuge too. When it returns before the original output,
# parked windows still need one separate temporary workspace per saved home.
"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
"$UMBRIEL" output-destroy HEADLESS-2 > /dev/null
"$UMBRIEL" output-create HEADLESS-2 > /dev/null
REFUGE_IDS="$("$UMBRIEL" workspaces --json | jq -c '[.[] | select(.output == "HEADLESS-2" and .index <= 2) | .id]')"
assert_evacuated refuge-first false
"$UMBRIEL" output-create HEADLESS-1 > /dev/null
assert_restored all-output-recreation false

echo "evacuated workspaces stayed separate with layouts intact across removal, reload, and full output loss"
