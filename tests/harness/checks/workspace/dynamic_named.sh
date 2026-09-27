#!/usr/bin/env bash
# harness: outputs=2
# Named workspace declarations materialize persistent entries inside dynamic inventories. Unscoped declarations
# apply to both outputs, output-scoped declarations stay local, and anonymous trailing workspaces remain available.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly BASELINE="$(< "$UMBRIEL_CONFIG")"

write_config() {
  local numeric_layout=$1 numeric_state=$2 right_output=$3 global_state=${4:-present}
  local next_config="${UMBRIEL_CONFIG}.next"
  printf '%s\n' "$BASELINE" > "$next_config"
  cat >> "$next_config" <<'EOF'

[animation]
enabled = false

[layout]
mode = "scrolling"

[output.HEADLESS-1]
workspaces = "dynamic"

[output.HEADLESS-2]
workspaces = "dynamic"
EOF
  if [[ $numeric_state == present ]]; then
    cat >> "$next_config" <<EOF

[[workspace]]
name = "2"
output = "HEADLESS-1"
layout.mode = "$numeric_layout"
EOF
  fi
  if [[ $global_state == present ]]; then
    cat >> "$next_config" <<'EOF'

[[workspace]]
name = "GLOBAL"
EOF
  fi
  cat >> "$next_config" <<EOF

[[workspace]]
name = "RIGHT"
output = "$right_output"
EOF
  cat >> "$next_config" <<'EOF'

[[workspace]]
index = 2
output = "HEADLESS-1"
layout.mode = "dwindle"

[[window_rule]]
match.app_id = "^dynamic-position-two$"
default_output = "HEADLESS-1"
default_workspace = 2

[[window_rule]]
match.app_id = "^dynamic-name-two$"
default_output = "HEADLESS-1"
default_workspace = "2"
default_focused = false

[[window_rule]]
match.app_id = "^dynamic-right$"
default_workspace = "RIGHT"
default_focused = false
EOF
  mv "$next_config" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}

workspace_id() {
  "$UMBRIEL" workspaces --json | jq -r --arg output "$1" --arg name "$2" \
    '.[] | select(.output == $output and .name == $name) | .id'
}

expect_inventory() {
  local label=$1 left_expected=$2 right_expected=$3 snapshot=
  for _ in $(seq 80); do
    snapshot=$("$UMBRIEL" workspaces --json)
    if jq -e --argjson left_expected "$left_expected" --argjson right_expected "$right_expected" '
      def rows($output):
        [.[] | select(.output == $output)] | sort_by(.index) | map([.name, .index, .layout]);
      rows("HEADLESS-1") == $left_expected
      and rows("HEADLESS-2") == $right_expected
    ' <<< "$snapshot" > /dev/null; then
      return 0
    fi
    sleep 0.05
  done
  echo "$label: dynamic named inventory did not settle: $snapshot"
  exit 1
}

wait_for_focused_workspace() {
  local expected=$1 actual=
  for _ in $(seq 60); do
    actual=$("$UMBRIEL" workspaces --json | jq -r \
      '[.[] | select(.focused) | .id] | if length == 1 then .[0] else "" end')
    [[ $actual == "$expected" ]] && return 0
    sleep 0.05
  done
  echo "expected focused workspace $expected, got '$actual': $("$UMBRIEL" workspaces --json)"
  exit 1
}

wait_for_window_workspace() {
  local title=$1 expected=$2 actual=
  for _ in $(seq 60); do
    actual=$("$UMBRIEL" windows --json | jq -r --arg title "$title" \
      '.[] | select(.title == $title) | .workspace')
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' on workspace $expected, got '$actual': $("$UMBRIEL" windows --json)"
  exit 1
}

destroy_window() {
  local title=$1 pid=$2
  kill "$pid"
  wait "$pid" 2>/dev/null || true
  for _ in $(seq 60); do
    if ! "$UMBRIEL" windows --json | jq -e --arg title "$title" '.[] | select(.title == $title)' > /dev/null; then
      return 0
    fi
    sleep 0.05
  done
  echo "'$title' did not close: $("$UMBRIEL" windows --json)"
  exit 1
}

