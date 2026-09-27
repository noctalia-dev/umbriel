#!/usr/bin/env bash
# An untrusted activation must not suppress the focus an ordinary remap receives. It still cannot override an explicit
# no-focus rule, so applications cannot gain focus merely by attaching a serialless activation request.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
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
  [[ $(grep -c '^mapped$' "$CLIENT_LOG") -eq 2 && $(jq -r '.active' <<< "$target") == true ]] && break
  sleep 0.1
done

if ! grep -q '^activation-requested$' "$CLIENT_LOG" || ! grep -q '^activation-sent$' "$CLIENT_LOG" \
    || [[ $(grep -c '^mapped$' "$CLIENT_LOG") -ne 2 ]]; then
  echo "ordinary client activation did not precede remap: $(< "$CLIENT_LOG")"
  exit 1
fi
if [[ $(jq -r '.active' <<< "$target") != true || $(jq -r '.urgent' <<< "$target") != false ]]; then
  echo "untrusted activation suppressed ordinary remap focus: $windows"
  exit 1
fi
if [[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name') != 1 ]]; then
  echo "ordinary remap did not reveal its workspace: $("$UMBRIEL" workspaces --json)"
  exit 1
fi

"$UMBRIEL" msg "window-close:$target_id" > /dev/null
for _ in $(seq 60); do
  [[ $(grep -c '^unmapped$' "$CLIENT_LOG") -eq 2 ]] && break
  sleep 0.1
done
if [[ $(grep -c '^unmapped$' "$CLIENT_LOG") -ne 2 ]]; then
  echo "client activation target did not unmap again: $(< "$CLIENT_LOG")"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.app_id = "^client-activation-target$"
default_focused = false
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg workspace-switch:2 > /dev/null
printf c >&"$control_fd"
for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  target=$(jq -c '.[] | select(.app_id == "client-activation-target")' <<< "$windows")
  [[ $(grep -c '^mapped$' "$CLIENT_LOG") -eq 3 && $(jq -r '.urgent' <<< "$target") == true ]] && break
  sleep 0.1
done

if [[ $(grep -c '^activation-sent$' "$CLIENT_LOG") -ne 2 || $(grep -c '^mapped$' "$CLIENT_LOG") -ne 3 ]]; then
  echo "second ordinary client activation did not precede remap: $(< "$CLIENT_LOG")"
  exit 1
fi
if [[ $(jq -r '.active' <<< "$target") != false || $(jq -r '.urgent' <<< "$target") != true ]]; then
  echo "untrusted activation overrode default_focused false: $windows"
  exit 1
fi
if [[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name') != 2 ]]; then
  echo "rule-vetoed client activation changed workspace: $("$UMBRIEL" workspaces --json)"
  exit 1
fi

echo "untrusted activation preserves ordinary remap focus and obeys explicit no-focus rules"
