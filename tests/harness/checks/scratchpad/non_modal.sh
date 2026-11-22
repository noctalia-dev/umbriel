#!/usr/bin/env bash
# A [[scratchpad]] with modal = false leaves the windows behind it usable when shown: a click reaches them and takes
# focus while the pad stays shown, a click on empty space leaves it shown, and a workspace switch still closes it,
# however it happens. Closing the window behind gives the keyboard back to the pad. A pad without it stays modal, and a
# reload that turns the setting off or on frees a pad already shown or closes it.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly BTN_LEFT=272
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly BEHIND=non-modal-behind
readonly FREE=non-modal-free
readonly STRICT=non-modal-strict
readonly OTHER=non-modal-other
readonly BEHIND_LOG="$UMBRIEL_RUNTIME_DIR/$BEHIND.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[input.focus]
follows_mouse = "off"

[[scratchpad]]
name = "free"
modal = false

[[scratchpad]]
name = "strict"

[[window_rule]]
match.title = "^non-modal-behind$"
default_floating = true
default_floating_size_px = { width = 300, height = 200 }
default_position = { x = 50, y = 50, anchor = "top_left" }

[[window_rule]]
match.title = "^non-modal-(free|strict)$"
default_floating = true
default_floating_size_px = { width = 420, height = 260 }
default_position = { x = 430, y = 230, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

windows() { "$UMBRIEL" windows --json; }

field_of() {
  windows | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

wait_for_field() {
  local title=$1 field=$2 expected=$3 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$title" "$field")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.05
  done
  echo "expected '$title' field '$field' to be '$expected', got '$actual': $(windows)"
  exit 1
}

show() {
  "$UMBRIEL" msg scratchpad-toggle "$1" > /dev/null
  wait_for_field "$2" visible true
  wait_for_field "$2" active true
}

"$UMBRIEL_SEAT_LOG_CLIENT" "$BEHIND" > "$BEHIND_LOG" 2>&1 &
wait_for_field "$BEHIND" active true
for pad in free strict; do
  title=non-modal-$pad
  "$CLIENT" "$title" 420 260 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  wait_for_field "$title" active true
  "$UMBRIEL" msg window-move-to-scratchpad "$pad" > /dev/null
  wait_for_field "$title" scratchpad "$pad"
done

# A click on the window behind reaches it and focuses it; the pad stays shown.
show free "$FREE"
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 150 150 click "$BTN_LEFT"
wait_for_field "$BEHIND" active true
grep -q "pointer-button code=$BTN_LEFT state=pressed" "$BEHIND_LOG" || {
  echo "the click did not reach the window behind the scratchpad: $(cat "$BEHIND_LOG")"
  exit 1
}
[[ $(field_of "$FREE" visible) == true ]] || {
  echo "focusing the window behind closed a non-modal scratchpad: $(windows)"
  exit 1
}

# A click on empty space leaves it shown too, and leaves focus behind it.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 1200 650 click "$BTN_LEFT"
"$UMBRIEL" settle
[[ $(field_of "$FREE" visible) == true && $(field_of "$BEHIND" active) == true ]] || {
  echo "a click on empty space closed a non-modal scratchpad or moved focus into it: $(windows)"
  exit 1
}

# A workspace switch still closes it, including one that focusing a window on another workspace makes.
"$UMBRIEL" msg workspace-switch 2 > /dev/null
wait_for_field "$FREE" visible false
"$CLIENT" "$OTHER" 300 200 > "$UMBRIEL_RUNTIME_DIR/$OTHER.log" 2>&1 &
wait_for_field "$OTHER" active true
"$UMBRIEL" msg workspace-switch 1 > /dev/null
show free "$FREE"
"$UMBRIEL" msg window-focus "$(field_of "$OTHER" id)" > /dev/null
wait_for_field "$OTHER" active true
wait_for_field "$FREE" visible false
"$UMBRIEL" msg workspace-switch 1 > /dev/null

# A pad without modal = false stays modal: a click behind it closes it.
show strict "$STRICT"
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 150 150 click "$BTN_LEFT"
wait_for_field "$STRICT" visible false

# A pad shown while modal drops its backdrop when a reload turns the setting off, so the click reaches the window behind.
sed -i 's/^modal = false$/modal = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
show free "$FREE"
sed -i 's/^modal = true$/modal = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 150 150 click "$BTN_LEFT"
wait_for_field "$BEHIND" active true
presses=$(grep -c "pointer-button code=$BTN_LEFT state=pressed" "$BEHIND_LOG")
[[ $presses == 2 && $(field_of "$FREE" visible) == true ]] || {
  echo "after the reload the click did not reach the window behind ($presses presses): $(windows)"
  exit 1
}

# A reload that makes the pad modal while the window behind it has focus closes it, as that focus would.
sed -i 's/^modal = false$/modal = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
wait_for_field "$FREE" visible false
sed -i 's/^modal = true$/modal = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

# Closing the window behind leaves the pad as the only thing to focus there.
show free "$FREE"
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 150 150 click "$BTN_LEFT"
wait_for_field "$BEHIND" active true
kill "$(field_of "$BEHIND" pid)"
wait_for_field "$FREE" active true

echo "a non-modal scratchpad left the windows behind it usable, and modal = true kept its pad modal"
