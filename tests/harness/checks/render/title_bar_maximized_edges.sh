#!/usr/bin/env bash
# Maximized-to-edges windows keep a reachable title bar and reserve its height, including overview and rule changes.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
title_bar_config
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[layout]
client_maximize = "edges"
[layout.scrolling]
default_extent_fraction = 0.5
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

image=$UMBRIEL_RUNTIME_DIR/edges-title-bar.png
spawn() {
  local title=$1
  env XDG_DECORATION=server "$UMBRIEL_FRACTIONAL_CLIENT" "$title" 480 300 \
    > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  title_bar_wait_field "$title" title_bar true
  "$UMBRIEL" settle > /dev/null
}
assert_bar() {
  grim -o HEADLESS-1 "$image"
  read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$image" pixel 200 14)
  if ! ((red > 240 && green < 12 && blue > 240)); then
    echo "maximized title bar was not drawn at the output edge: $red $green $blue"
    exit 1
  fi
}
click_maximize() {
  local title=$1 x y
  read -r x y < <(title_bar_window "$title" | jq -r '[.x + .w - 36, .y - 14] | @tsv')
  "$UMBRIEL_POINTER_CLIENT" 1280 720 move "$x" "$y" click 272
  "$UMBRIEL" settle > /dev/null
}

for kind in tile float stacking; do
  if [[ $kind == stacking ]]; then
    cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[[workspace]]
index = 1
layout.mode = "stacking"
CONFIG
    "$UMBRIEL" msg config-reload > /dev/null
  fi
  title=ssd-$kind-edges
  spawn "$title"
  original=$(title_bar_window "$title" | jq -r '"\(.w)x\(.h)+\(.x)+\(.y)"')
  if [[ $kind == float ]]; then
    click_maximize "$title"
    title_bar_wait_field "$title" maximized true
    title_bar_wait_field "$title" maximized_to_edges false
    click_maximize "$title"
    title_bar_wait_box "$title" "$original"
    "$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
    "$UMBRIEL" settle > /dev/null
  else
    click_maximize "$title"
  fi
  title_bar_wait_field "$title" maximized_to_edges true
  title_bar_wait_field "$title" title_bar true
  title_bar_wait_box "$title" 1280x692+0+28
  assert_bar

  # The overview owns its own frame preview, which must retain the title bar too.
  "$UMBRIEL" msg overview-open > /dev/null
  "$UMBRIEL" settle > /dev/null
  grim -o HEADLESS-1 "$image"
  count=$("$UMBRIEL_PIXEL_PROBE" "$image" count 'r > 0.8 && g < 0.2 && b > 0.8')
  ((count > 100))
  "$UMBRIEL" msg overview-close > /dev/null
  "$UMBRIEL" settle > /dev/null
  click_maximize "$title"
  title_bar_wait_field "$title" maximized_to_edges false
  title_bar_wait_box "$title" "$original"

  # A keybind-created edges maximize also keeps mouse-only restore with column client policy.
  sed -i 's/client_maximize = "edges"/client_maximize = "column"/' "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
  "$UMBRIEL" settle > /dev/null
  title_bar_wait_box "$title" 1280x692+0+28
  assert_bar
  click_maximize "$title"
  title_bar_wait_field "$title" maximized_to_edges false
  title_bar_wait_box "$title" "$original"

  "$UMBRIEL" msg window-toggle-fullscreen > /dev/null
  title_bar_wait_field "$title" title_bar false
  "$UMBRIEL" msg window-toggle-fullscreen > /dev/null
  title_bar_wait_field "$title" title_bar true
  "$UMBRIEL" msg window-close > /dev/null
  title_bar_wait_absent "$title"
  "$UMBRIEL" settle > /dev/null
  sed -i 's/client_maximize = "column"/client_maximize = "edges"/' "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
done

# State matching offers an explicit opt-out and reclaims the reserved title-bar space immediately.
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[[window_rule]]
match.title = "^ssd-rule-edges$"
match.is_maximized_to_edges = true
title_bar = "off"
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
spawn ssd-rule-edges
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
"$UMBRIEL" settle > /dev/null
title_bar_wait_field ssd-rule-edges title_bar false
title_bar_wait_box ssd-rule-edges 1280x720+0+0
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
title_bar_wait_field ssd-rule-edges title_bar true

# A false matcher selects the restored state, and live reload reevaluates existing windows.
sed -i 's/is_maximized_to_edges = true/is_maximized_to_edges = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
title_bar_wait_field ssd-rule-edges title_bar false
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
"$UMBRIEL" settle > /dev/null
title_bar_wait_field ssd-rule-edges title_bar true
title_bar_wait_box ssd-rule-edges 1280x692+0+28
assert_bar
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
title_bar_wait_field ssd-rule-edges title_bar false

echo "edges title bars reserve content space, preview in overview, restore by mouse, and follow live state rules"
