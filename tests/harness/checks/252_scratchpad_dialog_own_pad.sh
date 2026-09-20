#!/usr/bin/env bash
# A dialog a rule opens in a scratchpad of its own is no part of its parent's family in another pad: restoring it
# leaves the parent and its other dialog where they are.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly APP_LOG="$UMBRIEL_RUNTIME_DIR/scratchpad-own-pad-app.log"
readonly DIALOG_LOG="$UMBRIEL_RUNTIME_DIR/scratchpad-own-pad-dialog.log"

windows() {
  "$UMBRIEL" windows --json
}

field_of() {
  local title=$1 field=$2
  windows | jq -r --arg title "$title" --arg field "$field" '.[] | select(.title == $title) | .[$field]'
}

wait_for_window_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $(windows | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $(windows)"
  return 1
}

wait_for_field() {
  local title=$1 field=$2 want=$3
  for _ in $(seq 60); do
    [[ $(field_of "$title" "$field") == "$want" ]] && return 0
    sleep 0.1
  done
  echo "expected $title $field=$want, got: $(windows)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[[scratchpad]]
name = "app"

[[scratchpad]]
name = "picker"

[[window_rule]]
match.title = "^foreign-child$"
default_scratchpad = "picker"
EOF
"$UMBRIEL" msg config-reload > /dev/null

# A parent with a modal dialog over it moves into the "app" pad as one family.
TRANSIENT_SUITE=1 TRANSIENT_MODAL=1 TRANSIENT_PARENT_SIZE=800x600 EXPORT_PARENT=1 "$CLIENT" transient-child 400 300 \
  > "$APP_LOG" 2>&1 &
wait_for_window_count 3
wait_for_field transient-child focused true
handle=
for _ in $(seq 60); do
  handle=$(sed -n 's/^exported handle=//p' "$APP_LOG")
  [[ -n $handle ]] && break
  sleep 0.1
done
if [[ -z $handle ]]; then
  echo "the parent never received its xdg-foreign handle: $(cat "$APP_LOG")"
  exit 1
fi
"$UMBRIEL" msg window-move-to-scratchpad:app > /dev/null
wait_for_field transient-parent scratchpad app
wait_for_field transient-child scratchpad app

# A portal-style dialog of the parent opens where its rule sends it, the "picker" pad, instead of the parent's.
TRANSIENT_FOREIGN_HANDLE=$handle "$CLIENT" foreign-child 300 200 > "$DIALOG_LOG" 2>&1 &
wait_for_window_count 4
wait_for_field foreign-child scratchpad picker
wait_for_field transient-parent scratchpad app

# Restoring the picker's window takes only that window out.
"$UMBRIEL" msg scratchpad-toggle:picker > /dev/null
"$UMBRIEL" msg window-restore-from-scratchpad:picker > /dev/null
wait_for_field foreign-child scratchpad ""
wait_for_field transient-parent scratchpad app
wait_for_field transient-child scratchpad app

echo "a dialog in a scratchpad of its own is no part of its parent's family in another pad"
