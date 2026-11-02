#!/usr/bin/env bash
# harness: outputs=2
# A pure XDG activation keeps the output and workspace recorded at launch without taking focus from another output.
# Explicit output, workspace, and scratchpad rules remain authoritative over that recorded destination.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly XDG_LOG="$UMBRIEL_RUNTIME_DIR/spawn-workspace-xdg.log"
readonly POLICY_LOG="$UMBRIEL_RUNTIME_DIR/spawn-workspace-policy.log"
readonly TARGET_LOG="$UMBRIEL_RUNTIME_DIR/spawn-workspace-rule-target.log"
readonly SCRATCH_LOG="$UMBRIEL_RUNTIME_DIR/spawn-workspace-rule-scratch.log"
readonly XDG_START_FIFO="$UMBRIEL_RUNTIME_DIR/spawn-workspace-xdg-start"
readonly RULE_START_FIFO="$UMBRIEL_RUNTIME_DIR/spawn-workspace-rule-start"
readonly XDG_TOKEN_FILE="$UMBRIEL_RUNTIME_DIR/spawn-workspace-xdg-token"
readonly RULE_TOKEN_FILE="$UMBRIEL_RUNTIME_DIR/spawn-workspace-rule-token"

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

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output.HEADLESS-1]
position = [0, 0]
workspaces = 2

[output.HEADLESS-2]
position = [1280, 0]
workspaces = 2

[[scratchpad]]
name = "launch-rule"

[[window_rule]]
match.app_id = "^spawn-workspace-policy$"
focus_on_activate = false

[[window_rule]]
match.app_id = "^spawn-workspace-rule-target$"
default_output = "HEADLESS-2"
default_workspace = 2
default_focused = false
focus_on_activate = false

[[window_rule]]
match.app_id = "^spawn-workspace-rule-scratch$"
default_output = "HEADLESS-2"
default_workspace = 2
default_scratchpad = "launch-rule"
default_focused = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$UMBRIEL" msg workspace-switch 2 --output HEADLESS-1 > /dev/null
"$UMBRIEL" settle > /dev/null
launch_workspace=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .active) | .id')
rule_workspace=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-2" and .index == 2) | .id')
if [[ -z $launch_workspace || -z $rule_workspace ]]; then
  echo "static rule workspaces were not created: $("$UMBRIEL" workspaces --json)"
  exit 1
fi

mkfifo "$XDG_START_FIFO" "$RULE_START_FIFO"
exec {xdg_start_fd}<>"$XDG_START_FIFO"
exec {rule_start_fd}<>"$RULE_START_FIFO"
"$UMBRIEL" msg \
  spawn "printf '%s\n' \"\$XDG_ACTIVATION_TOKEN\" > '$XDG_TOKEN_FILE'; read -r _ < '$XDG_START_FIFO'; exec env -u UMBRIEL_LAUNCH_TOKEN APP_ID=spawn-workspace-xdg ACTIVATE_ON_START=1 '$CLIENT' spawn-workspace-xdg > '$XDG_LOG' 2>&1" \
  > /dev/null
for _ in $(seq 80); do
  [[ -s $XDG_TOKEN_FILE ]] && break
  sleep 0.1
done
if [[ ! -s $XDG_TOKEN_FILE ]]; then
  echo "XDG target did not inherit an activation token"
  exit 1
fi

"$UMBRIEL" msg workspace-switch 1 --output HEADLESS-2 > /dev/null
"$UMBRIEL" settle > /dev/null
right_workspace=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-2" and .focused) | .id')
printf 'start\n' >&"$xdg_start_fd"
wait_for_app spawn-workspace-xdg
windows=$("$UMBRIEL" windows --json)
xdg_target=$(jq -c '.[] | select(.app_id == "spawn-workspace-xdg")' <<< "$windows")
if ! grep -q '^activation-sent$' "$XDG_LOG"; then
  echo "XDG target did not activate before its initial map: $(< "$XDG_LOG")"
  exit 1
fi
if [[ $(jq -r '.workspace' <<< "$xdg_target") != "$launch_workspace" \
    || $(jq -r '.active' <<< "$xdg_target") != false ]]; then
  echo "XDG target did not map silently on its recorded output and workspace: $windows"
  exit 1
