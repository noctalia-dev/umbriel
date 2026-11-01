#!/usr/bin/env bash
# A token tied to a focused input serial is validated by wlroots before Umbriel sees it. It represents user launch
# intent and may reveal a mapped target even when unsolicited activation is disabled. A pinned source records the
# active workspace where it is presented, rather than the inactive workspace it owns.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly TARGET_LOG="$UMBRIEL_RUNTIME_DIR/input-activation-target.log"
readonly SOURCE_LOG="$UMBRIEL_RUNTIME_DIR/input-activation-source.log"
readonly POINTER_LOG="$UMBRIEL_RUNTIME_DIR/input-activation-pointer.log"
readonly PINNED_TARGET_LOG="$UMBRIEL_RUNTIME_DIR/input-activation-pinned-target.log"
readonly TARGET_FIFO="$UMBRIEL_RUNTIME_DIR/input-activation-target-control"
readonly SOURCE_FIFO="$UMBRIEL_RUNTIME_DIR/input-activation-source-control"
readonly PINNED_TARGET_FIFO="$UMBRIEL_RUNTIME_DIR/input-activation-pinned-target-control"
readonly TOKEN_FILE="$UMBRIEL_RUNTIME_DIR/input-activation-token"

sed -i '/autostart = \[\]/a focus_on_activate = false\n\n[output.HEADLESS-1]\nworkspaces = 3' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$TARGET_FIFO" "$SOURCE_FIFO"
exec {target_fd}<>"$TARGET_FIFO"
exec {source_fd}<>"$SOURCE_FIFO"
APP_ID=input-activation-target REMAP_ON_STDIN=1 ACTIVATION_TOKEN_FILE="$TOKEN_FILE" \
  "$CLIENT" input-activation-target <&"$target_fd" > "$TARGET_LOG" 2>&1 &

for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  target_id=$(jq -r '.[] | select(.app_id == "input-activation-target") | .id' <<< "$windows")
  [[ -n $target_id ]] && break
  sleep 0.1
done
if [[ -z ${target_id:-} ]]; then
  echo "input activation target did not map: $windows"
  exit 1
fi

