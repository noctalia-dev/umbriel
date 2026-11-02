#!/usr/bin/env bash
# Every edge and corner has a matching preview and placement, including on fractional-scale outputs.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[layout]
mode = "stacking"
[layout.stacking]
drop_zones = true
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.title_bar]
mode = "forced"
[appearance.shadow]
enabled = false
[colors]
insert_hint = "#00FFFFFF"
[[window_rule]]
match.title = "^region-edges$"
default_floating = true
default_position = { x = 173, y = 109, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL_FRACTIONAL_CLIENT" region-edges 480 300 >"$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
title_bar_wait_box region-edges 480x300+173+109
for scale in 1 1.25; do
  if [[ $scale == 1.25 ]]; then
    cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[output."HEADLESS-1"]
scale = 1.25
CONFIG
    "$UMBRIEL" msg config-reload >/dev/null
  fi
  "$UMBRIEL" settle >/dev/null
  width=1280
  height=720
  [[ $scale == 1 ]] || {
    width=1024
    height=576
  }
  area_w=$((width - 16))
  area_h=$((height - 16))
  half_w=$(((area_w - 8) / 2))
  half_h=$(((area_h - 8) / 2))
  for region in left right top bottom top_left top_right bottom_left bottom_right; do
    px=$((width / 2))
    py=$((height / 2))
    x=8
    y=8
    w=$area_w
    h=$area_h
    case $region in
      left | top_left | bottom_left)
        px=0
        w=$half_w
        ;;
      right | top_right | bottom_right)
        px=$((width - 1))
        x=$((16 + half_w))
        w=$half_w
        ;;
    esac
    case $region in
      top | top_left | top_right)
        py=0
        h=$half_h
        ;;
      bottom | bottom_left | bottom_right)
        py=$((height - 1))
        y=$((16 + half_h))
        h=$half_h
        ;;
    esac
    pointer_hold "$width" "$height" move 400 240 mod logo press 272 move "$px" "$py" -- release 272 mod none
    expected=$(awk -v x="$x" -v y="$y" -v w="$w" -v h="$h" -v s="$scale" 'BEGIN { printf "%d %d %d %d", x*s, y*s, w*s, h*s }')
    bounds=
    for _ in $(seq 60); do
      grim "$UMBRIEL_RUNTIME_DIR/preview.png"
      bounds=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/preview.png" bbox 'r < 0.1 && g > 0.9 && b > 0.9')
      [[ $bounds == "$expected" ]] && break
      sleep 0.025
    done
    [[ $bounds == "$expected" ]] || {
      echo "$region at scale $scale: preview $bounds, expected $expected"
      exit 1
    }
    pointer_release
    title_bar_wait_field region-edges region "$region"
    title_bar_wait_field region-edges maximized false
    title_bar_wait_box region-edges "${w}x$((h - 28))+${x}+$((y + 28))"
    "$UMBRIEL" msg window-clear-region >/dev/null
    title_bar_wait_field region-edges region null
    title_bar_wait_box region-edges 480x300+173+109
  done
done
echo "all eight region zones preview and place halves and quarters at normal and fractional scale"
