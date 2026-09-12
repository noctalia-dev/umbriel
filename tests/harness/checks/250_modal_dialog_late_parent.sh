#!/usr/bin/env bash
# A modal dialog that mapped before an ordinary window of its application and only then takes that window as its
# parent blocks it all the same: focus aimed at the window lands on the dialog.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/modal-dialog-late-parent.log"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/modal-dialog-late-parent.fifo"

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
EOF
"$UMBRIEL" msg config-reload > /dev/null

# The nested dialog maps first with no parent, then transient-child maps as an ordinary transient, not a modal dialog.
mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
TRANSIENT_SUITE=1 TRANSIENT_NESTED_ON_STDIN=1 "$CLIENT" transient-child <&"$control_fd" > "$CLIENT_LOG" 2>&1 &
wait_for_window_count 4
wait_for_field transient-child focused true

# The older nested dialog takes the newer window as its parent.
printf 'p' >&"$control_fd"
for _ in $(seq 60); do
  grep -q '^nested-parent-set$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^nested-parent-set$' "$CLIENT_LOG"; then
  echo "the nested dialog never took its parent: $(cat "$CLIENT_LOG")"
  exit 1
fi

# Focus aimed at the new parent lands on the dialog.
"$UMBRIEL" msg "window-focus:$(field_of transient-child id)" > /dev/null
wait_for_field transient-nested focused true
wait_for_field transient-child focused false

echo "a dialog that takes a newer window as its parent blocks it"
