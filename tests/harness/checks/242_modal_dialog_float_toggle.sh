#!/usr/bin/env bash
# Toggling floating while a modal dialog holds the focus toggles the window it is attached to, and the dialog stays
# centered on it either way.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/modal-dialog-float-toggle.log"

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

# A tiled parent with its modal dialog focused over it.
TRANSIENT_SUITE=1 TRANSIENT_MODAL=1 TRANSIENT_PARENT_SIZE=800x600 "$CLIENT" transient-child 400 300 \
  > "$CLIENT_LOG" 2>&1 &
wait_for_window_count 3
wait_for_field transient-child focused true
wait_for_field transient-parent floating false
wait_for_centered transient-child transient-parent

# The toggle reaches the parent, not the dialog.
"$UMBRIEL" msg window-toggle-floating > /dev/null
wait_for_field transient-parent floating true
wait_for_field transient-child floating true
wait_for_field transient-child focused true
wait_for_centered transient-child transient-parent

"$UMBRIEL" msg window-toggle-floating > /dev/null
wait_for_field transient-parent floating false
wait_for_field transient-child floating true
wait_for_centered transient-child transient-parent

echo "the floating toggle aimed at a modal dialog toggles its parent"
