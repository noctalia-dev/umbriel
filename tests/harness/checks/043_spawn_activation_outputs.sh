#!/usr/bin/env bash
# harness: outputs=2
# Each active output keeps a workspace-focused view, but only one view owns seat activation. A user launch targeting
# the other output must activate that view instead of mistaking its per-workspace focus for global keyboard focus.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly TARGET_LOG="$UMBRIEL_RUNTIME_DIR/spawn-output-target.log"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/spawn-output-control"
readonly TOKEN_FILE="$UMBRIEL_RUNTIME_DIR/spawn-output-token"

sed -i '/autostart = \[\]/a focus_on_activate = false' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.app_id = "^spawn-output-target$"
default_output = "HEADLESS-1"

[[window_rule]]
match.app_id = "^spawn-output-other$"
default_output = "HEADLESS-2"
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
APP_ID=spawn-output-target REMAP_ON_STDIN=1 ACTIVATION_TOKEN_FILE="$TOKEN_FILE" \
  "$CLIENT" spawn-output-target <&"$control_fd" > "$TARGET_LOG" 2>&1 &
APP_ID=spawn-output-other "$CLIENT" spawn-output-other > "$UMBRIEL_RUNTIME_DIR/spawn-output-other.log" 2>&1 &

for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  [[ $(jq 'length' <<< "$windows") -eq 2 ]] && break
  sleep 0.1
done
target_id=$(jq -r '.[] | select(.app_id == "spawn-output-target") | .id' <<< "$windows")
other_id=$(jq -r '.[] | select(.app_id == "spawn-output-other") | .id' <<< "$windows")
if [[ -z $target_id || -z $other_id ]]; then
  echo "two-output activation clients did not map: $windows"
  exit 1
fi

"$UMBRIEL" msg "window-focus:$other_id" > /dev/null
for _ in $(seq 40); do
  windows=$("$UMBRIEL" windows --json)
  target=$(jq -c --arg id "$target_id" '.[] | select(.id == $id)' <<< "$windows")
  other=$(jq -c --arg id "$other_id" '.[] | select(.id == $id)' <<< "$windows")
  [[ $(jq -r '.focused' <<< "$target") == true && $(jq -r '.active' <<< "$target") == false \
    && $(jq -r '.active' <<< "$other") == true ]] && break
  sleep 0.1
done
if [[ $(jq -r '.focused' <<< "$target") != true || $(jq -r '.active' <<< "$target") != false \
    || $(jq -r '.active' <<< "$other") != true ]]; then
  echo "setup did not leave target workspace-focused but seat-inactive: $windows"
  exit 1
fi

"$UMBRIEL" msg \
  "spawn:printf '%s\n' \"\$XDG_ACTIVATION_TOKEN\" > '$TOKEN_FILE'; printf a > '$CONTROL_FIFO'" > /dev/null
for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  [[ $(jq -r --arg id "$target_id" '.[] | select(.id == $id) | .active' <<< "$windows") == true ]] && break
  sleep 0.1
done

if [[ ! -s $TOKEN_FILE || ! $(grep -c '^activation-sent$' "$TARGET_LOG") -eq 1 ]]; then
  echo "mapped target did not receive the spawn token: $(< "$TARGET_LOG")"
  exit 1
fi
if [[ $(jq -r --arg id "$target_id" '.[] | select(.id == $id) | .active' <<< "$windows") != true ]]; then
  echo "spawn activation mistook per-workspace focus for seat activation: $windows"
  exit 1
fi

echo "spawn activation reaches a mapped workspace-focused view on another output"
