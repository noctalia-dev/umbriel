#!/usr/bin/env bash
# Client moves count motion before the request, and an already crossed threshold promotes without another event.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >>"$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
drag_opacity = 1.0
[appearance.shadow]
enabled = false
[input.focus]
follows_mouse = false
[output.HEADLESS-1]
scale = 1
CONFIG

cursor_hash() {
  local image=$UMBRIEL_RUNTIME_DIR/$1.png
  grim -c "$image"
  magick "$image" -crop "$((48 * scale))x$((32 * scale))+$(((pointer_x - 12) * scale))+$(((pointer_y - 12) * scale))" \
    +repage rgba:- | sha256sum | cut -d' ' -f1
}

content_box() {
  local image=$UMBRIEL_RUNTIME_DIR/$1.png
  grim "$image"
  "$UMBRIEL_PIXEL_PROBE" "$image" bbox 'b > 0.7 && r < 0.3 && g > 0.4'
}

for scale in 1 2; do
  sed -i "s/^scale = .*/scale = $scale/" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload >/dev/null
  output=$("$UMBRIEL" outputs --json | jq -c '.[0].logical_size')
  width=$(jq -r '.width' <<<"$output")
  height=$(jq -r '.height' <<<"$output")
  for layout in scrolling stacking; do
    "$UMBRIEL" msg workspace-set-layout "$layout" >/dev/null
    for timing in partial crossed; do
      title=press-origin-$scale-$layout-$timing
      log=$UMBRIEL_RUNTIME_DIR/$title.log
      "$UMBRIEL_SEAT_LOG_CLIENT" "$title" move-on-motion >"$log" 2>&1 &
      client=$!
      title_bar_wait_field "$title" title_bar false
      "$UMBRIEL" settle >/dev/null
      window=$(title_bar_window "$title")
      pointer_x=$(jq -r '.x + 200' <<<"$window")
      pointer_y=$(jq -r '.y + 60' <<<"$window")
      distance=8
      after=12
      if [[ $timing == crossed ]]; then
        distance=12
        after=13
      fi
      pointer_hold "$width" "$height" move "$pointer_x" "$pointer_y" press 272 -- \
        move "$((pointer_x + distance))" "$pointer_y" mark requested hold \
        move "$((pointer_x + after))" "$pointer_y" mark continued hold release 272
      before_box=$(content_box "$title-before")
      pointer_step requested
      pointer_x=$((pointer_x + distance))
      await_lines "$log" move-requested 1 0.025
      await_lines "$log" pointer-leave 1 0.025
      requested=$(cursor_hash "$title-requested")
      requested_box=$(content_box "$title-requested-box")
      [[ $requested_box == "$before_box" ]] || {
        echo "$title: accepting the move jumped the window: $before_box to $requested_box"
        exit 1
      }
      if [[ $timing == partial ]]; then
        # Accepting the request transfers cursor ownership from the client. Use this pending cursor as the
        # reference for the active transition, rather than comparing against an application that hides its cursor.
        normal=$requested
      else
        [[ $requested != "$normal" ]] || {
          echo "$title: an already crossed threshold waited for another motion event"
          exit 1
        }
      fi
      pointer_step continued
      pointer_x=$((pointer_x + after - distance))
      continued=$(cursor_hash "$title-continued")
      [[ $continued != "$normal" ]] || {
        echo "$title: the move applied a second drag threshold"
        exit 1
      }
      read -r bx by bw bh <<<"$before_box"
      read -r ax ay aw ah < <(content_box "$title-moved")
      expected_x=$((bx + (after - distance) * scale))
      [[ "$ax $ay $aw $ah" == "$expected_x $by $bw $bh" ]] || {
        echo "$title: continued drag lost the pointer grip: $ax,$ay ${aw}x$ah, expected $expected_x,$by ${bw}x$bh"
        exit 1
      }
      pointer_release
      "$UMBRIEL" settle >/dev/null
      kill "$client"
      wait "$client" 2>/dev/null || true
      title_bar_wait_absent "$title"
    done
  done
done
echo "delayed client moves share the original 10 logical pixel threshold without a second wait or a position jump"
