#!/usr/bin/env bash
# Floating windows above each tiling layout reject region actions and drag freely without a drop-zone preview.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
[appearance.title_bar]
mode = "off"
[colors]
insert_hint = "#00FFFFFF"
[[window_rule]]
match.title = "^region-ineligible$"
default_floating = true
default_position = { x = 173, y = 109, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL_FRACTIONAL_CLIENT" region-ineligible 480 300 >"$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
title_bar_wait_box region-ineligible 480x300+173+109
for mode in scrolling dwindle master; do
  "$UMBRIEL" msg workspace-set-layout "$mode" >/dev/null
  before=$(title_bar_window region-ineligible)
  if "$UMBRIEL" msg window-set-region left >"$UMBRIEL_RUNTIME_DIR/rejected.log" 2>&1; then
    echo "region placement accepted a floating window above $mode"
    exit 1
  fi
  after=$(title_bar_window region-ineligible)
  jq -en --argjson before "$before" --argjson after "$after" '
    ($before | [.id, .x, .y, .w, .h, .region]) == ($after | [.id, .x, .y, .w, .h, .region])
  ' >/dev/null
  read -r x y < <(jq -r '"\(.x + .w / 2 | floor) \(.y + .h / 2 | floor)"' <<<"$after")
  pointer_hold 1280 720 move "$x" "$y" mod logo press 272 move 0 360 -- release 272 mod none
  "$UMBRIEL" settle >/dev/null
  grim "$UMBRIEL_RUNTIME_DIR/held-$mode.png"
  hint=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/held-$mode.png" bbox 'r < 0.1 && g > 0.9 && b > 0.9')
  [[ $hint == '0 0 0 0' ]] || {
    echo "floating window above $mode received a drop-zone preview: $hint"
    exit 1
  }
  pointer_release
  title_bar_wait_field region-ineligible region null
  title_bar_wait_field region-ineligible floating true
  title_bar_wait_field region-ineligible w 480
  title_bar_wait_field region-ineligible h 300
done
echo "floating windows above scrolling, dwindle, and master reject region placement and retain free-drag sizing"