fi
workspaces=$("$UMBRIEL" workspaces --json)
if [[ $(jq -r '.[] | select(.focused) | .id' <<< "$workspaces") != "$right_workspace" ]]; then
  echo "XDG target pulled focus back to its recorded output: $workspaces"
  exit 1
fi

"$UMBRIEL" msg workspace-switch 2 --output HEADLESS-1 > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg \
  spawn "exec env -u UMBRIEL_LAUNCH_TOKEN APP_ID=spawn-workspace-policy ACTIVATE_ON_START=1 '$CLIENT' spawn-workspace-policy > '$POLICY_LOG' 2>&1" \
  > /dev/null
wait_for_app spawn-workspace-policy
windows=$("$UMBRIEL" windows --json)
policy_target=$(jq -c '.[] | select(.app_id == "spawn-workspace-policy")' <<< "$windows")
if ! grep -q '^activation-sent$' "$POLICY_LOG"; then
  echo "focus policy target did not activate before its initial map: $(< "$POLICY_LOG")"
  exit 1
fi
if [[ $(jq -r '.workspace' <<< "$policy_target") != "$launch_workspace" \
    || $(jq -r '.active' <<< "$policy_target") != false ]]; then
  echo "focus_on_activate changed launch placement or focused the new window: $windows"
  exit 1
fi

"$UMBRIEL" msg \
  spawn "printf '%s\n' \"\$XDG_ACTIVATION_TOKEN\" > '$RULE_TOKEN_FILE'; read -r _ < '$RULE_START_FIFO'; exec env -u UMBRIEL_LAUNCH_TOKEN APP_ID=spawn-workspace-rule-target ACTIVATE_ON_START=1 '$CLIENT' spawn-workspace-rule-target > '$TARGET_LOG' 2>&1" \
  > /dev/null
for _ in $(seq 80); do
  [[ -s $RULE_TOKEN_FILE ]] && break
  sleep 0.1
done
if [[ ! -s $RULE_TOKEN_FILE ]]; then
  echo "rule target did not inherit an activation token"
  exit 1
fi

"$UMBRIEL" msg workspace-switch 1 --output HEADLESS-1 > /dev/null
"$UMBRIEL" settle > /dev/null
left_workspace=$("$UMBRIEL" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .focused) | .id')
printf 'start\n' >&"$rule_start_fd"

wait_for_app spawn-workspace-rule-target
windows=$("$UMBRIEL" windows --json)
target=$(jq -c '.[] | select(.app_id == "spawn-workspace-rule-target")' <<< "$windows")
if ! grep -q '^activation-sent$' "$TARGET_LOG"; then
  echo "rule target did not activate before its initial map: $(< "$TARGET_LOG")"
  exit 1
fi
if [[ $(jq -r '.workspace' <<< "$target") != "$rule_workspace" \
    || $(jq -r '.active' <<< "$target") != false ]]; then
  echo "explicit output and workspace rules did not override launch placement: $windows"
  exit 1
fi
if [[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .active) | .id') != "$left_workspace" ]]; then
  echo "rule target changed the launch output's active workspace: $("$UMBRIEL" workspaces --json)"
  exit 1
fi

"$UMBRIEL" msg \
  spawn "exec env APP_ID=spawn-workspace-rule-scratch '$CLIENT' spawn-workspace-rule-scratch > '$SCRATCH_LOG' 2>&1" \
  > /dev/null
for _ in $(seq 80); do
  windows=$("$UMBRIEL" windows --json)
  scratch=$(jq -c '.[] | select(.app_id == "spawn-workspace-rule-scratch")' <<< "$windows")
  [[ -n $scratch ]] && break
  sleep 0.1
done
if [[ -z ${scratch:-} \
    || $(jq -r '.scratchpad' <<< "$scratch") != launch-rule \
    || $(jq -r '.workspace' <<< "$scratch") != "" \
    || $(jq -r '.active' <<< "$scratch") != false ]]; then
  echo "default_scratchpad did not override launch placement: $windows"
  exit 1
fi

echo "XDG launch placement preserves its output silently, while explicit placement rules remain authoritative"
