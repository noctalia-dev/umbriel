#!/usr/bin/env bash
# harness: outputs=1
# Toggling an empty scratchpad runs its spawn_when_empty command. The command
# detaches its client from the shell, and the window still joins the scratchpad
# without a window rule and is shown there. The ordinary toggle hides it again,
# and unrelated windows stay on their workspace.
set -euo pipefail

readonly CLIENT="$(realpath "${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}")"
readonly SPAWNED=scratchpad-spawned
readonly UNRELATED=scratchpad-unrelated

windows() { "$UMBRIEL" windows --json; }

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
name = "term"
spawn_when_empty = "sh -c '(\"$CLIENT\" $SPAWNED 480 300 &)' > '$UMBRIEL_RUNTIME_DIR/$SPAWNED.log' 2>&1"
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$UMBRIEL" msg scratchpad-toggle:term > /dev/null
wait_for_window "$SPAWNED" term true

"$CLIENT" "$UNRELATED" 480 300 > "$UMBRIEL_RUNTIME_DIR/$UNRELATED.log" 2>&1 &
wait_for_window "$UNRELATED" "" true

"$UMBRIEL" msg scratchpad-toggle:term > /dev/null
wait_for_window "$SPAWNED" term false

"$UMBRIEL" msg scratchpad-toggle:term > /dev/null
wait_for_window "$SPAWNED" term true
if [[ $(windows | jq --arg title "$SPAWNED" '[.[] | select(.title == $title)] | length') != 1 ]]; then
  echo "toggling a populated scratchpad ran spawn_when_empty again: $(windows)"
  exit 1
fi

echo "spawn_when_empty launched into an empty scratchpad and left unrelated windows on their workspace"
