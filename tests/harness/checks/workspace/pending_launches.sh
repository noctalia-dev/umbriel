#!/usr/bin/env bash
# Unclaimed launches keep their destinations privately after the visible windows close. Empty workspace selection
# stays stable, and a late application restores its original destination without taking focus.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
msg() { "$UMBRIEL" msg "$@" > /dev/null; }
active_id() { "$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .id'; }
declare -A source_ids window_ids gate_fds

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation.windows_in]
enabled = false
[animation.windows_out]
enabled = false
[animation.workspaces]
duration_ms = 100
curve = "linear"
CONFIG
BASELINE=$(cat "$UMBRIEL_CONFIG")
msg config-reload
for index in $(seq 1 7); do
  msg workspace-switch "$index"
  "$UMBRIEL" settle
  "$UMBRIEL_UNMAP_CLIENT" "source-$index" > "$UMBRIEL_RUNTIME_DIR/source-$index.log" 2>&1 &
  await_window "source-$index"
  source_ids[$index]=$(active_id)
  window_ids[$index]=$(window_by_title "source-$index" | jq -r .id)
  if (( index < 3 )); then continue; fi
  gate="$UMBRIEL_RUNTIME_DIR/gate-$index"
  ready="$UMBRIEL_RUNTIME_DIR/ready-$index"
  mkfifo "$gate"
  exec {gate_fd}<>"$gate"
  gate_fds[$index]=$gate_fd
  msg spawn "printf 'ready\\n' > '$ready'; read -r _ < '$gate'; exec '$UMBRIEL_UNMAP_CLIENT' late-$index > '$UMBRIEL_RUNTIME_DIR/late-$index.log' 2>&1"
  await_lines "$ready" 'ready$' 1
done

for index in $(seq 7 -1 3); do
  msg window-close "${window_ids[$index]}"
  await_lines "$UMBRIEL_RUNTIME_DIR/source-$index.log" 'unmapped$' 1
  msg workspace-previous
done
"$UMBRIEL" settle
state=$("$UMBRIEL" workspaces --json)
if ! jq -e 'length == 3 and ([.[] | select(.occupied)] | length == 2)' <<< "$state" > /dev/null; then
  echo "unclaimed launches left visible empty workspaces: $state"
  exit 1
fi
if [[ $("$UMBRIEL_WORKSPACE_CLIENT" --all | wc -l) != 3 ]]; then
  echo "the workspace protocol exposed retained launch destinations"
  "$UMBRIEL_WORKSPACE_CLIENT" --all
  exit 1
fi
tail_id=$(jq -r 'max_by(.index).id' <<< "$state")
msg workspace-switch 3
"$UMBRIEL" settle
[[ $(active_id) == "$tail_id" ]] \
  || { echo "selecting the trailing workspace changed its identity"; exit 1; }
"$UMBRIEL" workspaces --json | jq -e 'any(.[]; .active and .index == 3)' > /dev/null

printf 'start\n' >&"${gate_fds[3]}"
await_window late-3
"$UMBRIEL" settle
[[ $(window_by_title late-3 | jq -r .workspace) == "${source_ids[3]}" && $(active_id) == "$tail_id" ]] \
  || { echo "late launch lost its destination or stole focus: $("$UMBRIEL" windows --json)"; exit 1; }
"$UMBRIEL_WORKSPACE_CLIENT" --all | awk -F '\t' -v id="${source_ids[3]}" '$1 == id { found = 1 } END { exit !found }'

# Reloading the floor can reuse retained entries without consuming extra workspace slots.
for minimum in 6 1; do
  {
    printf '%s\n' "$BASELINE"
    printf '\n[output.HEADLESS-1]\nmin_workspaces = %s\n' "$minimum"
  } > "$UMBRIEL_CONFIG"
  msg config-reload
  "$UMBRIEL" settle
  expected=$minimum
  if (( minimum == 1 )); then expected=4; fi
  "$UMBRIEL" workspaces --json | jq -e --argjson expected "$expected" \
    'length == $expected and ([.[] | select(.occupied)] | length == 3)' > /dev/null
  [[ $(active_id) == "$tail_id" ]]
done

# Removing the output invalidates retained destinations before another application can claim them.
"$UMBRIEL" output-create HEADLESS-2 > /dev/null
"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
"$UMBRIEL" settle
printf 'start\n' >&"${gate_fds[4]}"
await_window late-4
"$UMBRIEL" settle
window_by_title late-4 | jq -e --arg original "${source_ids[4]}" \
  '.output == "HEADLESS-2" and .workspace != $original' > /dev/null
echo "unused launches stay private, late launches restore identity, and reloads and output removal remain safe"
