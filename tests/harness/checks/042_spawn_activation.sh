#!/usr/bin/env bash
# A user-requested spawn carries a compositor-issued activation token. The launched command can hand that token to an
# already-running application, which may activate an existing XDG surface before remapping it. That direct user intent
# must reveal the application even when unsolicited activation requests normally only mark windows urgent.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/spawn-activation-client.log"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/spawn-activation-control"
readonly TOKEN_FILE="$UMBRIEL_RUNTIME_DIR/spawn-activation-token"

sed -i '/autostart = \[\]/a focus_on_activate = false' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.app_id = "^spawn-activation-target$"
default_focused = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
APP_ID=spawn-activation-target REMAP_ON_STDIN=1 ACTIVATION_TOKEN_FILE="$TOKEN_FILE" \
  "$CLIENT" spawn-activation-target 900 600 <&"$control_fd" > "$CLIENT_LOG" 2>&1 &

for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" 2>/dev/null && break
  sleep 0.1
done
if ! grep -q '^mapped$' "$CLIENT_LOG" 2>/dev/null; then
  echo "activation target did not map: $(< "$CLIENT_LOG")"
  exit 1
fi

windows=$("$UMBRIEL" windows --json)
target_id=$(jq -r '.[] | select(.app_id == "spawn-activation-target") | .id' <<< "$windows")
target_workspace=$(jq -r '.[] | select(.app_id == "spawn-activation-target") | .workspace' <<< "$windows")
if [[ -z $target_id ]]; then
  echo "could not resolve activation target: $windows"
  exit 1
fi
"$UMBRIEL" msg "window-close:$target_id" > /dev/null
for _ in $(seq 60); do
  grep -q '^unmapped$' "$CLIENT_LOG" 2>/dev/null && break
  sleep 0.1
done
if ! grep -q '^unmapped$' "$CLIENT_LOG" 2>/dev/null; then
  echo "activation target did not unmap: $(< "$CLIENT_LOG")"
  exit 1
fi

"$UMBRIEL" msg workspace-switch:2 > /dev/null
if [[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name') != 2 ]]; then
  echo "workspace 2 did not become active before activation: $("$UMBRIEL" workspaces --json)"
  exit 1
fi

# The first printf finishes and closes its file before the FIFO signal is sent, so the target always reads a complete
# token. On the target's Wayland connection, activate is queued before the remap commit.
"$UMBRIEL" msg \
  "spawn:printf '%s\n' \"\$XDG_ACTIVATION_TOKEN\" > '$TOKEN_FILE'; printf a > '$CONTROL_FIFO'" > /dev/null

for _ in $(seq 80); do
  windows=$("$UMBRIEL" windows --json)
  if [[ $(jq -r '.[] | select(.app_id == "spawn-activation-target") | .active' <<< "$windows") == true ]]; then
    break
  fi
  sleep 0.1
done

if [[ ! -s $TOKEN_FILE ]]; then
  echo "spawned command did not receive an activation token"
  exit 1
fi
if ! grep -q '^activation-sent$' "$CLIENT_LOG" || [[ $(grep -c '^mapped$' "$CLIENT_LOG") -ne 2 ]]; then
  echo "target did not activate before remapping: $(< "$CLIENT_LOG")"
  exit 1
fi

windows=$("$UMBRIEL" windows --json)
target=$(jq -c '.[] | select(.app_id == "spawn-activation-target")' <<< "$windows")
if [[ $(jq -r '.workspace' <<< "$target") != "$target_workspace"
    || $(jq -r '.focused' <<< "$target") != true
    || $(jq -r '.active' <<< "$target") != true
    || $(jq -r '.urgent' <<< "$target") != false ]]; then
  echo "trusted spawn activation did not reveal and focus the remapped target: $windows"
  exit 1
fi
if [[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name') != 1 ]]; then
  echo "trusted spawn activation did not return to workspace 1: $("$UMBRIEL" workspaces --json)"
  exit 1
fi

echo "spawn token activates an unmapped target before remap and reveals its workspace"
