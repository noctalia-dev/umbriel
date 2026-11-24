#!/usr/bin/env bash
# harness: outputs=2
set -euo pipefail

msg() { "$UMBRIEL" msg "$@" > /dev/null; }
state() { "$UMBRIEL" workspaces --json; }
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[workspaces]
global_numbers = true
[output.HEADLESS-1]
position = [0, 0]
[output.HEADLESS-2]
position = [1280, 0]
CONFIG
msg config-reload
msg workspace-switch 7 --output HEADLESS-1
msg workspace-set-layout master
foot --title=swap-seven sleep 120 > /dev/null 2>&1 &
for _ in $(seq 60); do
  seven=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "swap-seven") | .workspace')
  [[ -n $seven ]] && break
  sleep 0.05
done
[[ -n $seven ]]
msg workspace-switch 8 --output HEADLESS-2
msg workspace-set-layout dwindle
foot --title=swap-eight sleep 120 > /dev/null 2>&1 &
for _ in $(seq 60); do
  eight=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "swap-eight") | .workspace')
  [[ -n $eight ]] && break
  sleep 0.05
done
[[ -n $eight ]]
msg workspace-switch 7 --output HEADLESS-1
foot --title=swap-pinned sleep 120 > /dev/null 2>&1 &
for _ in $(seq 60); do
  pinned=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "swap-pinned" and .active) | .id')
  [[ -n $pinned ]] && break
  sleep 0.05
done
[[ -n $pinned ]]
msg window-toggle-pinned
"$UMBRIEL" settle
watch_log="$UMBRIEL_RUNTIME_DIR/global-swap-workspaces.log"
"${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}" --watch-groups > "$watch_log" 2>&1 &
ready=0
for _ in $(seq 60); do
  if awk -F '\t' -v seven="$seven" -v eight="$eight" '
    $1 == seven && $2 == "7" && $3 == "HEADLESS-1" { a = 1 }
    $1 == eight && $2 == "8" && $3 == "HEADLESS-2" { b = 1 }
    END { exit !(a && b) }
  ' "$watch_log"; then
    ready=1
    break
  fi
  sleep 0.05
done
[[ $ready == 1 ]]
msg workspace-swap-active-output-right
"$UMBRIEL" settle
snapshot=$(state)
if ! jq -e --arg seven "$seven" --arg eight "$eight" '
  any(.[]; .id == $seven and .name == "7" and .output == "HEADLESS-2" and .active and .layout == "master")
  and any(.[]; .id == $eight and .name == "8" and .output == "HEADLESS-1" and .active and .layout == "dwindle")
' <<< "$snapshot" > /dev/null; then
  echo "workspace identities and numbers did not follow the swap: $snapshot"
  exit 1
fi
"$UMBRIEL" windows --json | jq -e --arg seven "$seven" --arg eight "$eight" '
  any(.[]; .title == "swap-seven" and .workspace == $seven)
  and any(.[]; .title == "swap-eight" and .workspace == $eight)
  and any(.[]; .title == "swap-pinned" and .pinned and .output == "HEADLESS-1")' > /dev/null
"${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}" --groups | awk -F '\t' -v seven="$seven" -v eight="$eight" '
  $1 == seven && $2 == "7" && $3 == "HEADLESS-2" { a = 1 }
  $1 == eight && $2 == "8" && $3 == "HEADLESS-1" { b = 1 }
  END { exit !(a && b) }
'
for _ in $(seq 60); do
  if awk -F '\t' -v seven="$seven" -v eight="$eight" '
    $1 == "snapshot" { a = b = 0 }
    $1 == seven && $2 == "7" && $3 == "HEADLESS-2" { a = 1 }
    $1 == eight && $2 == "8" && $3 == "HEADLESS-1" { b = 1 }
    END { exit !(a && b) }
  ' "$watch_log"; then
    break
  fi
  sleep 0.05
done
awk -F '\t' -v seven="$seven" -v eight="$eight" '
  $1 == "snapshot" { a = b = 0 }
  $1 == seven && $2 == "7" && $3 == "HEADLESS-2" { a = 1 }
  $1 == eight && $2 == "8" && $3 == "HEADLESS-1" { b = 1 }
  END { exit !(a && b) }
' "$watch_log"
msg workspace-swap-active-output-right
"$UMBRIEL" settle
state | jq -e --arg seven "$seven" --arg eight "$eight" '
  any(.[]; .id == $seven and .name == "7" and .output == "HEADLESS-1" and .active)
  and any(.[]; .id == $eight and .name == "8" and .output == "HEADLESS-2" and .active)' > /dev/null
# An empty workspace must survive the first transfer until the swap completes.
msg workspace-switch 9 --output HEADLESS-1
msg workspace-switch 8 --output HEADLESS-2
msg workspace-switch 9 --output HEADLESS-1
msg workspace-swap-active-output-right
"$UMBRIEL" settle
state | jq -e '
  any(.[]; .name == "9" and .output == "HEADLESS-2" and .active and (.occupied | not))
  and any(.[]; .name == "8" and .output == "HEADLESS-1" and .active and .occupied)' > /dev/null
"$UMBRIEL" config set input.cursor.follows_focus true > /dev/null
msg workspace-switch 7 --output HEADLESS-1
msg workspace-swap-active-output-right
"$UMBRIEL" settle
"$UMBRIEL" windows --json | jq -e 'any(.[]; .title == "swap-seven" and .output == "HEADLESS-2" and .active)' > /dev/null
state | jq -e 'any(.[]; .name == "7" and .output == "HEADLESS-2" and .focused)' > /dev/null
echo "global workspace identities follow their windows when swapping outputs"
