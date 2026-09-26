#!/usr/bin/env bash
# A pinned/fullscreen round trip restores pinning and the original tiled state.
set -euo pipefail
CLIENT="${UMBRIEL_UNMAP_CLIENT:?}"
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

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
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
"$CLIENT" pinned-fullscreen-restore 640 480 > "$UMBRIEL_RUNTIME_DIR/pinned-restore.log" 2>&1 &
for _ in $(seq 80); do
  id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title=="pinned-fullscreen-restore") | .id')
  [[ -n $id ]] && break
  sleep 0.05
done
[[ -n $id ]]
wait_fullscreen() {
  for _ in $(seq 80); do
    if "$UMBRIEL" tearing --json | jq -e --argjson state "$1" '.surfaces[] | select(.title=="pinned-fullscreen-restore" and .fullscreen==$state)' >/dev/null; then return 0; fi
    sleep 0.05
  done
  echo "Fullscreen transition did not settle: $1"; return 1
}
for extra_toggle in no yes; do
  "$UMBRIEL" msg "window-focus:$id" >/dev/null
  "$UMBRIEL" windows --json | jq -e --arg id "$id" '.[] | select(.id==$id and .floating==false)' >/dev/null
  "$UMBRIEL" msg window-toggle-pinned >/dev/null
  "$UMBRIEL" msg window-toggle-fullscreen >/dev/null
  wait_fullscreen true
  if [[ $extra_toggle == yes ]]; then "$UMBRIEL" msg window-toggle-pinned >/dev/null; fi
  "$UMBRIEL" msg window-toggle-fullscreen >/dev/null
  wait_fullscreen false
  "$UMBRIEL" msg workspace-switch:2 >/dev/null
  sleep 0.15
  grim "$UMBRIEL_RUNTIME_DIR/pinned-restored.png"
  read -r red green blue <<< "$(magick "$UMBRIEL_RUNTIME_DIR/pinned-restored.png" -crop 20x20+390+240 -colorspace RGB -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:)"
  if ((blue < 100 || blue < red + 30)); then echo "Pinned window did not survive fullscreen/workspace switch: $red $green $blue"; exit 1; fi
  "$UMBRIEL" msg "window-focus:$id" >/dev/null
  "$UMBRIEL" msg window-toggle-pinned >/dev/null
  "$UMBRIEL" windows --json | jq -e --arg id "$id" '.[] | select(.id==$id and .floating==false)' >/dev/null
  "$UMBRIEL" msg workspace-switch:1 >/dev/null
done
echo 'Pinned fullscreen restores pin visibility and original tiling, including a pin toggle while fullscreen'
