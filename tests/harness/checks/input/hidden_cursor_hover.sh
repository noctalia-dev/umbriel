#!/usr/bin/env bash
# A cursor hidden by typing must stop interacting: the surface under it receives pointer-leave, a compositor warp
# onto another window while hidden must not hand it pointer-enter, and the next real motion shows the cursor and
# restores hover there.
set -euo pipefail

readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly SEAT_LOG="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly KEY_A=30

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

events() { grep -c "^$2" "$UMBRIEL_RUNTIME_DIR/$1.log" || true; }

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[input.cursor]
hide_when_typing = true
hide_timeout_ms = 0
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn hidden-hover-a
spawn hidden-hover-b
"$UMBRIEL" settle

# Hover window A so it owns pointer focus, then hide the cursor with an unbound key.
read -r a_x a_y < <(windows | jq -r '.[] | select(.title == "hidden-hover-a") | "\(.x + .w / 2 | floor) \(.y + .h / 2 | floor)"')
pointer move "$a_x" "$a_y"
"$UMBRIEL" settle
[[ $(events hidden-hover-a pointer-enter) -ge 1 ]] || { echo "window A never received pointer-enter"; exit 1; }

pointer tap "$KEY_A"
"$UMBRIEL" settle
[[ $(events hidden-hover-a pointer-leave) -ge 1 ]] || { echo "hiding the cursor kept pointer focus on window A"; exit 1; }

# A compositor warp while hidden must not hand pointer focus to the window it lands on.
"$UMBRIEL" msg window-focus-warp "$(field hidden-hover-b id)" > /dev/null
"$UMBRIEL" settle
if [[ $(events hidden-hover-b pointer-enter) -ne 0 || $(events hidden-hover-b pointer-motion) -ne 0 ]]; then
  echo "the hidden cursor interacted with window B after the warp: $(cat "$UMBRIEL_RUNTIME_DIR/hidden-hover-b.log")"
  exit 1
fi

# Real motion shows the cursor again and restores hover at its new position.
pointer relative 3 2
"$UMBRIEL" settle
[[ $(events hidden-hover-b pointer-enter) -ge 1 ]] || { echo "motion did not restore pointer focus on window B"; exit 1; }

echo "a hidden cursor releases pointer focus and ignores warps until pointer activity restores it"
