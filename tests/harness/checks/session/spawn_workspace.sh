#!/usr/bin/env bash
# A delayed spawn keeps its exact dynamic launch workspace alive, then maps there without taking the user away from
# the workspace they switched to. The inherited-token fallback covers clients that never request XDG activation, and
# the captured placement is consumed before an existing toplevel remaps.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly HOME_LOG="$UMBRIEL_RUNTIME_DIR/spawn-workspace-home.log"
readonly TEMP_LOG="$UMBRIEL_RUNTIME_DIR/spawn-workspace-temp.log"
readonly TARGET_LOG="$UMBRIEL_RUNTIME_DIR/spawn-workspace-target.log"
readonly KEEPER_LOG="$UMBRIEL_RUNTIME_DIR/spawn-workspace-keeper.log"
readonly START_FIFO="$UMBRIEL_RUNTIME_DIR/spawn-workspace-start"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/spawn-workspace-control"
readonly TOKEN_FILE="$UMBRIEL_RUNTIME_DIR/spawn-workspace-token"

wait_for_app() {
  local app_id=$1 state=
  for _ in $(seq 80); do
    state=$("$UMBRIEL" windows --json)
    if jq -e --arg app_id "$app_id" 'any(.[]; .app_id == $app_id)' <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "application '$app_id' did not map: $state"
  return 1
}

APP_ID=spawn-workspace-home "$CLIENT" spawn-workspace-home > "$HOME_LOG" 2>&1 &
wait_for_app spawn-workspace-home
home_workspace=$("$UMBRIEL" windows --json | jq -r '.[] | select(.app_id == "spawn-workspace-home") | .workspace')

"$UMBRIEL" msg workspace-switch 2 > /dev/null
"$UMBRIEL" settle > /dev/null
launch_workspace=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .id')
if [[ -z $launch_workspace || $launch_workspace == "$home_workspace" ]]; then
  echo "workspace 2 did not become the launch origin: $("$UMBRIEL" workspaces --json)"
  exit 1
fi

APP_ID=spawn-workspace-temp "$CLIENT" spawn-workspace-temp > "$TEMP_LOG" 2>&1 &
wait_for_app spawn-workspace-temp
for _ in $(seq 60); do
  workspaces=$("$UMBRIEL" workspaces --json)
  trailing_workspace=$(jq -r 'sort_by(.index) | last | .id' <<< "$workspaces")
  [[ $trailing_workspace != "$launch_workspace" ]] && break
  sleep 0.1
done
if [[ -z $trailing_workspace || $trailing_workspace == "$launch_workspace" ]]; then
  echo "occupying the launch workspace did not create a trailing workspace: $workspaces"
  exit 1
fi

mkfifo "$START_FIFO" "$CONTROL_FIFO"
exec {start_fd}<>"$START_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
"$UMBRIEL" msg \
  spawn "printf '%s\n' \"\$XDG_ACTIVATION_TOKEN\" > '$TOKEN_FILE'; read -r _ < '$START_FIFO'; exec env APP_ID=spawn-workspace-target MAP_ON_STDIN=1 REMAP_ON_STDIN=1 '$CLIENT' spawn-workspace-target < '$CONTROL_FIFO' > '$TARGET_LOG' 2>&1" \
  > /dev/null

for _ in $(seq 80); do
  [[ -s $TOKEN_FILE ]] && break
  sleep 0.1
done
if [[ ! -s $TOKEN_FILE ]]; then
  echo "delayed spawn did not receive an activation token"
  exit 1
fi

temp_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.app_id == "spawn-workspace-temp") | .id')
trailing_index=$("$UMBRIEL" workspaces --json | jq -r --arg id "$trailing_workspace" '.[] | select(.id == $id) | .index')
"$UMBRIEL" msg window-focus "$temp_id" > /dev/null
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
"$UMBRIEL" msg window-move-to-workspace-silent "$trailing_index" > /dev/null
for _ in $(seq 80); do
  windows=$("$UMBRIEL" windows --json)
  [[ $(jq -r '.[] | select(.app_id == "spawn-workspace-temp") | .workspace' <<< "$windows") == "$trailing_workspace" ]] \
    && break
  sleep 0.1
done
if [[ $(jq -r '.[] | select(.app_id == "spawn-workspace-temp") | .workspace' <<< "$windows") != "$trailing_workspace" ]]; then
  echo "temporary window did not make the following workspace substantive: $windows"
  exit 1
fi

"$UMBRIEL" msg workspace-switch 1 > /dev/null
"$UMBRIEL" settle > /dev/null
workspaces=$("$UMBRIEL" workspaces --json)
if ! jq -e --arg pending "$launch_workspace" --arg occupied "$trailing_workspace" '
  all(.[]; .id != $pending)
  and any(.[]; .id == $occupied and .index == 2 and .occupied)
  and length == 3
