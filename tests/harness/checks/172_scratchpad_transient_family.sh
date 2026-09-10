#!/usr/bin/env bash
# A window and its dialogs enter and leave a scratchpad as one, whichever of them the action is aimed at, and a dialog
# that opens under a scratchpad parent joins it there.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly APP_LOG="$UMBRIEL_RUNTIME_DIR/scratchpad-family-app.log"
readonly DIALOG_LOG="$UMBRIEL_RUNTIME_DIR/scratchpad-family-dialog.log"

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

# The dialog sits centered on its parent, wherever the parent is now.
wait_for_centered() {
  local dialog=$1 parent=$2
  for _ in $(seq 60); do
    if windows | jq -e --arg d "$dialog" --arg p "$parent" '
        (map(select(.title == $d))[0]) as $d | (map(select(.title == $p))[0]) as $p
        | $d.x == $p.x + (($p.w - $d.w) / 2 | floor) and $d.y == $p.y + (($p.h - $d.h) / 2 | floor)' > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $dialog centered on $parent, got: $(windows)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

# A tiled parent with a modal dialog over it, and an unrelated tile of the same application.
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

# The action targets the focused dialog, and takes the parent along; the unrelated tile stays and gets the focus, as
# after any move into a scratchpad.
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
wait_for_field transient-parent scratchpad default
wait_for_field transient-child scratchpad default
wait_for_field transient-unrelated scratchpad ""
wait_for_field transient-unrelated focused true
wait_for_centered transient-child transient-parent

# Showing the pad focuses its parent, which hands the focus to the dialog over it.
"$UMBRIEL" msg scratchpad-toggle > /dev/null
"$UMBRIEL" msg scratchpad-toggle > /dev/null
wait_for_field transient-child focused true

# A dialog opening under the scratchpad parent joins the pad.
TRANSIENT_FOREIGN_HANDLE=$handle "$CLIENT" foreign-child 300 200 > "$DIALOG_LOG" 2>&1 &
wait_for_window_count 4
wait_for_field foreign-child scratchpad default
wait_for_field foreign-child focused true
wait_for_centered foreign-child transient-parent

# Restoring the focused dialog brings the family back: the parent to its tile, the dialogs over it.
"$UMBRIEL" msg window-restore-from-scratchpad > /dev/null
wait_for_field transient-parent scratchpad ""
wait_for_field transient-child scratchpad ""
wait_for_field foreign-child scratchpad ""
wait_for_field transient-parent floating false
wait_for_field foreign-child focused true
wait_for_centered foreign-child transient-parent

echo "a window and its dialogs move through the scratchpad together"
