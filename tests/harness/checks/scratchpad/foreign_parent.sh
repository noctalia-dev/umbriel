#!/usr/bin/env bash
# A portal-style dialog is created by another process and attached through
# xdg-foreign. A visible scratchpad parent owns that dialog's presentation,
# focus, and transient placement unless an explicit window rule says otherwise.
# The parent and the dialogs that share its pad leave and enter it as one.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly PARENT_CLIENT="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly CHILD_CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly PARENT_LOG="$UMBRIEL_RUNTIME_DIR/scratchpad-parent.log"
readonly CHILD_LOG="$UMBRIEL_RUNTIME_DIR/scratchpad-child.log"
readonly LATE_LOG="$UMBRIEL_RUNTIME_DIR/scratchpad-late-child.log"
readonly EXPLICIT_LOG="$UMBRIEL_RUNTIME_DIR/scratchpad-explicit-child.log"
readonly LATE_FIFO="$UMBRIEL_RUNTIME_DIR/scratchpad-late-child-control"
readonly FAMILY_LOG="$UMBRIEL_RUNTIME_DIR/scratchpad-family.log"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/scratchpad-drag.png"
readonly BTN_LEFT=272

windows() { "$UMBRIEL" windows --json; }

wait_for_count() {
  local expected=$1 actual=
  for _ in $(seq 80); do
    actual=$(windows | jq 'length')
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected windows, got $actual: $(windows)"
  return 1
}

