#!/usr/bin/env bash
# harness: outputs=2
# Logical placement and layout-float intent survive physical output loss and recovery.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance.title_bar]
mode = "forced"
height = 28
[output.HEADLESS-1]
scale = 1.25
position = [0, 0]
[output.HEADLESS-2]
position = [1280, 0]
[[workspace]]
index = 1
output = "HEADLESS-1"
layout.mode = "stacking"
[[window_rule]]
match.title = "^(policy|explicit)$"
default_output = "HEADLESS-1"
[[window_rule]]
match.title = "^explicit$"
default_floating = true
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
for _ in $(seq 120); do
  width=$("$UMBRIEL" outputs --json | jq -r '.[] | select(.name == "HEADLESS-1") | .usable_area.width')
  [[ $width == 1024 ]] && break
  sleep 0.025
done
[[ $width == 1024 ]]
for title in policy explicit; do
  "$UMBRIEL_FRACTIONAL_CLIENT" "$title" 480 300 >"$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  title_bar_wait_field "$title" floating true
  title_bar_wait_field "$title" title_bar true
done
title_bar_wait_box policy 480x300+272+152
snapshot() {
  "$UMBRIEL" windows --json | jq -cS 'sort_by(.title) | map({title, workspace, x, y, w, h, floating, title_bar})'
}
"$UMBRIEL" settle >/dev/null
before=$(snapshot)
"$UMBRIEL" output-destroy HEADLESS-1 >/dev/null
for title in policy explicit; do
  for _ in $(seq 120); do
    workspace=$(title_bar_window "$title" | jq -r .workspace)
    [[ $workspace == HEADLESS-2:* ]] && break
    sleep 0.025
  done
  [[ $workspace == HEADLESS-2:* ]]
done
[[ $("$UMBRIEL" output-create HEADLESS-1) == HEADLESS-1 ]]
for title in policy explicit; do
  title_bar_wait_field "$title" workspace HEADLESS-1:1
done
"$UMBRIEL" settle >/dev/null
[[ $(snapshot) == "$before" ]] || {
  echo "floating recovery changed geometry: $before -> $(snapshot)"
  exit 1
}
"$UMBRIEL" msg window-focus-warp "$(title_bar_window policy | jq -r .id)" >/dev/null
"$UMBRIEL" msg workspace-set-layout scrolling >/dev/null
title_bar_wait_field policy floating false
title_bar_wait_field explicit floating true
echo "fractionally scaled floating placement and output recovery verified"
