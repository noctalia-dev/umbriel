#!/usr/bin/env bash
# Clear-colored title bars must be redrawn above blur after overview and focus changes.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

cat >>"$UMBRIEL_CONFIG" <<'EOF'
[animation]
enabled = true
[appearance]
border_width = 2
corner_radius = 0
[appearance.title_bar]
mode = "forced"
[appearance.blur]
enabled = true
optimized = true
[colors]
backdrop = "#000000FF"
[colors.title_bar]
background = "#000000FF"
background_unfocused = "#FF00FF80"
[[window_rule]]
match.title = "^blur-refocus-"
blur = true
EOF
"$UMBRIEL" config validate -c "$UMBRIEL_CONFIG" >/dev/null
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 >"$UMBRIEL_RUNTIME_DIR/wallpaper.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/wallpaper.log" ready 1
for name in one two; do
  "$UMBRIEL_UNMAP_CLIENT" "blur-refocus-$name" 480 300 >"$UMBRIEL_RUNTIME_DIR/$name.log" 2>&1 &
  await_lines "$UMBRIEL_RUNTIME_DIR/$name.log" mapped 1
done

expect_background() {
  local phase=$1 window x y color
  "$UMBRIEL" settle >/dev/null
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.active)')
  [[ $(jq -r '.title_bar' <<<"$window") == true ]]
  x=$(jq -r '.x + 180' <<<"$window")
  y=$(jq -r '.y - 14' <<<"$window")
  grim -o HEADLESS-1 "$UMBRIEL_RUNTIME_DIR/title-bar.png"
  color=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/title-bar.png" pixel "$x" "$y")
  [[ $color == "0 0 0" ]] || {
    echo "$phase: focused title bar lost its opaque background: $color"
    exit 1
  }
}

ids=$("$UMBRIEL" windows --json | jq -r '.[].id')
for id in $ids; do
  "$UMBRIEL" msg window-focus "$id" >/dev/null
  expect_background "before overview"
  "$UMBRIEL" msg overview-open >/dev/null
  "$UMBRIEL" settle >/dev/null
  "$UMBRIEL" msg overview-close >/dev/null
  expect_background "after overview"
done
for id in $ids; do
  "$UMBRIEL" msg window-focus "$id" >/dev/null
  expect_background "after IPC refocus"
done
echo "opaque title bars retain their background across overview and IPC refocus with blur"
