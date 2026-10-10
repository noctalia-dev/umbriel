#!/usr/bin/env bash
# Stepping from a focused dialog steps from the scratchpad window it belongs to, and a scratchpad hidden while a dialog
# had focus shows that dialog's parent again, even if the dialog closed in the meantime. With the parent shown on its
# own, its focused dialog is the scratchpad's current window for focus-next and restore.
set -euo pipefail

readonly PARENT_CLIENT="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly PARENT_LOG="$UMBRIEL_RUNTIME_DIR/focus-parent.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

windows() { "$UMBRIEL" windows --json; }
window_of() { windows | jq -c --arg title "$1" '.[] | select(.title == $title)'; }
active() { windows | jq -r '.[] | select(.active) | .title'; }

wait_for_window() {
  for _ in $(seq 80); do
    [[ -n "$(window_of "$1")" ]] && return 0
    sleep 0.05
  done
  echo "window '$1' never appeared: $(windows)"
  return 1
}

expect_active() {
  local actual
  actual=$(active)
  if [[ $actual != "$1" ]]; then
    echo "$2: expected $1 to have focus, got [$actual]: $(windows)"
    exit 1
  fi
}

focus_dialog() {
  "$UMBRIEL" msg "window-focus:$(window_of focus-dialog | jq -r .id)" > /dev/null
  expect_active focus-dialog "the dialog did not take focus"
}

# Three regular members, the middle one able to parent a dialog.
"$CLIENT" focus-first 300 200 > "$UMBRIEL_RUNTIME_DIR/focus-first.log" 2>&1 &
wait_for_window focus-first
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null

EXPORT_TOPLEVEL=1 "$PARENT_CLIENT" focus-parent > "$PARENT_LOG" 2>&1 &
wait_for_window focus-parent
handle=
for _ in $(seq 80); do
  handle=$(sed -n 's/^exported handle=//p' "$PARENT_LOG")
  [[ -n $handle ]] && break
  sleep 0.05
done
if [[ -z $handle ]]; then
  echo "parent did not export an xdg-foreign handle: $(cat "$PARENT_LOG")"
  exit 1
fi
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null

"$CLIENT" focus-last 300 200 > "$UMBRIEL_RUNTIME_DIR/focus-last.log" 2>&1 &
wait_for_window focus-last
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null

# Show every window, then open the middle member's dialog; it joins the scratchpad with its parent.
"$UMBRIEL" msg scratchpad-toggle > /dev/null
TRANSIENT_FOREIGN_HANDLE="$handle" "$CLIENT" focus-dialog 200 150 > "$UMBRIEL_RUNTIME_DIR/focus-dialog.log" 2>&1 &
dialog=$!
wait_for_window focus-dialog
if [[ $(window_of focus-dialog | jq -r .scratchpad) != default ]]; then
  echo "the dialog did not join the scratchpad: $(windows)"
  exit 1
fi

focus_dialog
"$UMBRIEL" msg scratchpad-window-show-next > /dev/null
expect_active focus-last "next from the middle member's dialog"

"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" msg scratchpad-toggle > /dev/null
focus_dialog
"$UMBRIEL" msg scratchpad-window-show-previous > /dev/null
expect_active focus-first "previous from the middle member's dialog"

# Hidden with the dialog focused, the scratchpad shows the dialog's parent again.
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" msg scratchpad-toggle > /dev/null
focus_dialog
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" msg scratchpad-window-show-next > /dev/null
expect_active focus-parent "showing a scratchpad hidden while a dialog had focus"

# With the parent shown on its own, focus moves on from its focused dialog rather than staying there.
focus_dialog
"$UMBRIEL" msg scratchpad-focus-next > /dev/null
expect_active focus-parent "focus-next from the dialog of the window on show"

# The same holds when the dialog closes while the scratchpad is hidden.
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" msg scratchpad-toggle > /dev/null
focus_dialog
"$UMBRIEL" msg scratchpad-toggle > /dev/null
kill "$dialog"
for _ in $(seq 80); do
  [[ -z "$(window_of focus-dialog)" ]] && break
  sleep 0.05
done
if [[ -n "$(window_of focus-dialog)" ]]; then
  echo "the dialog did not close: $(windows)"
  exit 1
fi
"$UMBRIEL" msg scratchpad-window-show-next > /dev/null
expect_active focus-parent "showing a scratchpad whose focused dialog closed while it was hidden"

# Restoring with a dialog of the window on show focused takes out the dialog and keeps its parent stored.
TRANSIENT_FOREIGN_HANDLE="$handle" "$CLIENT" focus-dialog 200 150 > "$UMBRIEL_RUNTIME_DIR/focus-dialog-2.log" 2>&1 &
wait_for_window focus-dialog
focus_dialog
"$UMBRIEL" msg window-restore-from-scratchpad > /dev/null
restored_dialog=$(window_of focus-dialog | jq -r .scratchpad)
stored_parent=$(window_of focus-parent | jq -r .scratchpad)
if [[ -n $restored_dialog || $stored_parent != default ]]; then
  echo "restore took out [parent: '$stored_parent', dialog: '$restored_dialog'] instead of the focused dialog: $(windows)"
  exit 1
fi

echo "stepping from a focused dialog stepped from the window it belongs to"