expect_stable_id() {
  local output=$1 name=$2 expected=$3 actual
  actual=$(workspace_id "$output" "$name")
  if [[ $actual != "$expected" ]]; then
    echo "workspace '$name' on $output changed identity from $expected to '$actual'"
    exit 1
  fi
}

expect_missing_id() {
  local removed=$1 label=$2
  if "$UMBRIEL" workspaces --json | jq -e --arg id "$removed" 'any(.[]; .id == $id)' > /dev/null; then
    echo "$label retained removed workspace ID $removed: $("$UMBRIEL" workspaces --json)"
    exit 1
  fi
}

expect_named_state() {
  local id=$1 expected=$2 actual
  actual=$("$UMBRIEL" workspaces --json | jq -r --arg id "$id" '.[] | select(.id == $id) | .named')
  if [[ $actual != "$expected" ]]; then
    echo "workspace $id had named=$actual instead of $expected: $("$UMBRIEL" workspaces --json)"
    exit 1
  fi
}

write_config master present HEADLESS-2
expect_inventory "initial materialization" \
  '[["2",1,"master"],["GLOBAL",2,"dwindle"],["3",3,"scrolling"]]' \
  '[["GLOBAL",1,"scrolling"],["RIGHT",2,"scrolling"],["3",3,"scrolling"]]'

numeric_id=$(workspace_id HEADLESS-1 2)
left_global_id=$(workspace_id HEADLESS-1 GLOBAL)
right_global_id=$(workspace_id HEADLESS-2 GLOBAL)
right_id=$(workspace_id HEADLESS-2 RIGHT)
left_sentinel_id=$(workspace_id HEADLESS-1 3)
right_sentinel_id=$(workspace_id HEADLESS-2 3)

if [[ -z $numeric_id || -z $left_global_id || -z $right_global_id || -z $right_id \
      || -z $left_sentinel_id || -z $right_sentinel_id ]]; then
  echo "dynamic named inventory is missing an expected workspace: $("$UMBRIEL" workspaces --json)"
  exit 1
fi
if [[ $left_global_id == "$right_global_id" ]]; then
  echo "the unscoped GLOBAL declaration did not materialize independently on both outputs"
  exit 1
fi
expect_named_state "$numeric_id" true
expect_named_state "$left_global_id" true
expect_named_state "$right_global_id" true
expect_named_state "$right_id" true
expect_named_state "$left_sentinel_id" false
expect_named_state "$right_sentinel_id" false

# The string "2" is an exact name at index 1. Selecting it must not fall back to the dynamic trailing workspace.
"$UMBRIEL" msg 'workspace-switch:"2"/HEADLESS-1' > /dev/null
wait_for_focused_workspace "$numeric_id"

# A TOML string window target resolves the exact numeric name rather than positional slot 2.
"$UMBRIEL" msg workspace-switch:GLOBAL/HEADLESS-1 > /dev/null
wait_for_focused_workspace "$left_global_id"
APP_ID=dynamic-name-two "$CLIENT" dynamic-name-two 800 600 \
  > "$UMBRIEL_RUNTIME_DIR/dynamic-name-two.log" 2>&1 &
dynamic_name_pid=$!
wait_for_window_workspace dynamic-name-two "$numeric_id"
destroy_window dynamic-name-two "$dynamic_name_pid"

# Without an explicit output, a unique scoped dynamic name selects its owning output.
APP_ID=dynamic-right "$CLIENT" dynamic-right 800 600 \
  > "$UMBRIEL_RUNTIME_DIR/dynamic-right.log" 2>&1 &
dynamic_right_pid=$!
wait_for_window_workspace dynamic-right "$right_id"
destroy_window dynamic-right "$dynamic_right_pid"

