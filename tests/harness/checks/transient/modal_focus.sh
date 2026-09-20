#!/usr/bin/env bash
# Focus aimed at a window blocked by a modal dialog lands on the dialog. An application's own modal dialog blocks its
# windows on the dialog's workspace; a dialog attached from another process blocks only its parent and the parent's
# dialogs.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly APP_LOG="$UMBRIEL_RUNTIME_DIR/modal-focus-app.log"
readonly FOREIGN_LOG="$UMBRIEL_RUNTIME_DIR/modal-focus-foreign.log"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/modal-focus.fifo"

windows() {
  "$UMBRIEL" windows --json
}

field_of() {
  local title=$1 field=$2
  windows | jq -r --arg title "$title" --arg field "$field" '.[] | select(.title == $title) | .[$field]'
}

active_workspace() {
  "$UMBRIEL" workspaces --json | jq -r '[.[] | select(.active) | .name] | if length == 1 then .[0] else "none" end'
}

workspace_id() {
  local name=$1
  "$UMBRIEL" workspaces --json | jq -r --arg name "$name" '.[] | select(.name == $name) | .id'
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

[input.focus]
follows_mouse = true

[[window_rule]]
match.title = "^transient-parent$"
default_floating = true
default_position = { x = 0, y = 0, anchor = "top_right" }

[[window_rule]]
match.title = "^transient-unrelated$"
default_floating = true
default_position = { x = 0, y = 0, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

# The unrelated window covers 0,0 to 400,300 and the parent 480,0 to 1280,600, with the modal child over its middle.
TRANSIENT_SUITE=1 TRANSIENT_MODAL=1 TRANSIENT_PARENT_SIZE=800x600 "$CLIENT" transient-child 400 300 \
  > "$APP_LOG" 2>&1 &
app=$!
wait_for_window_count 3
wait_for_field transient-unrelated x 0
wait_for_field transient-parent x 480
wait_for_field transient-child x 680
wait_for_field transient-child focused true

# The unrelated window belongs to the same application and was open before the dialog, so it is blocked too.
"$POINTER" 1280 720 move 200 150
sleep 0.3 # real time: proves the hover does not move the focus
wait_for_field transient-child focused true
wait_for_field transient-unrelated focused false

# Without the modal dialog this hover focuses the parent, as transient/stacking shows.
"$POINTER" 1280 720 move 520 500
sleep 0.3 # real time: proves the hover does not move the focus
wait_for_field transient-child focused true
wait_for_field transient-parent focused false

# Closing the dialog frees the application: the same hover now focuses the unrelated window.
"$UMBRIEL" msg window-close > /dev/null
wait_for_window_count 2
"$POINTER" 1280 720 move 200 150
wait_for_field transient-unrelated focused true
kill "$app"
wait_for_window_count 0

# The same layout with a plain dialog at 680,150 and the parent exported, so another process can attach its 300x200
# dialog to the parent, centered at 730,200 inside the plain one.
TRANSIENT_SUITE=1 TRANSIENT_PARENT_SIZE=800x600 EXPORT_PARENT=1 "$CLIENT" transient-child 400 300 > "$APP_LOG" 2>&1 &
app=$!
wait_for_window_count 3
wait_for_field transient-child x 680
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
TRANSIENT_FOREIGN_HANDLE=$handle "$CLIENT" foreign-child 300 200 > "$FOREIGN_LOG" 2>&1 &
foreign=$!
wait_for_window_count 4
wait_for_field foreign-child x 730
wait_for_field foreign-child focused true

# The foreign dialog leaves the application's other window usable, but blocks the parent and the plain dialog that was
# open on it.
"$POINTER" 1280 720 move 200 150
wait_for_field transient-unrelated focused true
"$POINTER" 1280 720 move 700 170
wait_for_field foreign-child focused true
wait_for_field transient-child focused false
"$POINTER" 1280 720 move 200 150
wait_for_field transient-unrelated focused true
"$POINTER" 1280 720 move 520 500
wait_for_field foreign-child focused true
wait_for_field transient-parent focused false
kill "$foreign" "$app"
wait_for_window_count 0
"$POINTER" 1280 720 move 200 650

# A modal dialog that mapped before another dialog of its application and only then takes that dialog as its parent
# leaves focus on the newer dialog. Each would otherwise block the other, and the compositor hangs walking focus from
# one to the other, so this step times out instead of failing.
mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
TRANSIENT_SUITE=1 TRANSIENT_MODAL=1 TRANSIENT_NESTED_ON_STDIN=1 "$CLIENT" transient-child \
  <&"$control_fd" > "$APP_LOG" 2>&1 &
app=$!
wait_for_window_count 4
wait_for_field transient-child focused true
printf 'p' >&"$control_fd"
for _ in $(seq 60); do
  grep -q '^nested-parent-set$' "$APP_LOG" && break
  sleep 0.1
done
if ! grep -q '^nested-parent-set$' "$APP_LOG"; then
  echo "the nested dialog never took its parent: $(cat "$APP_LOG")"
  exit 1
fi
"$UMBRIEL" msg "window-focus:$(field_of transient-parent id)" > /dev/null
wait_for_field transient-child focused true
"$UMBRIEL" msg "window-focus:$(field_of transient-nested id)" > /dev/null
wait_for_field transient-child focused true
wait_for_field transient-nested focused false
kill "$app"
wait_for_window_count 0

# The same modal dialog taking an ordinary window that mapped after it as its parent blocks that window all the same.
TRANSIENT_SUITE=1 TRANSIENT_NESTED_ON_STDIN=1 "$CLIENT" transient-child <&"$control_fd" > "$APP_LOG" 2>&1 &
app=$!
wait_for_window_count 4
wait_for_field transient-child focused true
printf 'p' >&"$control_fd"
for _ in $(seq 60); do
  grep -q '^nested-parent-set$' "$APP_LOG" && break
  sleep 0.1
done
if ! grep -q '^nested-parent-set$' "$APP_LOG"; then
  echo "the nested dialog never took its late parent: $(cat "$APP_LOG")"
  exit 1
fi
"$UMBRIEL" msg "window-focus:$(field_of transient-child id)" > /dev/null
wait_for_field transient-nested focused true
wait_for_field transient-child focused false
kill "$app"
wait_for_window_count 0

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.title = "^transient-unrelated$"
default_workspace = 2
EOF
"$UMBRIEL" msg config-reload > /dev/null

# The parent opens on workspace 1 and the application's other window on workspace 2, which becomes active. The dialog
# still opens with its parent, not on the active workspace.
TRANSIENT_SUITE=1 TRANSIENT_MODAL=1 "$CLIENT" transient-child 400 300 > "$APP_LOG" 2>&1 &
wait_for_window_count 3
wait_for_field transient-unrelated workspace "$(workspace_id 2)"
wait_for_field transient-child workspace "$(workspace_id 1)"
"$UMBRIEL" msg workspace-switch:1 > /dev/null
wait_for_field transient-child focused true

# The window on workspace 2 is out of the dialog's reach: the switch lands there and the focus stays with it.
"$UMBRIEL" msg workspace-switch:2 > /dev/null
wait_for_field transient-unrelated focused true
sleep 0.3 # real time: proves the switch does not bounce back
if [[ $(active_workspace) != 2 ]]; then
  echo "the switch bounced back to the dialog's workspace: $(active_workspace)"
  exit 1
fi

echo "focus aimed at a window a modal dialog blocks lands on the dialog"
