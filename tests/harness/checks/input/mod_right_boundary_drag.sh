#!/usr/bin/env bash
# The deferred Mod+right resize must grab the edge the PRESS resolved: the
# thirds zones make the proposal position-sensitive, and the pointer has moved
# by the time the drag commits. A drag starting just inside an outer third and
# moving toward the centre crosses the boundary and must still resize; a
# centre-third press crossing outward must not start a resize at all.
set -euo pipefail

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

# The deferred grab belongs to the opt-in gesture; enable it for the check.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

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

# Drags the border between the left third and the centre third in 4px steps,
# starting at offset $1 from the boundary ($1 < 0: inside the outer third).
drag_across_boundary() {
  local offset=$1 windows base_x y x
  windows=$("$UMBRIEL" windows --json)
  base_x=$(jq -r '.[0].x + (.[0].w / 3 | floor)' <<< "$windows")
  y=$(jq -r '.[0].y + (.[0].h / 2 | floor)' <<< "$windows")
  x=$((base_x + offset))
  local dir=4
  ((offset >= 0)) && dir=-4
  pointer move "$x" "$y" mod logo press "$BTN_RIGHT" \
    move $((x + dir)) "$y" move $((x + 2 * dir)) "$y" move $((x + 3 * dir)) "$y" \
    release "$BTN_RIGHT" mod none
  "$UMBRIEL" settle
}

failed=0

# Inside the outer third, 4px from the boundary: the press resolves the left
# edge and the drag toward the centre must resize with it.
before=$(window_box)
drag_across_boundary -4
after=$(window_box)
if [[ $after == "$before" ]]; then
  echo "outer-third press dragged across the boundary did not resize: $after"
  failed=1
fi

# Inside the centre third, 4px from the boundary: the press resolves no edge,
# so crossing outward must not start a resize.
before=$(window_box)
drag_across_boundary 4
after=$(window_box)
if [[ $after != "$before" ]]; then
  echo "centre-third press dragged across the boundary resized: $before to $after"
  failed=1
fi

if ((failed != 0)); then
  exit 1
fi

echo "a deferred Mod+right drag resizes with its press-time edge"
