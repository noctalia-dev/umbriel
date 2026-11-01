#!/usr/bin/env bash
# Rendered title bar cursors stay unchanged on press and within slop, then switch only for an actual drag.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
title_bar_config
sed -i '/^\[appearance\]$/a drag_opacity = 1.0' "$UMBRIEL_CONFIG"
cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[input.focus]
follows_mouse = false
[[window_rule]]
match.title = "^drag-cursor-.*-client$"
title_bar = "off"
CONFIG
"$UMBRIEL" msg config-reload >/dev/null

# Crop inside the solid title bar (or the solid client buffer for a client-side move).
# This compares rendered cursors without depending on a particular installed cursor theme.
cursor_hash() {
  local image=$UMBRIEL_RUNTIME_DIR/$1.png
  grim -c "$image"
  magick "$image" -crop "48x24+$((pointer_x - 12))+$((pointer_y - 12))" +repage rgba:- | sha256sum | cut -d' ' -f1
}

for layout in scrolling stacking; do
  "$UMBRIEL" msg workspace-set-layout "$layout" >/dev/null
  for decoration in server client; do
    title=drag-cursor-$layout-$decoration
    if [[ $decoration == server ]]; then
      env XDG_DECORATION=server "$UMBRIEL_FRACTIONAL_CLIENT" "$title" 480 300 \
        >"$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
    else
      "$UMBRIEL_SEAT_LOG_CLIENT" "$title" move-on-press >"$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
    fi
    client=$!
    title_bar_wait_field "$title" title_bar "$([[ $decoration == server ]] && echo true || echo false)"
    "$UMBRIEL" settle >/dev/null
    window=$(title_bar_window "$title")
    origin_x=$(jq -r '.x' <<<"$window")
    origin_y=$(jq -r '.y' <<<"$window")
    pointer_x=$((origin_x + 360))
    pointer_y=$((origin_y + 40))
    if [[ $decoration == server ]]; then
      pointer_y=$((origin_y - 14))
    fi
    pointer_hold 1280 720 move "$pointer_x" "$pointer_y" -- \
      press 272 mark pressed hold \
      move "$((pointer_x + 5))" "$pointer_y" mark slop hold \
      move "$((pointer_x + 25))" "$((pointer_y + 20))" mark dragged hold release 272
    normal=$(cursor_hash "$title-hover")
    pointer_step pressed
    if [[ $decoration == client ]]; then
      await_lines "$UMBRIEL_RUNTIME_DIR/$title.log" move-requested 1 0.025
    fi
    pressed=$(cursor_hash "$title-pressed")
    [[ $pressed == "$normal" ]] || {
      echo "$title: a stationary press changed the cursor"
      exit 1
    }
    pointer_step slop
    pointer_x=$((pointer_x + 5))
    slop=$(cursor_hash "$title-slop")
    [[ $slop == "$normal" ]] || {
      echo "$title: motion below the drag threshold changed the cursor"
      exit 1
    }
    pointer_step dragged
    pointer_x=$((pointer_x + 20))
    pointer_y=$((pointer_y + 20))
    dragged=$(cursor_hash "$title-dragged")
    [[ $dragged != "$normal" ]] || {
      echo "$title: crossing the drag threshold did not change the cursor"
      exit 1
    }
    pointer_release
    "$UMBRIEL" settle >/dev/null
    window=$(title_bar_window "$title")
    pointer_x=$(jq -r '.x + 360' <<<"$window")
    pointer_y=$(jq -r '.y + 40' <<<"$window")
    if [[ $decoration == server ]]; then
      pointer_y=$(jq -r '.y - 14' <<<"$window")
    fi
    "$UMBRIEL_POINTER_CLIENT" 1280 720 move "$pointer_x" "$pointer_y"
    restored=$(cursor_hash "$title-restored")
    [[ $restored == "$normal" ]] || {
      echo "$title: releasing the drag did not restore the cursor"
      exit 1
    }
    kill "$client"
    wait "$client" 2>/dev/null || true
    title_bar_wait_absent "$title"
  done
done
echo "server and client title bars keep the cursor through press and slop, change it for dragging, and restore on release"
