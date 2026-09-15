#!/usr/bin/env bash
# In the scrolling layout a modal dialog belongs to its parent's column: focusing it reveals the parent, scrolling the
# strip takes the dialog off screen with the parent, and directional focus steps from the parent's column.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/modal-dialog-scrolling.log"

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

# The parent keeps its own 400x300 inside its column; harness-a fills its column, so its width is the column width.
wait_for_column_flush_right() {
  local title=$1
  for _ in $(seq 60); do
    if windows | jq -e --arg title "$title" '
        (map(select(.title == "harness-a"))[0].w) as $w
        | .[] | select(.title == $title) | .x + $w == 1270' > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected the column of $title flush with the right edge, got: $(windows)"
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

# Output is 1280x720 with edgePad 10, so a column flush with the left edge sits at x=10. Three 0.6 columns overflow the
# strip, and revealing the third leaves the second partly off the left edge.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[layout.scrolling]
default_width_fraction = 0.6
center_focused = "never"
EOF
"$UMBRIEL" msg config-reload > /dev/null

foot --title=harness-a sh -c 'sleep 120' > /dev/null 2>&1 &
wait_for_window_count 1
wait_for_field harness-a focused true

# The parent takes the second column, the application's other window the third, and the dialog opens last while the
# parent is partly scrolled away. Its focus reveals the parent, and it sits centered on the parent itself.
TRANSIENT_SUITE=1 TRANSIENT_MODAL=1 "$CLIENT" transient-child 400 300 > "$CLIENT_LOG" 2>&1 &
wait_for_window_count 4
wait_for_field transient-child focused true
wait_for_field transient-parent x 10
wait_for_centered transient-child transient-parent

# Scrolling the strip moves the dialog with the parent instead of holding it on screen.
"$UMBRIEL" msg layout-scroll-right > /dev/null
wait_for_field transient-parent x -50
wait_for_centered transient-child transient-parent

# Stacked under harness-a, the parent is the most recently focused window of the column, so moving up from the dialog
# must still reach harness-a instead of returning to the parent. Coming back down lands on the dialog, and the parent
# splits back out to the right, revealed flush with the right edge.
"$UMBRIEL" msg window-consume-left > /dev/null
wait_for_field transient-parent x 10
"$UMBRIEL" msg window-focus-up > /dev/null
wait_for_field harness-a focused true
"$UMBRIEL" msg window-focus-down > /dev/null
wait_for_field transient-child focused true
"$UMBRIEL" msg window-consume-or-expel-right > /dev/null
wait_for_column_flush_right transient-parent
wait_for_centered transient-child transient-parent

# Directional focus steps from the parent's column, and coming back lands on the dialog with the parent revealed by
# the shortest move, flush with the right edge.
"$UMBRIEL" msg window-focus-left > /dev/null
wait_for_field harness-a focused true
wait_for_field harness-a x 10
"$UMBRIEL" msg window-focus-right > /dev/null
wait_for_field transient-child focused true
wait_for_column_flush_right transient-parent
wait_for_centered transient-child transient-parent

echo "a modal dialog follows its parent through the scrolling strip"
