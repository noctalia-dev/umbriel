#!/usr/bin/env bash
# While a modal dialog is attached, its parent's keyboard and pointer input go to the dialog and the parent is shaded.
# Dropping the modal hint or the dialog object frees the parent, and setting it again attaches the dialog again. The
# overview shows the dialog over its parent's card. `general.attach_modal_dialogs = false` and an `attach_to_parent =
# false` rule on the dialog or on its parent opt out on reload; the dialog's own rule wins over its parent's. Closing
# the parent while the dialog is open leaves the dialog as an ordinary window.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CONTROL="$UMBRIEL_RUNTIME_DIR/modal-input-control"
readonly BASE_CONFIG="$UMBRIEL_RUNTIME_DIR/modal-input-base.toml"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/modal-input.png"
readonly BTN_LEFT=272

windows() { "$UMBRIEL" windows --json; }
field() { windows | jq -r --arg title "$1" ".[] | select(.title == \$title) | .$2"; }
active() { windows | jq -r '.[] | select(.active) | .title'; }

expect_active() {
  local want=$1 label=$2
  for _ in $(seq 80); do
    [[ $(active) == "$want" ]] && return 0
    sleep 0.05
  done
  echo "$label: expected $want to hold the keyboard, got [$(active)]: $(windows)"
  exit 1
}

# Focusing the parent lands on the dialog exactly while the dialog is attached.
expect_attached() {
  "$UMBRIEL" msg window-focus "$(field transient-parent id)" > /dev/null
  expect_active modal-dialog "$1"
}

expect_detached() {
  "$UMBRIEL" msg window-focus "$(field transient-parent id)" > /dev/null
  expect_active transient-parent "$1"
}

# The parent's own colour, 0xFF5577AA, at a point the dialog does not cover.
parent_pixel() {
  "$UMBRIEL" settle
  grim "$IMAGE"
  "$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 130 130
}

expect_parent_colour() {
  local label=$1 want_blue=$2 red green blue
  read -r red green blue < <(parent_pixel)
  if ((blue < want_blue - 8 || blue > want_blue + 8)); then
    echo "$label: expected the parent's blue near $want_blue, got $red $green $blue"
    exit 1
  fi
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[[window_rule]]
match.title = "^transient-parent$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF
cp "$UMBRIEL_CONFIG" "$BASE_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$CONTROL"
exec {control_fd}<>"$CONTROL"
TRANSIENT_SUITE=1 TRANSIENT_MODAL=1 TRANSIENT_DIALOG_ON_STDIN=1 REPAINT_ON_STDIN=1 TRANSIENT_PARENT_SIZE=640x480 \
  "$CLIENT" modal-dialog 200 150 <&"$control_fd" > "$UMBRIEL_RUNTIME_DIR/modal-input.log" 2>&1 &
for _ in $(seq 80); do
  [[ $(windows | jq 'length') == 3 ]] && break
  sleep 0.05
done

# Keyboard: focusing the parent focuses its dialog, and the parent is shaded by animation.dialogs.dim (0.3).
expect_attached "keyboard"
expect_parent_colour "shaded" $((170 * 7 / 10))

# Pointer: a press on the parent, clear of the dialog, focuses the dialog.
"$UMBRIEL" msg window-focus "$(field transient-unrelated id)" > /dev/null
expect_active transient-unrelated "focusing the unrelated window"
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 130 130 press "$BTN_LEFT" release "$BTN_LEFT" > /dev/null
expect_active modal-dialog "pointer press on the parent"

# The overview mirrors the red dialog over its parent's card: at zoom 0.5 the preview starts at 320,180.
read -r dialog_x dialog_y < <(windows | jq -r '.[] | select(.title == "modal-dialog") | "\(.x + .w / 2 | floor) \(.y + .h / 2 | floor)"')
printf R >&"$control_fd"
red=0
for _ in $(seq 80); do
  grim "$IMAGE"
  read -r red _ _ < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$dialog_x" "$dialog_y")
  ((red > 200)) && break
  sleep 0.05
done
if ((red <= 200)); then
  echo "the dialog never drew red at $dialog_x,$dialog_y"
  exit 1
fi
card_x=$((320 + dialog_x / 2))
card_y=$((180 + dialog_y / 2))
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
grim "$IMAGE"
read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel "$card_x" "$card_y")
if ((red < 200 || green > 40 || blue > 40)); then
  echo "the overview did not show the dialog over its parent's card at $card_x,$card_y: $red $green $blue"
  exit 1
fi
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" settle

# unset_modal frees the parent and lifts the shade; set_modal attaches the dialog again.
printf n >&"$control_fd"
expect_detached "unset_modal"
expect_parent_colour "unshaded" 170
printf m >&"$control_fd"
expect_attached "set_modal again"

# The global switch opts out on reload, and back in.
sed 's/^\[general\]$/[general]\nattach_modal_dialogs = false/' "$BASE_CONFIG" > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
expect_detached "attach_modal_dialogs = false"
cp "$BASE_CONFIG" "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
expect_attached "attach_modal_dialogs back on"

# So does a rule selecting dialogs.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.is_dialog = true
attach_to_parent = false
EOF
"$UMBRIEL" msg config-reload > /dev/null
expect_detached "attach_to_parent = false"
cp "$BASE_CONFIG" "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
expect_attached "rule removed"

# A rule on the parent covers its dialogs, as an application's rule must cover the portal dialogs it opens.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.title = "^transient-parent$"
attach_to_parent = false
EOF
"$UMBRIEL" msg config-reload > /dev/null
expect_detached "attach_to_parent = false on the parent"

# The dialog's own rule decides over its parent's.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.is_dialog = true
attach_to_parent = true
EOF
"$UMBRIEL" msg config-reload > /dev/null
expect_attached "attach_to_parent = true on the dialog"
cp "$BASE_CONFIG" "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
expect_attached "parent rule removed"

# Destroying the dialog object takes its modality with it.
printf x >&"$control_fd"
expect_detached "xdg_dialog_v1 destroyed"

# Closing a parent with its dialog attached leaves the dialog as an ordinary window that keeps the keyboard.
TRANSIENT_SUITE=1 TRANSIENT_MODAL=1 TRANSIENT_PARENT_SIZE=640x480 "$CLIENT" modal-orphan 200 150 \
  > "$UMBRIEL_RUNTIME_DIR/modal-orphan.log" 2>&1 &
for _ in $(seq 80); do
  [[ $(windows | jq '[.[] | select(.title == "modal-orphan")] | length') == 1 ]] && break
  sleep 0.05
done
expect_active modal-orphan "the second dialog opening"
parent_id=$(windows | jq -r '[.[] | select(.title == "transient-parent")] | last | .id')
"$UMBRIEL" msg window-close "$parent_id" > /dev/null
for _ in $(seq 80); do
  windows | jq -e --arg id "$parent_id" \
    '([.[] | select(.id == $id)] | length == 0)
     and ([.[] | select(.title == "modal-orphan" and .workspace != "" and .visible and .active)] | length == 1)' \
    > /dev/null && break
  sleep 0.05
done
if ! windows | jq -e '[.[] | select(.title == "modal-orphan" and .workspace != "" and .visible and .active)] | length == 1' \
  > /dev/null; then
  echo "the dialog of a closed parent did not stay as an ordinary focused window: $(windows)"
  exit 1
fi

echo "an attached dialog took its parent's input and shade, showed over its card, and let go when asked or orphaned"