# Leaving the empty named workspace triggers dynamic reconciliation. It must retain the same protocol identity.
"$UMBRIEL" msg workspace-switch:GLOBAL/HEADLESS-1 > /dev/null
wait_for_focused_workspace "$left_global_id"
expect_inventory "empty named persistence" \
  '[["2",1,"master"],["GLOBAL",2,"dwindle"],["3",3,"scrolling"]]' \
  '[["GLOBAL",1,"scrolling"],["RIGHT",2,"scrolling"],["3",3,"scrolling"]]'
expect_stable_id HEADLESS-1 2 "$numeric_id"
expect_stable_id HEADLESS-1 3 "$left_sentinel_id"
expect_named_state "$numeric_id" true
expect_named_state "$left_sentinel_id" false

# An integer window-rule target remains positional. Slot 2 is GLOBAL, while the numeric string name "2" is slot 1.
"$UMBRIEL" msg 'workspace-switch:"2"/HEADLESS-1' > /dev/null
wait_for_focused_workspace "$numeric_id"
APP_ID=dynamic-position-two "$CLIENT" dynamic-position-two 800 600 \
  > "$UMBRIEL_RUNTIME_DIR/dynamic-position-two.log" 2>&1 &
wait_for_window_workspace dynamic-position-two "$left_global_id"
expect_inventory "integer window-rule target" \
  '[["2",1,"master"],["GLOBAL",2,"dwindle"],["3",3,"scrolling"]]' \
  '[["GLOBAL",1,"scrolling"],["RIGHT",2,"scrolling"],["3",3,"scrolling"]]'
expect_stable_id HEADLESS-1 2 "$numeric_id"

# A layout-only reload updates the named rule without replacing named workspaces or either anonymous sentinel.
write_config scrolling present HEADLESS-2
expect_inventory "layout-only reload" \
  '[["2",1,"scrolling"],["GLOBAL",2,"dwindle"],["3",3,"scrolling"]]' \
  '[["GLOBAL",1,"scrolling"],["RIGHT",2,"scrolling"],["3",3,"scrolling"]]'
expect_stable_id HEADLESS-1 2 "$numeric_id"
expect_stable_id HEADLESS-1 GLOBAL "$left_global_id"
expect_stable_id HEADLESS-2 GLOBAL "$right_global_id"
expect_stable_id HEADLESS-2 RIGHT "$right_id"
expect_stable_id HEADLESS-1 3 "$left_sentinel_id"
expect_stable_id HEADLESS-2 3 "$right_sentinel_id"
expect_named_state "$numeric_id" true
expect_named_state "$left_global_id" true
expect_named_state "$right_global_id" true
expect_named_state "$right_id" true
expect_named_state "$left_sentinel_id" false
expect_named_state "$right_sentinel_id" false

# Remove the empty numeric name while GLOBAL is active. Ordinary dynamic pruning removes its old ID and renumbers
# the same trailing sentinel into slot 2, while every unrelated configured workspace keeps its identity.
"$UMBRIEL" msg workspace-switch:GLOBAL/HEADLESS-1 > /dev/null
wait_for_focused_workspace "$left_global_id"
write_config scrolling absent HEADLESS-2
expect_inventory "named declaration removal" \
  '[["GLOBAL",1,"scrolling"],["2",2,"dwindle"]]' \
  '[["GLOBAL",1,"scrolling"],["RIGHT",2,"scrolling"],["3",3,"scrolling"]]'
expect_missing_id "$numeric_id" "named declaration removal"
expect_stable_id HEADLESS-1 GLOBAL "$left_global_id"
expect_stable_id HEADLESS-2 GLOBAL "$right_global_id"
expect_stable_id HEADLESS-2 RIGHT "$right_id"
expect_stable_id HEADLESS-1 2 "$left_sentinel_id"
expect_stable_id HEADLESS-2 3 "$right_sentinel_id"
expect_named_state "$left_sentinel_id" false

