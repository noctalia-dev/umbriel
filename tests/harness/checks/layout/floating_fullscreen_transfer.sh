#!/usr/bin/env bash
# A fullscreen layout float keeps its normal geometry through a tiled workspace and back.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance.title_bar]
mode = "forced"
[output.HEADLESS-1]
workspaces = 5
[[workspace]]
index = 5
layout.mode = "stacking"
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL" msg workspace-switch 5 >/dev/null
"$UMBRIEL_FRACTIONAL_CLIENT" restore-size 480 300 >"$UMBRIEL_RUNTIME_DIR/restore-size.log" 2>&1 &
title_bar_wait_field restore-size floating true
title_bar_wait_box restore-size 480x300+400+224
for mode in scrolling dwindle master; do
  "$UMBRIEL" msg workspace-switch 4 >/dev/null
  "$UMBRIEL" msg workspace-set-layout "$mode" >/dev/null
  "$UMBRIEL" msg workspace-switch 5 >/dev/null
  "$UMBRIEL" msg window-toggle-fullscreen >/dev/null
  title_bar_wait_field restore-size fullscreen true
  title_bar_wait_box restore-size 1280x720+0+0
  "$UMBRIEL" msg window-move-to-workspace 4 >/dev/null
  title_bar_wait_field restore-size floating false
  title_bar_wait_field restore-size fullscreen true
  "$UMBRIEL" msg window-move-to-workspace 5 >/dev/null
  title_bar_wait_field restore-size floating true
  title_bar_wait_field restore-size fullscreen true
  "$UMBRIEL" msg window-toggle-fullscreen >/dev/null
  title_bar_wait_field restore-size fullscreen false
  title_bar_wait_box restore-size 480x300+400+224
done
echo "fullscreen floats retained normal geometry across all tiling layouts"
