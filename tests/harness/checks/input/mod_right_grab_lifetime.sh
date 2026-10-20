#!/usr/bin/env bash
# The deferred Mod+right resize is a grab state: it holds its initiating
# button, so a second button cannot start a competing operation, every release
# path ends it, and nothing stale promotes once all buttons are up. The
# sequence under test: Mod+right press on an edge zone, Mod+left press, release
# right, drag with left past the slop, release left — then motion with no
# buttons held must not resize the window.
set -euo pipefail

readonly BTN_LEFT=272 # evdev BTN_LEFT
readonly BTN_RIGHT=273 # evdev BTN_RIGHT
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

if [[ ! -x $POINTER ]]; then
  echo "input helpers are not available"
  exit 1
fi

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

window_box() {
  "$UMBRIEL" windows --json | jq -r '.[0] | "\(.x) \(.y) \(.w) \(.h)"'
}

# Geometry must be readable without settle: it waits for a drawn frame and
# wedges while a button is held, which is exactly the state under test.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[input]
mod_right_double_click = "maximize"
EOF
"$UMBRIEL" msg config-reload > /dev/null

foot sh -c 'sleep 120' > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.25
done
"$UMBRIEL" settle

# The press lands in the left third so it proposes a resize edge. The whole
# sequence must leave the window alone: the deferred press holds its button, so
# the Mod+left press is swallowed and the drag commits nothing. The floating
# round matters: a tiled window snapped back to its column after an unwanted
# move-grab would look unchanged.
repro_round() {
  local kind=$1 windows x y before after
  windows=$("$UMBRIEL" windows --json)
  x=$(jq -r '.[0] | "\(.x + (.w * 0.2 | floor))"' <<< "$windows")
  y=$(jq -r '.[0] | "\(.y + (.h / 2 | floor))"' <<< "$windows")
  before=$(window_box)
  pointer mod logo \
    move "$x" "$y" \
    press "$BTN_RIGHT" press "$BTN_LEFT" \
    release "$BTN_RIGHT" \
    move $((x + 40)) "$y" \
    release "$BTN_LEFT" mod none
  "$UMBRIEL" settle

  # Motion with no buttons held must not resize anything either. A grab that
  # outlives its button resizes during the motions themselves; poll so a late
  # configure cannot hide it.
  pointer move $((x + 120)) "$y" move $((x + 200)) "$y"
  for _ in $(seq 20); do
    after=$(window_box)
    if [[ $after != "$before" ]]; then
      echo "$kind: the deferred press outlived its button and moved or resized the window:"
      echo "before: $before"
      echo "after:  $after"
      return 1
    fi
    sleep 0.1
  done
  return 0
}

failed=0
repro_round tiled || failed=1
"$UMBRIEL" msg window-toggle-floating > /dev/null
"$UMBRIEL" settle
repro_round floating || failed=1

if (( failed != 0 )); then
  exit 1
fi

echo "the deferred Mod+right press ends with its button release"