"$UMBRIEL" msg workspace-switch 2 > /dev/null
if [[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name') != 2 ]]; then
  echo "workspace 2 did not become active before input activation: $("$UMBRIEL" workspaces --json)"
  exit 1
fi
APP_ID=input-activation-source REMAP_ON_STDIN=1 ACTIVATION_TOKEN_FILE="$TOKEN_FILE" \
  "$CLIENT" input-activation-source <&"$source_fd" > "$SOURCE_LOG" 2>&1 &
for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  source_id=$(jq -r '.[] | select(.app_id == "input-activation-source") | .id' <<< "$windows")
  [[ -n $source_id ]] && break
  sleep 0.1
done
if [[ -z ${source_id:-} ]]; then
  echo "input activation source did not map: $windows"
  exit 1
fi
"$UMBRIEL" msg window-focus "$source_id" > /dev/null

"$POINTER" 1280 720 tap 30 pause 5000 > "$POINTER_LOG" 2>&1 &
for _ in $(seq 40); do
  grep -q '^key 30 1$' "$SOURCE_LOG" && break
  sleep 0.1
done
if ! grep -q '^key 30 1$' "$SOURCE_LOG"; then
  echo "activation source did not receive focused keyboard input: source=$(< "$SOURCE_LOG") pointer=$(< "$POINTER_LOG")"
  exit 1
fi

printf i >&"$source_fd"
for _ in $(seq 60); do
  [[ -s $TOKEN_FILE ]] && grep -q '^input-activation-token-written$' "$SOURCE_LOG" && break
  sleep 0.1
done
if [[ ! -s $TOKEN_FILE ]] || ! grep -q '^input-activation-token-written$' "$SOURCE_LOG"; then
  echo "focused source did not produce an activation token: $(< "$SOURCE_LOG")"
  exit 1
fi

printf a >&"$target_fd"
for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  target=$(jq -c '.[] | select(.app_id == "input-activation-target")' <<< "$windows")
  [[ $(jq -r '.active' <<< "$target") == true ]] && break
  sleep 0.1
done

if ! grep -q '^activation-sent$' "$TARGET_LOG"; then
  echo "validated input activation did not reach its target: $(< "$TARGET_LOG")"
  exit 1
fi
if [[ $(jq -r '.active' <<< "$target") != true || $(jq -r '.urgent' <<< "$target") != false ]]; then
  echo "validated input activation did not reveal and focus its target: $windows"
  exit 1
fi
if [[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name') != 1 ]]; then
  echo "validated input activation did not reveal workspace 1: $("$UMBRIEL" workspaces --json)"
  exit 1
fi

"$UMBRIEL" msg window-focus "$source_id" > /dev/null
"$UMBRIEL" msg window-toggle-pinned > /dev/null
"$UMBRIEL" msg workspace-switch 3 > /dev/null
"$UMBRIEL" msg window-focus "$source_id" > /dev/null
"$UMBRIEL" settle > /dev/null
pinned_launch_workspace=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .id')
if [[ -z $pinned_launch_workspace \
    || $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name') != 3 ]]; then
  echo "workspace 3 did not become the pinned launch origin: $("$UMBRIEL" workspaces --json)"
  exit 1
fi

: > "$TOKEN_FILE"
"$POINTER" 1280 720 tap 31 pause 5000 >> "$POINTER_LOG" 2>&1 &
for _ in $(seq 40); do
  grep -q '^key 31 1$' "$SOURCE_LOG" && break
  sleep 0.1
done
if ! grep -q '^key 31 1$' "$SOURCE_LOG"; then
  echo "pinned activation source did not receive focused keyboard input: source=$(< "$SOURCE_LOG")"
  exit 1
fi

printf i >&"$source_fd"
for _ in $(seq 60); do
  [[ -s $TOKEN_FILE ]] && [[ $(grep -c '^input-activation-token-written$' "$SOURCE_LOG") -eq 2 ]] && break
  sleep 0.1
done
if [[ ! -s $TOKEN_FILE ]] || [[ $(grep -c '^input-activation-token-written$' "$SOURCE_LOG") -ne 2 ]]; then
  echo "pinned source did not produce an activation token: $(< "$SOURCE_LOG")"
  exit 1
fi

mkfifo "$PINNED_TARGET_FIFO"
exec {pinned_target_fd}<>"$PINNED_TARGET_FIFO"
activation_token=$(< "$TOKEN_FILE")
env -u UMBRIEL_LAUNCH_TOKEN XDG_ACTIVATION_TOKEN="$activation_token" \
  APP_ID=input-activation-pinned-target ACTIVATE_ON_START=1 MAP_ON_STDIN=1 \
  "$CLIENT" input-activation-pinned-target <&"$pinned_target_fd" > "$PINNED_TARGET_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -q '^map-pending$' "$PINNED_TARGET_LOG" 2>/dev/null && break
  sleep 0.1
done
if ! grep -q '^map-pending$' "$PINNED_TARGET_LOG" 2>/dev/null; then
  echo "pinned-source target did not create its toplevel role: $(< "$PINNED_TARGET_LOG")"
  exit 1
fi

"$UMBRIEL" msg workspace-switch 1 > /dev/null
"$UMBRIEL" settle > /dev/null
printf m >&"$pinned_target_fd"
for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  pinned_target=$(jq -c '.[] | select(.app_id == "input-activation-pinned-target")' <<< "$windows")
  [[ -n $pinned_target ]] && break
  sleep 0.1
done
if [[ -z ${pinned_target:-} \
    || $(jq -r '.workspace' <<< "$pinned_target") != "$pinned_launch_workspace" \
    || $(jq -r '.active' <<< "$pinned_target") != false ]]; then
  echo "pinned source did not place its launch on the presented workspace: $windows"
  exit 1
fi

echo "validated input tokens focus existing targets and place pinned-source launches on their presented workspace"
