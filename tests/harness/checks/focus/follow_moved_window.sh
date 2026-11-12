#!/usr/bin/env bash
# follows_focus carries the pointer along with the focused window: a swap or a floating move puts the pointer at the
# window's center. A new window that takes focus away from the pointer leaves the pointer where it is, so
# a launcher click can be followed by another.
set -euo pipefail

readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly SEAT_LOG="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"

windows() { "$UMBRIEL" windows --json; }
window() { windows | jq -c --arg title "$1" '.[] | select(.title == $title)'; }
field() { window "$1" | jq -r ".$2"; }
pointer() { "$POINTER" 1280 720 "$@"; }

spawn() {
  LOG_POINTER_INPUT=1 "$SEAT_LOG" "$1" > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 60); do
    [[ -n $(window "$1") ]] && return 0
    sleep 0.1
  done
  echo "window $1 did not map: $(windows)"
  exit 1
}

wait_for_active() {
  for _ in $(seq 60); do
    [[ $(field "$1" active) == true ]] && return 0
    sleep 0.05
  done
  echo "expected $1 to be active: $(windows)"
  exit 1
}

# The surface-local position of the last pointer event the window received.
last_motion() {
  sed -n 's/^pointer-motion x=\([-0-9.]*\) y=\([-0-9.]*\)$/\1 \2/p' "$UMBRIEL_RUNTIME_DIR/$1.log" | tail -1
}

expect_motion_at() {
  local title=$1 want_x=$2 want_y=$3 context=$4 x y
  read -r x y <<< "$(last_motion "$title")"
  if [[ -z $x ]] || (($(printf '%.0f' "$x") != want_x || $(printf '%.0f' "$y") != want_y)); then
    echo "$context: expected the pointer at $want_x,$want_y in $title, last motion was '${x:-none} ${y:-}'"
    exit 1
  fi
}

events() { grep -c "^$2$" "$UMBRIEL_RUNTIME_DIR/$1.log" || true; }

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "dwindle"

[animation]
enabled = false

[input.cursor]
follows_focus = true

[[window_rule]]
match.title = "^follow-float-"
default_workspace = 2
default_floating = true
default_floating_size_px = { width = 300, height = 200 }

[[window_rule]]
match.title = "^follow-float-kept$"
default_position = { x = 100, y = 100, anchor = "top_left" }

[[window_rule]]
match.title = "^follow-float-new$"
default_position = { x = 800, y = 400, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

# A swap carries the focused tile to its neighbor's place; the pointer keeps its spot inside it.
spawn follow-left
spawn follow-right
"$UMBRIEL" settle
"$UMBRIEL" msg window-focus "$(field follow-left id)" > /dev/null
wait_for_active follow-left
# Enter carries no logged position, so finish with a motion inside the window.
pointer move "$(($(field follow-left x) + 31))" "$(($(field follow-left y) + 40))" \
  move "$(($(field follow-left x) + 30))" "$(($(field follow-left y) + 40))"
"$UMBRIEL" settle
expect_motion_at follow-left 30 40 "before the swap"
left_x=$(field follow-left x)
left_leaves=$(events follow-left pointer-leave)
"$UMBRIEL" msg window-swap-right > /dev/null
"$UMBRIEL" settle
[[ $(field follow-left x) != "$left_x" ]] || { echo "window-swap-right did not move follow-left: $(windows)"; exit 1; }
wait_for_active follow-left
(($(events follow-left pointer-leave) == left_leaves)) || { echo "the pointer stayed behind after the swap"; exit 1; }
expect_motion_at follow-left "$(($(field follow-left w) / 2))" "$(($(field follow-left h) / 2))" "after the swap"

# A floating move does the same.
"$UMBRIEL" msg workspace-switch 2 > /dev/null
spawn follow-float-kept
"$UMBRIEL" settle
wait_for_active follow-float-kept
pointer move 151 160 move 150 160
"$UMBRIEL" settle
expect_motion_at follow-float-kept 50 60 "before the floating move"
kept_leaves=$(events follow-float-kept pointer-leave)
"$UMBRIEL" msg window-move-by 200 120 > /dev/null
"$UMBRIEL" settle
[[ $(field follow-float-kept x) == 300 ]] || { echo "window-move-by did not move follow-float-kept: $(windows)"; exit 1; }
(($(events follow-float-kept pointer-leave) == kept_leaves)) || { echo "the pointer stayed behind after the move"; exit 1; }
expect_motion_at follow-float-kept 150 100 "after the floating move"

# A window that opens away from the pointer takes focus without pulling the pointer along.
kept_leaves=$(events follow-float-kept pointer-leave)
spawn follow-float-new
"$UMBRIEL" settle
wait_for_active follow-float-new
(($(events follow-float-new pointer-enter) == 0)) || { echo "the pointer followed the new window"; exit 1; }
(($(events follow-float-kept pointer-leave) == kept_leaves)) || { echo "the pointer left follow-float-kept"; exit 1; }

echo "follows_focus carries the pointer with swaps and floating moves, and stays put when a window opens"
