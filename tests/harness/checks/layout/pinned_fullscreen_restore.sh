#!/usr/bin/env bash
# A pinned window made fullscreen and back is pinned again, surviving a workspace switch, and unpinning it restores its
# original tiled state. The second round also toggles the pin while fullscreen.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly TITLE=pinned-fullscreen-restore
readonly SHOT="$UMBRIEL_RUNTIME_DIR/pinned-restored.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.blur]
enabled = false

[output.HEADLESS-1]
workspaces = 2
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" "$TITLE" 640 480 > "$UMBRIEL_RUNTIME_DIR/$TITLE.log" 2>&1 &
id=
for _ in $(seq 80); do
  id=$("$UMBRIEL" windows --json | jq -r --arg title "$TITLE" '.[] | select(.title == $title) | .id')
  [[ -n $id ]] && break
  sleep 0.05
done
if [[ -z $id ]]; then
  echo "$TITLE did not map"
  exit 1
fi

expect_floating() {
  if ! "$UMBRIEL" windows --json | jq -e --arg id "$id" --argjson floating "$1" \
    '.[] | select(.id == $id and .floating == $floating)' > /dev/null; then
    echo "$2: expected floating=$1"
    exit 1
  fi
}

expect_fullscreen() {
  if ! "$UMBRIEL" tearing --json | jq -e --arg title "$TITLE" --argjson state "$1" \
    '.surfaces[] | select(.title == $title and .fullscreen == $state)' > /dev/null; then
    echo "expected fullscreen=$1"
    exit 1
  fi
}

for pin_while_fullscreen in no yes; do
  "$UMBRIEL" msg "window-focus:$id" > /dev/null
  "$UMBRIEL" settle
  expect_floating false "round $pin_while_fullscreen start"
  "$UMBRIEL" msg window-toggle-pinned > /dev/null
  "$UMBRIEL" msg window-toggle-fullscreen > /dev/null
  "$UMBRIEL" settle
  expect_fullscreen true
  if [[ $pin_while_fullscreen == yes ]]; then
    "$UMBRIEL" msg window-toggle-pinned > /dev/null
  fi
  "$UMBRIEL" msg window-toggle-fullscreen > /dev/null
  "$UMBRIEL" settle
  expect_fullscreen false
  "$UMBRIEL" msg workspace-switch:2 > /dev/null
  "$UMBRIEL" settle
  grim "$SHOT"
  read -r red _ blue _ <<< "$("$UMBRIEL_PIXEL_PROBE" "$SHOT" mean 20x20+390+240)"
  if ((blue < 100 || blue < red + 30)); then
    echo "round $pin_while_fullscreen: pinned window hidden after the workspace switch: red=$red blue=$blue"
    exit 1
  fi
  "$UMBRIEL" msg "window-focus:$id" > /dev/null
  "$UMBRIEL" msg window-toggle-pinned > /dev/null
  "$UMBRIEL" settle
  expect_floating false "round $pin_while_fullscreen unpin"
  "$UMBRIEL" msg workspace-switch:1 > /dev/null
done
echo "a pinned window regains its pin after fullscreen and unpins back into tiling"
