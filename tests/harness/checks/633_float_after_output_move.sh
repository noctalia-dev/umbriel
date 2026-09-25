#!/usr/bin/env bash
# Floating a tile right after it moves to another output places it from its slot on that output, whether or not a
# frame has applied the move yet. Until that frame the tile is still drawn on the output it left, so placing from the
# drawn position would put it at the new output's edge. Both outputs run at 4 Hz, so after settle returns the move and
# the float land before the next frame; nothing may query windows in between, since that would apply the layout itself.
# harness: outputs=2
set -euo pipefail

printf '\n[output."HEADLESS-1"]\nmode = "1280x720@4"\n\n[output."HEADLESS-2"]\nmode = "1280x720@4"\n' \
  >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

# Opens a tile on HEADLESS-2, moves it to a workspace of its own on HEADLESS-1, floats it, and prints where it landed.
float_after_move() {
  local title=$1 workspace=$2 timing=$3 id
  if [[ $("$UMBRIEL" workspaces --json | jq -r '.[] | select(.focused) | .output') != HEADLESS-2 ]]; then
    "$UMBRIEL" msg output-focus-left > /dev/null
  fi
  "$UMBRIEL_UNMAP_CLIENT" "$title" 400 300 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 60); do
    id=$("$UMBRIEL" windows --json | jq -r --arg title "$title" '.[] | select(.title == $title) | .id')
    [[ -n $id ]] && break
    sleep 0.05
  done
  if [[ -z $id ]]; then
    echo "$title never mapped"
    return 1
  fi
  if [[ $("$UMBRIEL" windows --json | jq -r --arg id "$id" '.[] | select(.id == $id) | .workspace') != HEADLESS-2:* ]]; then
    echo "$title did not open on HEADLESS-2: $("$UMBRIEL" windows --json)"
    return 1
  fi
  "$UMBRIEL" msg "window-focus:$id" > /dev/null
  "$UMBRIEL" settle
  "$UMBRIEL" msg "window-move-to-workspace:$workspace/HEADLESS-1" > /dev/null
  [[ $timing == settled ]] && "$UMBRIEL" settle
  "$UMBRIEL" msg window-toggle-floating > /dev/null
  "$UMBRIEL" settle
  "$UMBRIEL" windows --json | jq -r --arg id "$id" '.[] | select(.id == $id) | "\(.x) \(.y) \(.workspace)"'
}

pending=$(float_after_move float-pending 2 pending)
settled=$(float_after_move float-settled 3 settled)
if [[ ${pending% *} != "${settled% *}" || $pending != *HEADLESS-1:* ]]; then
  echo "floating before the move's frame placed $pending, after it settled $settled"
  exit 1
fi

echo "a tile floated right after moving to another output landed where it lands once the move has settled: $pending"
