#!/usr/bin/env bash
# A maximized tile sent to the scratchpad or pinned comes back maximized, and unmaximizing restores its normal width.
# A float resized out of maximize is no longer maximized, so it re-tiles at its normal width.
set -euo pipefail

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

state() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | "\(.w) \(.maximized)"'
}

act() {
  "$UMBRIEL" msg "$@" > /dev/null
  "$UMBRIEL" settle
}

expect() {
  local actual
  actual=$(state "$title")
  if [[ $actual != "$1" ]]; then
    echo "$detach: $2 at '$actual', expected '$1'"
    exit 1
  fi
}

for detach in scratchpad pin resize; do
  title="maximized-tile-$detach"
  foot --title="$title" sh -c 'sleep 120' > /dev/null 2>&1 &
  for _ in $(seq 60); do
    [[ -n $(state "$title") ]] && break
    sleep 0.1
  done
  "$UMBRIEL" settle
  normal=$(state "$title")

  if [[ $detach == resize ]]; then
    act window-toggle-floating
    act window-toggle-maximize
    act window-modify-width-right -0.1
    act window-toggle-floating
    expect "$normal" "re-tiled float"
    act window-close
    continue
  fi

  act window-toggle-maximize
  maximized=$(state "$title")
  if [[ $detach == scratchpad ]]; then
    act window-toggle-scratchpad
    act scratchpad-toggle
    act window-toggle-scratchpad
  else
    act window-toggle-pinned
    act window-toggle-pinned
  fi
  expect "$maximized" "tile returned"

  act window-toggle-maximize
  expect "$normal" "unmaximized tile"
  act window-close
done
