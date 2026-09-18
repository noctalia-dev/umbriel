#!/usr/bin/env bash
# harness: outputs=1
# While a spawn_when_empty launch is pending, toggling again hides it: the
# window still joins the scratchpad but stays hidden. A pending, shown launch
# also claims the next window opened on that output even when the window's
# process never received the launch token.
set -euo pipefail

readonly CLIENT="$(realpath "${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}")"
readonly LATE=scratchpad-pending-late
readonly HANDOFF=scratchpad-pending-handoff

windows() { "$UMBRIEL" windows --json; }

wait_for_file() {
  for _ in $(seq 80); do
    [[ -e $1 ]] && return 0
    sleep 0.1
  done
  echo "expected $1 to exist"
  return 1
}

wait_for_window() {
  local title=$1 scratchpad=$2 active=$3 state=
  for _ in $(seq 80); do
    state=$(windows)
    if jq -e --arg title "$title" --arg scratchpad "$scratchpad" --argjson active "$active" '
      any(.[];
        .title == $title
        and .scratchpad == $scratchpad
        and .active == $active)
    ' <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected '$title' in scratchpad '$scratchpad' with active=$active: $state"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<EOF

[animation]
enabled = false

[[scratchpad]]
name = "late"
spawn_when_empty = "touch '$UMBRIEL_RUNTIME_DIR/late-started'; sleep 1; exec '$CLIENT' $LATE 480 300 > '$UMBRIEL_RUNTIME_DIR/$LATE.log' 2>&1"

[[scratchpad]]
name = "handoff"
spawn_when_empty = "touch '$UMBRIEL_RUNTIME_DIR/handoff-started'"
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$UMBRIEL" msg scratchpad-toggle:late > /dev/null
wait_for_file "$UMBRIEL_RUNTIME_DIR/late-started"
"$UMBRIEL" msg scratchpad-toggle:late > /dev/null
wait_for_window "$LATE" late false

"$UMBRIEL" msg scratchpad-toggle:handoff > /dev/null
wait_for_file "$UMBRIEL_RUNTIME_DIR/handoff-started"
"$CLIENT" "$HANDOFF" 480 300 > "$UMBRIEL_RUNTIME_DIR/$HANDOFF.log" 2>&1 &
wait_for_window "$HANDOFF" handoff true

echo "a hidden pending launch stored its window hidden, and a shown one claimed a window without the token"