' <<< "$workspaces" > /dev/null; then
  echo "pending launch left a visible empty workspace: $workspaces"
  exit 1
fi

printf 'start\n' >&"$start_fd"
for _ in $(seq 80); do
  grep -q '^map-pending$' "$TARGET_LOG" 2>/dev/null && break
  sleep 0.1
done
if ! grep -q '^map-pending$' "$TARGET_LOG" 2>/dev/null; then
  echo "delayed target did not create its toplevel role: $(< "$TARGET_LOG")"
  exit 1
fi
workspaces=$("$UMBRIEL" workspaces --json)
if [[ $(jq -r --arg id "$launch_workspace" '.[] | select(.id == $id) | .occupied' <<< "$workspaces") != false ]]; then
  echo "unmapped launch target occupied its reserved workspace: $workspaces"
  exit 1
fi
printf m >&"$control_fd"
wait_for_app spawn-workspace-target
windows=$("$UMBRIEL" windows --json)
target=$(jq -c '.[] | select(.app_id == "spawn-workspace-target")' <<< "$windows")
if [[ $(jq -r '.workspace' <<< "$target") != "$launch_workspace" \
    || $(jq -r '.active' <<< "$target") != false ]]; then
  echo "delayed target did not map silently on its launch workspace: $windows"
  exit 1
fi
if [[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .id') != "$home_workspace" ]]; then
  echo "delayed target pulled focus away from the current workspace: $("$UMBRIEL" workspaces --json)"
  exit 1
fi

target_id=$(jq -r '.id' <<< "$target")
"$UMBRIEL" msg window-focus-warp "$target_id" > /dev/null
APP_ID=spawn-workspace-keeper "$CLIENT" spawn-workspace-keeper > "$KEEPER_LOG" 2>&1 &
keeper_pid=$!
wait_for_app spawn-workspace-keeper
windows=$("$UMBRIEL" windows --json)
if [[ $(jq -r '.[] | select(.app_id == "spawn-workspace-keeper") | .workspace' <<< "$windows") != "$launch_workspace" ]]; then
  echo "remap guard did not map on the launch workspace: $windows"
  exit 1
fi

trailing_index=$("$UMBRIEL" workspaces --json | jq -r --arg id "$trailing_workspace" '.[] | select(.id == $id) | .index')
"$UMBRIEL" msg window-focus "$target_id" > /dev/null
"$UMBRIEL" msg window-move-to-workspace-silent "$trailing_index" > /dev/null
"$UMBRIEL" msg workspace-switch 1 > /dev/null
"$UMBRIEL" settle > /dev/null

windows=$("$UMBRIEL" windows --json)
if [[ $(jq -r '.[] | select(.app_id == "spawn-workspace-target") | .workspace' <<< "$windows") != "$trailing_workspace" ]]; then
  echo "moving the target changed the destination workspace identity: $windows"
  exit 1
fi
"$UMBRIEL" msg window-close "$target_id" > /dev/null
for _ in $(seq 60); do
  grep -q '^unmapped$' "$TARGET_LOG" 2>/dev/null && break
  sleep 0.1
done
if ! grep -q '^unmapped$' "$TARGET_LOG" 2>/dev/null; then
  echo "target did not unmap before the remap request: $(< "$TARGET_LOG")"
  exit 1
fi
printf r >&"$control_fd"
for _ in $(seq 80); do
  windows=$("$UMBRIEL" windows --json)
  [[ $(grep -c '^mapped$' "$TARGET_LOG" 2>/dev/null || true) -eq 2 \
      && $(jq -r '.[] | select(.app_id == "spawn-workspace-target") | .workspace' <<< "$windows") \
          == "$trailing_workspace" ]] \
    && break
  sleep 0.1
done
if [[ $(grep -c '^mapped$' "$TARGET_LOG" 2>/dev/null || true) -ne 2 \
    || $(jq -r '.[] | select(.app_id == "spawn-workspace-target") | .workspace' <<< "$windows") != "$trailing_workspace" ]]; then
  echo "remapping target reused its consumed launch placement: log=$(< "$TARGET_LOG") windows=$windows"
  exit 1
fi

kill "$keeper_pid"
wait "$keeper_pid" 2>/dev/null || true
for _ in $(seq 60); do
  workspaces=$("$UMBRIEL" workspaces --json)
  ! jq -e --arg id "$launch_workspace" 'any(.[]; .id == $id)' <<< "$workspaces" > /dev/null && break
  sleep 0.1
done
if jq -e --arg id "$launch_workspace" 'any(.[]; .id == $id)' <<< "$workspaces" > /dev/null; then
  echo "consumed launch reservation kept an empty workspace alive: $workspaces"
  exit 1
fi

echo "delayed spawn keeps its launch workspace, maps silently, releases the reservation, and applies placement once"
