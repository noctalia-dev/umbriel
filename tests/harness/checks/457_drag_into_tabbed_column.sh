#!/usr/bin/env bash
# Dropping a window on a tabbed column adds it as the last tab. Tabs share one box, so there is no row boundary to
# aim for: the hint covers the column's bar while the drag is held, and the dropped window lands as the shown tab.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly BTN_LEFT=272
readonly SHOT="$UMBRIEL_RUNTIME_DIR/tabbed-drop.png"
readonly HINT='r > 0.8 && g < 0.2 && b < 0.2'

cat >> "$UMBRIEL_CONFIG" << 'EOF'

[colors]
insert_hint = "#FF0000FF"

[appearance]
drag_opacity = 0.0

[layout.scrolling]
default_extent_fraction = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

accepts() {
  if ! out=$("$UMBRIEL" msg "$1" 2>&1); then
    echo "expected '$1' to be accepted, got: $out"
    return 1
  fi
}

spawn_client() {
  FILL_COLOR=$2 RESIZE_FILL_COLOR=$2 "$UMBRIEL_UNMAP_CLIENT" "$1" 400 300 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 50); do
    [[ $("$UMBRIEL" windows --json | jq --arg t "$1" '[.[] | select(.title == $t)] | length') == 1 ]] && return 0
    sleep 0.1
  done
  echo "expected $1 to map"
  return 1
}

spawn_client drop-a 0xFF00FF00
spawn_client drop-b 0xFF0000FF
accepts window-consume-left
accepts column-toggle-tabbed
spawn_client drop-c 0xFFFFFFFF
"$UMBRIEL" settle

windows=$("$UMBRIEL" windows --json)
read -r cx cy < <(jq -r '.[] | select(.title == "drop-c") | "\(.x + .w / 2 | floor) \(.y + .h / 2 | floor)"' <<< "$windows")
read -r ty tw < <(jq -r '.[] | select(.title == "drop-a") | "\(.y) \(.w)"' <<< "$windows")

# Lifting drop-c leaves the tabbed column alone in the strip, which centres it, so the output's centre is inside it.
"$UMBRIEL" clock-freeze
pointer_hold "$OUTPUT_W" "$OUTPUT_H" move "$cx" "$cy" mod logo press "$BTN_LEFT" move $((OUTPUT_W / 2)) "$cy" \
  -- release "$BTN_LEFT" mod none
"$UMBRIEL" clock-advance 1000 > /dev/null
grim "$SHOT"
# The hint is the bar: a strip one bar high across the column, in the space the column reserves above the tabs' box
# (bar height 24 plus gap 8). Like every drop hint it is drawn on the content edge, inside the borders.
read -r hx hy hw hh <<< "$("$UMBRIEL_PIXEL_PROBE" "$SHOT" bbox "$HINT")"
if ((hy != ty - 24 - 8 || hh != 24 || hw != tw || hx > OUTPUT_W / 2 || hx + hw < OUTPUT_W / 2)); then
  echo "expected the drop hint over the tabbed column's bar, got ${hw}x${hh}+${hx}+${hy}"
  exit 1
fi
pointer_release
"$UMBRIEL" clock-advance 1000 > /dev/null

for _ in $(seq 50); do
  windows=$("$UMBRIEL" windows --json)
  if jq -e '
    . as $w
    | ($w | map(select(.title == "drop-c")) | first) as $c
    | $c.tabbed and ($c.tab_hidden | not)
      and all($w[] | select(.title != "drop-c"); .tabbed and .tab_hidden and .x == $c.x and .y == $c.y and .w == $c.w)
  ' <<< "$windows" > /dev/null; then
    echo "a window dropped on a tabbed column joined it as the shown tab, hinted by its bar"
    exit 0
  fi
  sleep 0.1
done
echo "expected drop-c to join the tabbed column as its shown tab: $windows"
exit 1
