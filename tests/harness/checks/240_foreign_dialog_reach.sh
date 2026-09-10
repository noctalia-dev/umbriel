#!/usr/bin/env bash
# A dialog attached from another process, as a portal attaches its file chooser, blocks its parent and the dialogs
# the parent had open, and leaves the application's other windows usable.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly APP_LOG="$UMBRIEL_RUNTIME_DIR/foreign-reach-app.log"
readonly DIALOG_LOG="$UMBRIEL_RUNTIME_DIR/foreign-reach-dialog.log"

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

[input.focus]
follows_mouse = true

[[window_rule]]
match.title = "^transient-parent$"
default_floating = true
default_position = { x = 0, y = 0, anchor = "top_left" }

[[window_rule]]
match.title = "^transient-unrelated$"
default_floating = true
default_position = { x = 0, y = 0, anchor = "bottom_right" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

# One application: the 800x600 parent at 0,0, its plain dialog centered on it at 200,150, and an unrelated window
# covering 640,240 to 1280,720. The parent is exported so another process can attach a dialog to it.
TRANSIENT_SUITE=1 TRANSIENT_PARENT_SIZE=800x600 EXPORT_PARENT=1 "$CLIENT" transient-child 400 300 > "$APP_LOG" 2>&1 &
wait_for_window_count 3
wait_for_field transient-child x 200
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

# The 300x200 foreign dialog lands centered on the parent, at 250,200, inside the plain dialog.
TRANSIENT_FOREIGN_HANDLE=$handle "$CLIENT" foreign-child 300 200 > "$DIALOG_LOG" 2>&1 &
wait_for_window_count 4
wait_for_field foreign-child x 250
wait_for_field foreign-child focused true

# The unrelated window is not under the foreign dialog, so the application keeps using it.
"$POINTER" 1280 720 move 1000 600
wait_for_field transient-unrelated focused true

# The plain dialog was open on the parent when the foreign dialog arrived, so it is blocked with the parent.
"$POINTER" 1280 720 move 220 170
wait_for_field foreign-child focused true
wait_for_field transient-child focused false

"$POINTER" 1280 720 move 700 100
wait_for_field foreign-child focused true
wait_for_field transient-parent focused false

"$POINTER" 1280 720 move 1000 600
wait_for_field transient-unrelated focused true

echo "a foreign dialog blocks its parent's family and nothing else of the application"