# Readding the declaration creates a fresh exact name at the top-side insertion point. Existing names and the
# renumbered sentinel retain their IDs.
write_config scrolling present HEADLESS-2
expect_inventory "named declaration readdition" \
  '[["2",1,"scrolling"],["GLOBAL",2,"dwindle"],["3",3,"scrolling"]]' \
  '[["GLOBAL",1,"scrolling"],["RIGHT",2,"scrolling"],["3",3,"scrolling"]]'
readded_numeric_id=$(workspace_id HEADLESS-1 2)
if [[ -z $readded_numeric_id || $readded_numeric_id == "$numeric_id" || $readded_numeric_id == "$left_sentinel_id" ]]; then
  echo "readded numeric name did not receive a fresh identity: old=$numeric_id new='$readded_numeric_id'"
  exit 1
fi
expect_missing_id "$numeric_id" "named declaration readdition"
expect_stable_id HEADLESS-1 GLOBAL "$left_global_id"
expect_stable_id HEADLESS-2 GLOBAL "$right_global_id"
expect_stable_id HEADLESS-2 RIGHT "$right_id"
expect_stable_id HEADLESS-1 3 "$left_sentinel_id"
expect_stable_id HEADLESS-2 3 "$right_sentinel_id"
expect_named_state "$readded_numeric_id" true
expect_named_state "$left_sentinel_id" false
"$UMBRIEL" msg 'workspace-switch:"2"/HEADLESS-1' > /dev/null
wait_for_focused_workspace "$readded_numeric_id"
"$UMBRIEL" msg workspace-switch:GLOBAL/HEADLESS-1 > /dev/null
wait_for_focused_workspace "$left_global_id"

# Retargeting RIGHT removes the old empty instance on HEADLESS-2 and materializes a fresh one at the top of
# HEADLESS-1. Previously configured names and both anonymous sentinels keep their protocol identities.
write_config scrolling present HEADLESS-1
expect_inventory "named declaration retarget" \
  '[["RIGHT",1,"scrolling"],["2",2,"dwindle"],["GLOBAL",3,"scrolling"],["4",4,"scrolling"]]' \
  '[["GLOBAL",1,"scrolling"],["2",2,"scrolling"]]'
retargeted_right_id=$(workspace_id HEADLESS-1 RIGHT)
if [[ -z $retargeted_right_id || $retargeted_right_id == "$right_id" ]]; then
  echo "retargeted RIGHT did not materialize with a fresh identity: old=$right_id new='$retargeted_right_id'"
  exit 1
fi
expect_missing_id "$right_id" "named declaration retarget"
expect_stable_id HEADLESS-1 2 "$readded_numeric_id"
expect_stable_id HEADLESS-1 GLOBAL "$left_global_id"
expect_stable_id HEADLESS-2 GLOBAL "$right_global_id"
expect_stable_id HEADLESS-1 4 "$left_sentinel_id"
expect_stable_id HEADLESS-2 2 "$right_sentinel_id"
expect_named_state "$retargeted_right_id" true
expect_named_state "$readded_numeric_id" true
expect_named_state "$left_sentinel_id" false
expect_named_state "$right_sentinel_id" false

# Removing a populated inactive name keeps that workspace and its windows as an anonymous member. The empty GLOBAL on
# HEADLESS-2 is active, so it survives anonymously and becomes the trailing empty after its old successor is pruned.
"$UMBRIEL" msg workspace-switch:GLOBAL/HEADLESS-2 > /dev/null
wait_for_focused_workspace "$right_global_id"
"$UMBRIEL" msg workspace-switch:RIGHT/HEADLESS-1 > /dev/null
wait_for_focused_workspace "$retargeted_right_id"
write_config scrolling present HEADLESS-1 absent
expect_inventory "populated named declaration removal" \
  '[["RIGHT",1,"scrolling"],["2",2,"dwindle"],["3",3,"scrolling"],["4",4,"scrolling"]]' \
  '[["1",1,"scrolling"]]'
