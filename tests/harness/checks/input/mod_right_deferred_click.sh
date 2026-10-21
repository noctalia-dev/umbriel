#!/usr/bin/env bash
# With mod_right_double_click = "maximize", a Mod+right click behaves like the
# default one until it resizes: it reveals the partly off-screen column it
# lands on, and motion inside the click slop does not reach focus-follows-mouse.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272 # evdev BTN_LEFT
readonly BTN_RIGHT=273 # evdev BTN_RIGHT
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[layout]
gap = 0

[input]
mod_right_double_click = "maximize"
EOF
"$UMBRIEL" msg config-reload > /dev/null

await_count() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $1 ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for $1 window(s)"
  return 1
}

field() {
  "$UMBRIEL" windows --json | jq -r --arg t "$1" ".[] | select(.title == \$t) | .$2"
}

focused() {
  "$UMBRIEL" windows --json | jq -r '.[] | select(.focused) | .title'
}

foot --title A sh -c 'sleep 120' > /dev/null 2>&1 &
await_count 1
foot --title B sh -c 'sleep 120' > /dev/null 2>&1 &
await_count 2
"$UMBRIEL" settle

failed=0

# B is focused and A sits partly off the left edge. A single Mod+right click on
# A's visible part must bring A fully into view.
ay=$(field A y)
ah=$(field A h)
y=$((ay + ah / 2))
if (($(field A x) >= 0)); then
  echo "setup: A is not partly off-screen: $("$UMBRIEL" windows --json | jq -c 'map([.title, .x])')"
  exit 1
fi
pointer move 300 "$y" mod logo click "$BTN_RIGHT" mod none
"$UMBRIEL" settle
if [[ $(focused) != A ]] || (($(field A x) < 0)); then
  echo "Mod+right click did not reveal A: $("$UMBRIEL" windows --json | jq -c 'map([.title, .x, .focused])')"
  failed=1
fi

# With focus-follows-mouse on and A focused, press Mod+right 3 px inside A's
# right edge and move into B without leaving the slop: focus must stay on A.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[input.focus]
follows_mouse = true
EOF
"$UMBRIEL" msg config-reload > /dev/null
pointer move 300 "$y"
"$UMBRIEL" settle
x=$(($(field A x) + $(field A w) - 3))
pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  move "$x" "$y" mod logo press "$BTN_RIGHT" move $((x + 7)) "$y" \
  -- release "$BTN_RIGHT" mod none
if [[ $(focused) != A ]]; then
  echo "motion inside the Mod+right slop moved focus to $(focused)"
  failed=1
fi
pointer_release

if ((failed != 0)); then
  exit 1
fi
echo "a deferred Mod+right click reveals its column and holds in-slop motion"