wait_for_query() {
  local query=$1 message=$2
  for _ in $(seq 80); do
    windows | jq -e "$query" > /dev/null && return 0
    sleep 0.1
  done
  echo "$message: $(windows)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[animation.scratchpad]
scale = 0 # PAD_PLACEMENT

[[scratchpad]]
name = "portal"

[[scratchpad]]
name = "forced"

[[scratchpad]]
name = "family"

[[window_rule]]
match.title = "^scratchpad-foreign-parent$"
default_floating = true
default_floating_size_px = { width = 640, height = 480 }
default_position = { x = 100, y = 110, anchor = "top_left" }

[[window_rule]]
match.title = "^scratchpad-foreign-explicit$"
default_scratchpad = "forced"
default_floating = true
default_floating_size_px = { width = 300, height = 200 }
EOF
"$UMBRIEL" msg config-reload > /dev/null

EXPORT_TOPLEVEL=1 "$PARENT_CLIENT" scratchpad-foreign-parent > "$PARENT_LOG" 2>&1 &
wait_for_count 1
handle=
for _ in $(seq 80); do
  handle=$(sed -n 's/^exported handle=//p' "$PARENT_LOG")
  [[ -n $handle ]] && break
  sleep 0.1
done
if [[ -z $handle ]]; then
  echo "parent did not export an xdg-foreign handle: $(cat "$PARENT_LOG")"
  exit 1
fi

"$UMBRIEL" msg window-move-to-scratchpad:portal > /dev/null
"$UMBRIEL" msg scratchpad-toggle:portal > /dev/null
wait_for_query \
  '[.[] | select(.title == "scratchpad-foreign-parent" and .scratchpad == "portal" and .active)] | length == 1' \
  "scratchpad parent did not become visible and focused"

# The foreign parent is set before the child's initial commit, matching portal
# backends. The child must move from its provisional workspace into the visible
# pad, take seat focus, and center over the 640x480 parent at 100,110.
TRANSIENT_FOREIGN_HANDLE="$handle" \
  "$CHILD_CLIENT" scratchpad-foreign-child 400 300 > "$CHILD_LOG" 2>&1 &
wait_for_count 2
wait_for_query \
  '[.[] | select(.title == "scratchpad-foreign-child" and .scratchpad == "portal" and .workspace == "" and .active and .x == 220 and .y == 200)] | length == 1' \
  "portal child did not inherit its visible parent's scratchpad, focus, and placement"

# Membership owns visibility too. Hiding and showing the pad must bring the
# dialog back as its last focused member.
"$UMBRIEL" msg scratchpad-toggle:portal > /dev/null
wait_for_query '[.[] | select(.active)] | length == 0' "hiding the inherited dialog did not clear focus"
"$UMBRIEL" msg scratchpad-toggle:portal > /dev/null
wait_for_query \
  '[.[] | select(.title == "scratchpad-foreign-child" and .scratchpad == "portal" and .active)] | length == 1' \
  "showing the scratchpad did not restore focus to the inherited dialog"

# Some clients establish their parent after mapping. Exercise the set-parent
# event rather than relying only on the opening state.
mkfifo "$LATE_FIFO"
exec {late_fd}<>"$LATE_FIFO"
TRANSIENT_FOREIGN_HANDLE="$handle" TRANSIENT_FOREIGN_PARENT_ON_STDIN=1 \
  "$CHILD_CLIENT" scratchpad-foreign-late 400 300 <&"$late_fd" > "$LATE_LOG" 2>&1 &
wait_for_count 3
wait_for_query \
  '[.[] | select(.title == "scratchpad-foreign-late" and .scratchpad == "" and .workspace != "" and .active)] | length == 1' \
  "late-parent fixture did not begin on the workspace"
printf p >&"$late_fd"
wait_for_query \
  '[.[] | select(.title == "scratchpad-foreign-late" and .scratchpad == "portal" and .workspace == "" and .active and .x == 220 and .y == 200)] | length == 1' \
  "late parent request did not move the dialog into the visible scratchpad"

# A configured scratchpad is an explicit placement decision and wins over
# inherited parent placement. The forced pad stays hidden, so the late child
# must also retain seat focus.
TRANSIENT_FOREIGN_HANDLE="$handle" \
  "$CHILD_CLIENT" scratchpad-foreign-explicit 300 200 > "$EXPLICIT_LOG" 2>&1 &
wait_for_count 4
wait_for_query \
  '[.[] | select(.title == "scratchpad-foreign-explicit" and .scratchpad == "forced" and .workspace == "")] | length == 1' \
  "explicit scratchpad rule did not override parent inheritance"
wait_for_query \
  '[.[] | select(.title == "scratchpad-foreign-late" and .active)] | length == 1' \
  "hidden explicitly assigned dialog stole focus"

# The explicitly placed dialog is no part of the family in the other pad: restoring it takes out only that window.
"$UMBRIEL" msg scratchpad-toggle:forced > /dev/null
"$UMBRIEL" msg window-restore-from-scratchpad:forced > /dev/null
wait_for_query \
  '[.[] | select(.title == "scratchpad-foreign-explicit" and .scratchpad == "" and .workspace != "")] | length == 1' \
  "the explicitly placed dialog did not leave its own pad"
wait_for_query '[.[] | select(.scratchpad == "portal")] | length == 3' \
  "restoring the dialog in its own pad took the other pad's family along"

# Restoring one dialog of the family brings back the parent and its other dialog. Back on the parent's workspace, the
# explicitly placed dialog belongs to the family too, so moving the family into a pad through a dialog takes all four.
"$UMBRIEL" msg "window-focus:$(windows | jq -r '.[] | select(.title == "scratchpad-foreign-child") | .id')" > /dev/null
"$UMBRIEL" msg window-restore-from-scratchpad:portal > /dev/null
wait_for_query '[.[] | select(.scratchpad == "" and .workspace != "")] | length == 4' \
  "restoring a dialog did not bring its family out of the pad"
"$UMBRIEL" msg "window-focus:$(windows | jq -r '.[] | select(.title == "scratchpad-foreign-late") | .id')" > /dev/null
"$UMBRIEL" msg window-move-to-scratchpad:portal > /dev/null
wait_for_query '[.[] | select(.scratchpad == "portal")] | length == 4' \
  "moving a dialog into the pad did not take its family along"

# Dragging the shown parent lifts its dialogs into the drag with it, so they stay drawn over it. The parent's own
# #3388CC and the dialogs' #5577AA differ in red; the sample is the dialogs' middle, moved with the drag.
"$UMBRIEL" msg scratchpad-toggle:portal > /dev/null
wait_for_query '[.[] | select(.scratchpad == "portal" and .active)] | length == 1' "showing the family's pad focused none of it"
windows=$(windows)
grab_x=$(jq -r '.[] | select(.title == "scratchpad-foreign-parent") | .x + 30' <<< "$windows")
grab_y=$(jq -r '.[] | select(.title == "scratchpad-foreign-parent") | .y + 30' <<< "$windows")
sample_x=$(jq -r '.[] | select(.title == "scratchpad-foreign-child") | .x + .w / 2 + 20 | floor' <<< "$windows")
sample_y=$(jq -r '.[] | select(.title == "scratchpad-foreign-child") | .y + .h / 2 + 20 | floor' <<< "$windows")
pointer_hold 1280 720 move "$grab_x" "$grab_y" mod logo press "$BTN_LEFT" move $((grab_x + 40)) $((grab_y + 40))
"$UMBRIEL" settle
grim "$SHOT"
pointer_release
dialog_pixels=$("$UMBRIEL_PIXEL_PROBE" "$SHOT" count 'r > 0.28 && b < 0.72' "20x20+$sample_x+$sample_y")
if (( dialog_pixels < 300 )); then
  echo "the dragged parent covered its dialog: $dialog_pixels of 400 sampled pixels show the dialog"
  exit 1
fi

# A tiled parent with its modal dialog goes into a pad through the dialog and comes back to its tile, with the dialog
# over it and focused.
TRANSIENT_SUITE=1 TRANSIENT_MODAL=1 TRANSIENT_PARENT_SIZE=800x600 \
  "$CHILD_CLIENT" transient-child 400 300 > "$FAMILY_LOG" 2>&1 &
family=$!
wait_for_count 7
wait_for_query '[.[] | select(.title == "transient-child" and .focused)] | length == 1' "the modal dialog did not take focus"
wait_for_query '[.[] | select(.title == "transient-parent" and .floating == false)] | length == 1' \
  "the dialog's parent did not open tiled"
"$UMBRIEL" msg window-move-to-scratchpad:family > /dev/null
wait_for_query '[.[] | select(.scratchpad == "family")] | length == 2' "the dialog went into the pad without its parent"
wait_for_query '[.[] | select(.title == "transient-unrelated" and .scratchpad == "")] | length == 1' \
  "the application's unrelated window followed the family into the pad"
"$UMBRIEL" msg scratchpad-toggle:family > /dev/null
wait_for_query '[.[] | select(.title == "transient-child" and .active)] | length == 1' \
  "showing the pad did not hand the parent's focus to its dialog"
"$UMBRIEL" msg window-restore-from-scratchpad:family > /dev/null
wait_for_query '[.[] | select(.scratchpad == "family")] | length == 0' "restoring the dialog left part of its family"
wait_for_query '[.[] | select(.title == "transient-parent" and .floating == false)] | length == 1' \
  "the parent did not return to its tile"
wait_for_query '[.[] | select(.title == "transient-child" and .focused)] | length == 1' \
  "the restored dialog did not keep the focus"

# Restoring through a dialog that is not modal leaves the focus on that dialog too; with nothing to block the parent,
# no modal dialog hands it over.
kill "$family"
wait_for_count 4
TRANSIENT_SUITE=1 TRANSIENT_PARENT_SIZE=800x600 "$CHILD_CLIENT" transient-child 400 300 > "$FAMILY_LOG" 2>&1 &
family=$!
wait_for_count 7
wait_for_query '[.[] | select(.title == "transient-child" and .focused)] | length == 1' "the dialog did not take focus"
"$UMBRIEL" msg window-move-to-scratchpad:family > /dev/null
wait_for_query '[.[] | select(.scratchpad == "family")] | length == 2' "the dialog went into the pad without its parent"
"$UMBRIEL" msg "window-focus:$(windows | jq -r '.[] | select(.title == "transient-child") | .id')" > /dev/null
wait_for_query '[.[] | select(.title == "transient-child" and .active)] | length == 1' \
  "focusing the dialog in the pad did not make it active"
"$UMBRIEL" msg window-restore-from-scratchpad:family > /dev/null
wait_for_query '[.[] | select(.scratchpad == "family")] | length == 0' "restoring the dialog left part of its family"
wait_for_query '[.[] | select(.title == "transient-child" and .focused)] | length == 1' \
  "restoring the dialog handed the focus to its parent"

# With pads showing their windows fullscreen, the parent goes fullscreen and its modal dialog stays drawn over it. The
# 200x100 parent shows letterboxed on the black fullscreen backdrop, and the 400x300 dialog centered on it reaches past
# it: the sample is the dialog's top-left corner, clear of the parent.
kill "$family"
wait_for_count 4
sed -i 's/^scale = 0 # PAD_PLACEMENT$/fullscreen = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
TRANSIENT_SUITE=1 TRANSIENT_MODAL=1 TRANSIENT_PARENT_SIZE=200x100 "$CHILD_CLIENT" transient-child 400 300 \
  > "$FAMILY_LOG" 2>&1 &
wait_for_count 7
wait_for_query '[.[] | select(.title == "transient-child" and .focused)] | length == 1' "the modal dialog did not take focus"
"$UMBRIEL" msg window-move-to-scratchpad:family > /dev/null
wait_for_query '[.[] | select(.scratchpad == "family")] | length == 2' "the dialog went into the pad without its parent"
"$UMBRIEL" msg scratchpad-toggle:family > /dev/null
wait_for_query '[.[] | select(.title == "transient-child" and .active)] | length == 1' \
  "showing the pad did not hand the fullscreen parent's focus to its dialog"
"$UMBRIEL" settle
grim "$SHOT"
dialog_pixels=$("$UMBRIEL_PIXEL_PROBE" "$SHOT" count 'r > 0.28 && b > 0.6' "20x20+460+230")
if (( dialog_pixels < 300 )); then
  echo "the fullscreen parent covered its modal dialog: $dialog_pixels of 400 sampled pixels show the dialog"
  exit 1
fi

echo "portal dialogs share their scratchpad parent's pad and move with it, and explicit placement still wins"
