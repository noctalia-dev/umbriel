#!/usr/bin/env bash
# Detaching a placed or maximized window keeps chrome and content at the restored size, even before a client redraw.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[layout]
mode = "stacking"
[layout.stacking]
drop_zones = true
[animation]
duration_ms = 2000
curve = "linear"
[animation.windows_in]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
drag_opacity = 1.0
[appearance.shadow]
enabled = false
[appearance.title_bar]
mode = "forced"
[colors.title_bar]
background = "#FF00FFFF"
background_unfocused = "#FF00FFFF"
text = "#FF00FFFF"
text_unfocused = "#FF00FFFF"
close = "#FF00FFFF"
maximize = "#FF00FFFF"
[[window_rule]]
match.title = "^region-frame$"
default_floating = true
default_position = { x = 173, y = 109, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL_FRACTIONAL_CLIENT" region-frame 480 300 >"$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
client=$!
trap 'kill -CONT "$client" 2>/dev/null || true' EXIT
title_bar_wait_box region-frame 480x300+173+109
"$UMBRIEL" settle >/dev/null
check_frame() {
  local image="$UMBRIEL_RUNTIME_DIR/$1.png" bar content bx by bw bh cx cy cw ch
  grim "$image"
  bar=$("$UMBRIEL_PIXEL_PROBE" "$image" bbox 'r > 0.9 && g < 0.1 && b > 0.9')
  content=$("$UMBRIEL_PIXEL_PROBE" "$image" bbox 'r < 0.1 && (g > 0.4 || b > 0.4)')
  read -r bx by bw bh <<<"$bar"
  read -r cx cy cw ch <<<"$content"
  [[ $bw == 480 && $bh == 28 && $cw == 480 && $ch == 300 && $bx == "$cx" && $((by + bh)) == "$cy" ]] || {
    echo "$1: restored frame and content disagree: bar=$bar content=$content"
    return 1
  }
}
"$UMBRIEL" clock-freeze >/dev/null
for state in placed maximized; do
  for phase in held released; do
    if [[ $state == placed ]]; then
      "$UMBRIEL" msg window-set-region left >/dev/null
      title_bar_wait_box region-frame 628x676+8+36
    else
      "$UMBRIEL" msg window-toggle-maximize >/dev/null
      title_bar_wait_box region-frame 1264x676+8+36
    fi
    "$UMBRIEL" clock-advance 3000 >/dev/null
    "$UMBRIEL" settle >/dev/null
    # Pause only this isolated client. The first screenshot observes presentation before the restore configure is acked.
    kill -STOP "$client"
    pointer_hold 1280 720 move 200 22 press 272 move 600 240 -- move 620 250 mark committed hold release 272
    check_frame "$state-$phase-pending"
    if [[ $phase == released ]]; then
      pointer_step committed
      pointer_release
      check_frame "$state-released-before-commit"
    fi
    kill -CONT "$client"
    title_bar_wait_field region-frame w 480
    title_bar_wait_field region-frame h 300
    check_frame "$state-$phase-committed"
    if [[ $phase == held ]]; then
      pointer_step committed
      check_frame "$state-motion"
      pointer_release
    fi
    "$UMBRIEL" clock-advance 3000 >/dev/null
    "$UMBRIEL" settle >/dev/null
  done
done
"$UMBRIEL" clock-resume >/dev/null
echo "detaching keeps title bar and content aligned before and after the client's restore commit"
