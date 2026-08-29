#!/usr/bin/env bash
# An explicit window rule remains authoritative over compositor-issued launch intent. The app ID is deliberately sent
# after activation and before remap so the policy must be matched from the final map metadata, not cached too early.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/spawn-rule-client.log"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/spawn-rule-control"
readonly TOKEN_FILE="$UMBRIEL_RUNTIME_DIR/spawn-rule-token"

sed -i '/autostart = \[\]/a focus_on_activate = false' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.app_id = "^spawn-rule-target$"
focus_on_activate = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
APP_ID=spawn-rule-initial APP_ID_AFTER_ACTIVATION=spawn-rule-target REMAP_ON_STDIN=1 \
  ACTIVATION_TOKEN_FILE="$TOKEN_FILE" "$CLIENT" spawn-rule-target \
  <&"$control_fd" > "$CLIENT_LOG" 2>&1 &

for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  target_id=$(jq -r '.[] | select(.app_id == "spawn-rule-initial") | .id' <<< "$windows")
  [[ -n $target_id ]] && break
  sleep 0.1
done
if [[ -z ${target_id:-} ]]; then
  echo "rule target did not map with its initial app ID: $windows"
  exit 1
fi
"$UMBRIEL" msg "window-close:$target_id" > /dev/null
for _ in $(seq 60); do
  grep -q '^unmapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^unmapped$' "$CLIENT_LOG"; then
  echo "rule target did not unmap: $(< "$CLIENT_LOG")"
  exit 1
fi

"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" msg \
  "spawn:printf '%s\n' \"\$XDG_ACTIVATION_TOKEN\" > '$TOKEN_FILE'; printf a > '$CONTROL_FIFO'" > /dev/null
for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  target=$(jq -c '.[] | select(.app_id == "spawn-rule-target")' <<< "$windows")
  [[ $(grep -c '^mapped$' "$CLIENT_LOG") -eq 2 && $(jq -r '.urgent' <<< "$target") == true ]] && break
  sleep 0.1
done

if [[ ! -s $TOKEN_FILE || ! $(grep -c '^activation-sent$' "$CLIENT_LOG") -eq 1 \
    || $(grep -c '^mapped$' "$CLIENT_LOG") -ne 2 ]]; then
  echo "trusted activation did not precede remap: $(< "$CLIENT_LOG")"
  exit 1
fi
if [[ $(jq -r '.active' <<< "$target") != false || $(jq -r '.urgent' <<< "$target") != true ]]; then
  echo "focus_on_activate rule did not veto trusted launch intent: $windows"
  exit 1
fi
if [[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name') != 2 ]]; then
  echo "rule-vetoed activation changed workspace: $("$UMBRIEL" workspaces --json)"
  exit 1
fi

echo "focus_on_activate false vetoes a spawn token resolved from final map metadata"
