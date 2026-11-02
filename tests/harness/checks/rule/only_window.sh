#!/usr/bin/env bash
# `match.is_alone` counts the tiled windows of a workspace, so a float beside a
# window leaves it alone, which is what fullscreen, maximize, and extents want.
# `match.is_only_window` asks the whole set instead, a float as much as a tile,
# which is what the settings updated while a window is open want. The tiled
# window here carries one rule of each: it must lose its opacity beside a float
# and beside a tile alike, while its alone width survives the float and not the
# tile.
set -euo pipefail

readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/rule-only-window.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[layout.scrolling]
default_extent_fraction = 0.5

[colors]
backdrop = "#00FF00FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[[window_rule]]
match.app_id = "^only-a$"
match.is_alone = true
default_scrolling_extent = 0.75

[[window_rule]]
match.app_id = "^only-a$"
match.is_only_window = true
opacity = 0.5

[[window_rule]]
match.app_id = "^only-b$"
default_floating = true
default_floating_size_px = { width = 200, height = 160 }
default_position = { x = 1050, y = 560, anchor = "top_left" }

[[window_rule]]
match.app_id = "^only-b$"
match.is_only_window = true
opacity = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

client_pid=

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg a "$1" --arg f "$2" '.[] | select(.app_id == $a) | .[$f]'
}

wait_mapped() {
  local app=$1
  for _ in $(seq 80); do
    [[ -n $(field_of "$app" w) ]] && return 0
    sleep 0.1
  done
  echo "window '$app' never mapped"
  exit 1
}

wait_gone() {
  local app=$1
  for _ in $(seq 80); do
    [[ -z $(field_of "$app" w) ]] && return 0
    sleep 0.1
  done
  echo "window '$app' never closed"
  exit 1
}

# Settles first, so the width is the one the client is holding rather than one a
# transition is about to replace.
width_of() {
  "$UMBRIEL" settle
  field_of only-a w
}

# Opaque black client content over the solid green backdrop encodes green 0; at
# rule opacity 0.5 the backdrop shows through and lifts it to about 128. The
# sample sits a quarter across and down the tile's box, clear of the float in
# the bottom right corner.
sample_green() {
  local x y w h
  read -r x y w h <<< "$(
    "$UMBRIEL" windows --json | jq -r '.[] | select(.app_id == "only-a") | "\(.x) \(.y) \(.w) \(.h)"'
  )"
  grim "$SCREENSHOT"
  "$UMBRIEL_PIXEL_PROBE" "$SCREENSHOT" mean "20x20+$((x + w / 4 - 10))+$((y + h / 4 - 10))" | awk '{print $2}'
}

# Failures go to stderr: the caller captures stdout to report the samples it saw.
expect_opacity() {
  local label=$1 want=$2 green=
  "$UMBRIEL" settle
  green=$(sample_green)
  if [[ $want == applied ]] && ((green < 96 || green > 160)); then
    echo "$label: the is_only_window rule did not apply (green=$green)" >&2
    exit 1
  fi
  if [[ $want == absent ]] && ((green > 32)); then
    echo "$label: the is_only_window rule stayed applied (green=$green)" >&2
    exit 1
  fi
  echo "$label green=$green"
}

start_client() {
  foot --config=/dev/null --app-id="$1" --override=colors-dark.background=000000 sh -c 'sleep 120' \
    > /dev/null 2>&1 &
  client_pid=$!
  wait_mapped "$1"
}

start_client only-a
alone_width=$(width_of)
solo=$(expect_opacity "alone" applied)

start_client only-b
beside_float=$(expect_opacity "beside a float" absent)
float_width=$(width_of)
if ((float_width != alone_width)); then
  echo "a float changed the alone width: alone=$alone_width beside=$float_width"
  exit 1
fi

kill "$client_pid"
wait_gone only-b
restored=$(expect_opacity "alone again" applied)
restored_width=$(width_of)
if ((restored_width != alone_width)); then
  echo "the alone width did not come back: restored=$restored_width alone=$alone_width"
  exit 1
fi

# A second tile is company for both questions, in opposite directions: the
# is_only_window rule gives up, and so does the alone extent.
start_client only-c
beside_tile=$(expect_opacity "beside a tile" absent)
tile_width=$(width_of)
if ((tile_width >= alone_width)); then
  echo "the alone width survived a second tile: alone=$alone_width shared=$tile_width"
  exit 1
fi

echo "is_only_window follows the workspace while is_alone keeps its own answer: $solo, $beside_float, $restored, $beside_tile; alone width $alone_width -> $float_width -> $restored_width -> $tile_width"
