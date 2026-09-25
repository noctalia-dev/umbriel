#!/usr/bin/env bash
# A portal-style dialog is created by another process and attached through
# xdg-foreign. A visible scratchpad parent owns that dialog's presentation,
# focus, and transient placement unless an explicit window rule says otherwise.
set -euo pipefail

readonly PARENT_CLIENT="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly CHILD_CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly PARENT_LOG="$UMBRIEL_RUNTIME_DIR/scratchpad-parent.log"
readonly CHILD_LOG="$UMBRIEL_RUNTIME_DIR/scratchpad-child.log"
readonly LATE_LOG="$UMBRIEL_RUNTIME_DIR/scratchpad-late-child.log"
readonly EXPLICIT_LOG="$UMBRIEL_RUNTIME_DIR/scratchpad-explicit-child.log"
readonly LATE_FIFO="$UMBRIEL_RUNTIME_DIR/scratchpad-late-child-control"

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
scale = 0

[[scratchpad]]
name = "portal"

[[scratchpad]]
name = "forced"

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

echo "portal dialogs inherit visible scratchpad parents and explicit placement still wins"
