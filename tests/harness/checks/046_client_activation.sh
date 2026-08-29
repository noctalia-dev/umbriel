#!/usr/bin/env bash
# An ordinary client-issued activation remains unsolicited when the global policy is disabled. It may mark a hidden
# target urgent after remap, but it must not reveal the target workspace or steal seat focus.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/client-activation.log"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/client-activation-control"

sed -i '/autostart = \[\]/a focus_on_activate = false' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
APP_ID=client-activation-target REMAP_ON_STDIN=1 \
  "$CLIENT" client-activation-target <&"$control_fd" > "$CLIENT_LOG" 2>&1 &

for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  target_id=$(jq -r '.[] | select(.app_id == "client-activation-target") | .id' <<< "$windows")
  [[ -n $target_id ]] && break
  sleep 0.1
done
if [[ -z ${target_id:-} ]]; then
  echo "client activation target did not map: $windows"
  exit 1
fi
"$UMBRIEL" msg "window-close:$target_id" > /dev/null
for _ in $(seq 60); do
  grep -q '^unmapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^unmapped$' "$CLIENT_LOG"; then
  echo "client activation target did not unmap: $(< "$CLIENT_LOG")"
  exit 1
fi

"$UMBRIEL" msg workspace-switch:2 > /dev/null
printf c >&"$control_fd"
for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  target=$(jq -c '.[] | select(.app_id == "client-activation-target")' <<< "$windows")
  [[ $(grep -c '^mapped$' "$CLIENT_LOG") -eq 2 && $(jq -r '.urgent' <<< "$target") == true ]] && break
  sleep 0.1
done

if ! grep -q '^activation-requested$' "$CLIENT_LOG" || ! grep -q '^activation-sent$' "$CLIENT_LOG" \
    || [[ $(grep -c '^mapped$' "$CLIENT_LOG") -ne 2 ]]; then
  echo "ordinary client activation did not precede remap: $(< "$CLIENT_LOG")"
  exit 1
fi
if [[ $(jq -r '.active' <<< "$target") != false || $(jq -r '.urgent' <<< "$target") != true ]]; then
  echo "ordinary client activation stole focus or failed to mark urgent: $windows"
  exit 1
fi
if [[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name') != 2 ]]; then
  echo "ordinary client activation changed workspace: $("$UMBRIEL" workspaces --json)"
  exit 1
fi

echo "ordinary client activation remaps urgent without stealing focus"
