#!/usr/bin/env bash
# A dialog that turns modal after it mapped takes its parent's focus and shades it, and one that turns back, or drops
# its xdg-dialog-v1 object, frees the parent again.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/modal-dialog-late.log"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/modal-dialog-late.fifo"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/modal-dialog-late.png"

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

# Mean red of a 20x20 patch of the parent, clear of the dialog. Its pixels are #5577AA, so red reads 85 unshaded and
# about 60 under the 30% black shade a modal dialog puts over it.
parent_red() {
  sleep 0.3
  grim "$SHOT"
  magick "$SHOT" -crop 20x20+20+520 -format '%[fx:round(255*mean.r)]' info:
}

expect_shaded() {
  local red
  red=$(parent_red)
  if (( red > 72 )); then
    echo "$1: the parent is not shaded, red=$red"
    exit 1
  fi
}

expect_unshaded() {
  local red
  red=$(parent_red)
  if (( red < 72 )); then
    echo "$1: the parent is still shaded, red=$red"
    exit 1
  fi
}

# Hovering the application's other window, then the parent, focuses each of them only while no modal dialog blocks.
expect_parent_free() {
  "$POINTER" 1280 720 move 1000 600
  wait_for_field transient-unrelated focused true
  "$POINTER" 1280 720 move 20 520
  wait_for_field transient-parent focused true
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

# The parent covers 0,0 to 800,600 and the unrelated window 880,420 to 1280,720. The dialog has an xdg-dialog-v1
# object but no modal flag yet, so it is an ordinary transient and blocks nothing.
mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
TRANSIENT_SUITE=1 TRANSIENT_DIALOG_ON_STDIN=1 TRANSIENT_PARENT_SIZE=800x600 "$CLIENT" transient-child 400 300 \
  <&"$control_fd" > "$CLIENT_LOG" 2>&1 &
wait_for_window_count 3
wait_for_field transient-parent x 0
wait_for_field transient-unrelated x 880
expect_parent_free
expect_unshaded "before set_modal"

# set_modal on the mapped dialog attaches it: the focused parent hands its focus over and is shaded.
printf 'm' >&"$control_fd"
wait_for_field transient-child focused true
wait_for_centered transient-child transient-parent
expect_shaded "after set_modal"

# unset_modal frees the parent and lifts the shade.
printf 'n' >&"$control_fd"
expect_unshaded "after unset_modal"
expect_parent_free

# Destroying the xdg-dialog-v1 object undoes its modality the same way.
printf 'm' >&"$control_fd"
wait_for_field transient-child focused true
expect_shaded "after the second set_modal"
printf 'x' >&"$control_fd"
expect_unshaded "after the dialog object was destroyed"
expect_parent_free

echo "a dialog's modality follows set_modal, unset_modal, and the dialog object's destruction after map"
