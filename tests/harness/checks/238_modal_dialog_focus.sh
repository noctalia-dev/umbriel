#!/usr/bin/env bash
# Focusing the parent of an open modal dialog, or another window of the same application, focuses the dialog instead.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/modal-dialog-focus.log"

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
default_position = { x = 0, y = 0, anchor = "top_right" }

[[window_rule]]
match.title = "^transient-unrelated$"
default_floating = true
default_position = { x = 0, y = 0, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

# The unrelated window covers 0,0 to 400,300 and the parent 480,0 to 1280,600, with the modal child over its middle.
TRANSIENT_SUITE=1 TRANSIENT_MODAL=1 TRANSIENT_PARENT_SIZE=800x600 "$CLIENT" transient-child 400 300 \
  > "$CLIENT_LOG" 2>&1 &
wait_for_window_count 3
wait_for_field transient-unrelated x 0
wait_for_field transient-parent x 480
wait_for_field transient-child x 680
wait_for_field transient-child focused true

# The unrelated window belongs to the same application and was open before the dialog, so it is blocked too.
"$POINTER" 1280 720 move 200 150
sleep 0.3
wait_for_field transient-child focused true
wait_for_field transient-unrelated focused false

# Without the modal dialog this hover focuses the parent, as 236_transient_stacking shows.
"$POINTER" 1280 720 move 520 500
sleep 0.3
wait_for_field transient-child focused true
wait_for_field transient-parent focused false

# Closing the dialog frees the application: the same hover now focuses the unrelated window.
"$UMBRIEL" msg window-close > /dev/null
wait_for_window_count 2
"$POINTER" 1280 720 move 200 150
wait_for_field transient-unrelated focused true

echo "focus aimed at a modal dialog's application lands on the dialog"
