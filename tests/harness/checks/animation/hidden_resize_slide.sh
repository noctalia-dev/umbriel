#!/usr/bin/env bash
# A tile maximized by is_alone while hidden returns at its new width throughout the workspace slide.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly IMAGE="$UMBRIEL_RUNTIME_DIR/hidden-resize-slide.png"
readonly SURVIVOR_LOG="$UMBRIEL_RUNTIME_DIR/survivor.log"
readonly PEER_LOG="$UMBRIEL_RUNTIME_DIR/peer.log"
msg() { "$UMBRIEL" msg "$@" > /dev/null; }

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = true
[animation.workspaces]
enabled = true
style = "slide"
duration_ms = 1000
curve = "linear"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.title_bar]
mode = "off"
[appearance.shadow]
enabled = false
[colors]
backdrop = "#000000FF"
[layout]
mode = "scrolling"
gap = 8
[layout.scrolling]
default_extent_fraction = 0.5
[output.HEADLESS-1]
workspaces = 2
workspace_axis = "vertical"
[[window_rule]]
match.title = "^hidden-survivor$"
match.is_alone = true
default_maximize = true
CONFIG
msg config-reload

red_box() {
  grim -o HEADLESS-1 "$IMAGE"
  "$UMBRIEL_PIXEL_PROBE" "$IMAGE" bbox 'r > 0.9 && g < 0.1 && b < 0.1'
}

FILL_COLOR=0xFFFF0000 RESIZE_FILL_COLOR=0xFFFF0000 \
  "$UMBRIEL_UNMAP_CLIENT" hidden-survivor 1280 720 > "$SURVIVOR_LOG" 2>&1 &
await_window hidden-survivor
"$UMBRIEL" settle
read -r _ _ alone_width alone_height <<< "$(red_box)"
survivor=$(window_by_title hidden-survivor | jq -r .id)

FILL_COLOR=0xFF0000FF RESIZE_FILL_COLOR=0xFF0000FF \
  "$UMBRIEL_UNMAP_CLIENT" hidden-peer 1280 720 > "$PEER_LOG" 2>&1 &
await_window hidden-peer
msg window-focus "$survivor"
"$UMBRIEL" settle
read -r _ _ shared_width _ <<< "$(red_box)"
(( alone_width > shared_width + 200 )) || {
  echo "the peer did not shrink the survivor: alone=$alone_width shared=$shared_width"; exit 1;
}

# Finish the outgoing slide before closing the peer, so the resize happens on a fully hidden workspace.
msg workspace-switch 2
"$UMBRIEL" settle
msg window-close "$(window_by_title hidden-peer | jq -r .id)"
await_lines "$PEER_LOG" unmapped 1
"$UMBRIEL" settle
hidden_width=$(window_by_title hidden-survivor | jq -r .w)
(( hidden_width == alone_width )) || {
  echo "the hidden survivor did not resize: expected=$alone_width got=$hidden_width"; exit 1;
}

# A vertical slide leaves the whole width observable. Check multiple intermediate frames, including just before
# completion, and require partial height so an accidentally immediate switch cannot pass.
"$UMBRIEL" clock-freeze
msg workspace-switch 1
elapsed=0
for step in 250 250 400; do
  "$UMBRIEL" clock-advance "$step"
  elapsed=$((elapsed + step))
  read -r _ _ width height <<< "$(red_box)"
  (( width == alone_width && height > 0 && height < alone_height )) || {
    echo "slide at ${elapsed}ms: expected width=$alone_width and partial height<$alone_height, got ${width}x${height} (old width=$shared_width)"
    exit 1
  }
done
"$UMBRIEL" clock-advance 100
"$UMBRIEL" settle
read -r _ _ width height <<< "$(red_box)"
(( width == alone_width && height == alone_height )) || {
  echo "the returned survivor did not settle at ${alone_width}x${alone_height}: ${width}x${height}"; exit 1;
}

echo "a hidden is_alone resize is visible throughout the returning workspace slide"
