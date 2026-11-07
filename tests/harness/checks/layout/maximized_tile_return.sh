#!/usr/bin/env bash
# A maximized tile sent to the scratchpad or pinned comes back to the layout at its normal width.
set -euo pipefail

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

width() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | .w'
}

act() {
  "$UMBRIEL" msg "$@" > /dev/null
  "$UMBRIEL" settle
}

for detach in scratchpad pin; do
  title="maximized-tile-$detach"
  foot --title="$title" sh -c 'sleep 120' > /dev/null 2>&1 &
  for _ in $(seq 60); do
    [[ -n $(width "$title") ]] && break
    sleep 0.1
  done
  "$UMBRIEL" settle
  normal=$(width "$title")

  act window-toggle-maximize
  if [[ $detach == scratchpad ]]; then
    act window-toggle-scratchpad
    act scratchpad-toggle
    act window-toggle-scratchpad
  else
    act window-toggle-pinned
    act window-toggle-pinned
  fi

  returned=$(width "$title")
  if ((returned != normal)); then
    echo "$detach: tile returned $returned wide, expected its normal width $normal"
    exit 1
  fi
  act window-close
done
