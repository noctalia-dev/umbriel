#!/usr/bin/env bash
# A workspace step stops at the ends of the inventory unless the output enables
# cyclic_workspaces, in which case it wraps to the other end. The switch and the
# move variants have to agree: stepping back on the first workspace would
# otherwise wrap the workspace while leaving moved content behind.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"

active_workspace() {
  "$UMBRIEL" workspaces | sed -n 's/^\* HEADLESS-1: \([0-9]\{1,\}\).*/\1/p'
}

expect_workspace() {
  local want=$1 what=$2 got
  got=$(active_workspace)
  if [[ $got != "$want" ]]; then
    echo "$what: on workspace '$got', want '$want'"
    exit 1
  fi
}

window_workspace() {
  "$UMBRIEL" windows --json | jq -r --arg t "$1" '.[] | select(.title == $t) | .workspace'
}

# Moves reach the workspace through layout reconciliation, so poll rather than
# sampling once.
expect_window_at() {
  local title=$1 want=$2 what=$3 got=
  for _ in $(seq 60); do
    got=$(window_workspace "$title")
    [[ $got == "$want" ]] && return 0
    sleep 0.05
  done
  echo "$what: window '$title' is on '$got', want '$want'"
  exit 1
}

spawn() {
  "$CLIENT" "$1" > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 60); do
    [[ -n $(window_workspace "$1") ]] && return 0
    sleep 0.05
  done
  echo "client '$1' never mapped: $("$UMBRIEL" windows --json)"
  exit 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1

[output.HEADLESS-1]
workspaces = 3
cyclic_workspaces = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

expect_workspace 1 "start"

"$UMBRIEL" msg workspace-previous > /dev/null
expect_workspace 3 "workspace-previous on the first workspace"

"$UMBRIEL" msg workspace-next > /dev/null
expect_workspace 1 "workspace-next on the last workspace"

# Two windows on the first workspace so the column move and the window move can
# be checked one at a time; the later one owns focus and therefore the column.
spawn cyclic-a
spawn cyclic-b
expect_window_at cyclic-a "HEADLESS-1:1" "first probe window"
expect_window_at cyclic-b "HEADLESS-1:1" "second probe window"

"$UMBRIEL" msg column-move-to-workspace-previous > /dev/null
expect_window_at cyclic-b "HEADLESS-1:3" "column-move-to-workspace-previous"
expect_window_at cyclic-a "HEADLESS-1:1" "the window left behind"

FOCUS_ID=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "cyclic-a") | .id')
"$UMBRIEL" msg "window-focus:$FOCUS_ID" > /dev/null
"$UMBRIEL" msg window-move-to-workspace-previous > /dev/null
expect_window_at cyclic-a "HEADLESS-1:3" "window-move-to-workspace-previous"

# At the column edge, the composite vertical move steps workspaces with the same wrap.
"$UMBRIEL" msg window-move-or-workspace-down > /dev/null
expect_window_at cyclic-a "HEADLESS-1:1" "window-move-or-workspace-down on the last workspace"
"$UMBRIEL" msg window-move-or-workspace-up > /dev/null
expect_window_at cyclic-a "HEADLESS-1:3" "window-move-or-workspace-up on the first workspace"

# Without the key the step stops at the ends.
sed -i 's/^cyclic_workspaces = true$/cyclic_workspaces = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg workspace-switch:1 > /dev/null
expect_workspace 1 "workspace-switch back to the first workspace"
"$UMBRIEL" msg workspace-previous > /dev/null
expect_workspace 1 "workspace-previous without the key"

echo "workspace steps wrapped at both ends with the key, for the switch and the column, window, and edge moves, and stopped without it"