expect_stable_id HEADLESS-1 3 "$left_global_id"
expect_stable_id HEADLESS-2 1 "$right_global_id"
expect_named_state "$left_global_id" false
expect_named_state "$right_global_id" false
expect_stable_id HEADLESS-1 4 "$left_sentinel_id"
expect_missing_id "$right_sentinel_id" "active unnamed workspace becoming the trailing empty"
wait_for_window_workspace dynamic-position-two "$left_global_id"

# Save the populated anonymous workspace as a scratchpad return target, then remove that workspace and insert enough
# configured names to occupy its former position. Restoring must insert a new anonymous slot instead of sending the
# window to the numeric-looking configured name now at that position.
position_window_id=$("$UMBRIEL" windows --json | jq -r \
  '.[] | select(.title == "dynamic-position-two") | .id')
"$UMBRIEL" msg "window-focus:$position_window_id" > /dev/null
wait_for_focused_workspace "$left_global_id"
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
for _ in $(seq 60); do
  if "$UMBRIEL" windows --json | jq -e '
    any(.[]; .title == "dynamic-position-two" and .scratchpad == "default" and .workspace == "")
  ' > /dev/null; then
    break
  fi
  sleep 0.05
done
if ! "$UMBRIEL" windows --json | jq -e '
  any(.[]; .title == "dynamic-position-two" and .scratchpad == "default" and .workspace == "")
' > /dev/null; then
  echo "dynamic-position-two did not enter the scratchpad: $("$UMBRIEL" windows --json)"
  exit 1
fi
"$UMBRIEL" msg workspace-switch:RIGHT/HEADLESS-1 > /dev/null
wait_for_focused_workspace "$retargeted_right_id"
write_config scrolling present HEADLESS-1 present
expect_inventory "scratchpad return collision setup" \
  '[["GLOBAL",1,"scrolling"],["RIGHT",2,"dwindle"],["2",3,"scrolling"],["4",4,"scrolling"]]' \
  '[["GLOBAL",1,"scrolling"],["2",2,"scrolling"]]'
expect_stable_id HEADLESS-1 2 "$readded_numeric_id"

"$UMBRIEL" msg scratchpad-toggle > /dev/null
for _ in $(seq 60); do
  if "$UMBRIEL" windows --json | jq -e '
    any(.[]; .title == "dynamic-position-two" and .scratchpad == "default" and .active)
  ' > /dev/null; then
    break
  fi
  sleep 0.05
done
if ! "$UMBRIEL" windows --json | jq -e '
  any(.[]; .title == "dynamic-position-two" and .scratchpad == "default" and .active)
' > /dev/null; then
  echo "dynamic-position-two did not focus in the scratchpad: $("$UMBRIEL" windows --json)"
  exit 1
fi
"$UMBRIEL" msg window-restore-from-scratchpad > /dev/null
for _ in $(seq 60); do
  restored_workspace=$("$UMBRIEL" windows --json | jq -r \
    '.[] | select(.title == "dynamic-position-two" and .scratchpad == "") | .workspace')
  [[ -n $restored_workspace ]] && break
  sleep 0.05
done
if [[ -z ${restored_workspace:-} ]]; then
  echo "dynamic-position-two did not leave the scratchpad: $("$UMBRIEL" windows --json)"
  exit 1
fi
restored_named=$("$UMBRIEL" workspaces --json | jq -r --arg id "$restored_workspace" \
  '.[] | select(.id == $id) | .named')
restored_index=$("$UMBRIEL" workspaces --json | jq -r --arg id "$restored_workspace" \
  '.[] | select(.id == $id) | .index')
if [[ $restored_workspace == "$readded_numeric_id" || $restored_named != false || $restored_index != 3 ]]; then
  echo "scratchpad restore did not recreate anonymous position 3: $("$UMBRIEL" workspaces --json)"
  exit 1
fi
expect_stable_id HEADLESS-1 2 "$readded_numeric_id"
expect_named_state "$readded_numeric_id" true
wait_for_window_workspace dynamic-position-two "$restored_workspace"

echo "dynamic named workspaces preserve typed identities through reload, placement, scratchpad restore, and pruning"
