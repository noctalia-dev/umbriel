#!/usr/bin/env bash
# harness: outputs=2
# Per-output scrolling defaults size new columns on their owning monitor. A
# reload updates the default for future columns without resizing existing ones.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/tests/subsurface-client}"

window_state() {
  local title=$1
  "$UMBRIEL" windows --json \
    | jq -c --arg title "$title" '[.[] | select(.title == $title)]'
}

wait_for_window() {
  local title=$1 expected_width=$2 expected_output=$3 matches=
  for _ in $(seq 80); do
    matches=$(window_state "$title")
    if jq -e \
      --arg output "$expected_output:" \
      --argjson width "$expected_width" \
      'length == 1 and .[0].w == $width and (.[0].workspace | startswith($output))' \
      <<< "$matches" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected '$title' at width $expected_width on $expected_output, got: $matches"
  return 1
}

spawn_client() {
  local title=$1
  "$CLIENT" "$title" 640 480 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[appearance]
border_width = 0
outer_border_width = 0

[layout]
mode = "scrolling"
gap = 0

[layout.scrolling]
default_width_fraction = 0.5
center_underfull_strip = false

[output.HEADLESS-1]
position = [0, 0]
layout.scrolling.default_width_fraction = 0.25

[output.HEADLESS-2]
position = [1280, 0]
layout.scrolling.default_width_fraction = 0.75

[[window_rule]]
match.app_id = "^output-width-left$"
default_output = "HEADLESS-1"

[[window_rule]]
match.app_id = "^output-width-right-"
default_output = "HEADLESS-2"
EOF
"$UMBRIEL" msg config-reload > /dev/null

# Both headless outputs are 1280 logical pixels wide. With zero gap and border,
# their 0.25 and 0.75 defaults produce exact 320 and 960 pixel columns.
spawn_client output-width-left
wait_for_window output-width-left 320 HEADLESS-1
spawn_client output-width-right-old
wait_for_window output-width-right-old 960 HEADLESS-2

# Updating HEADLESS-2 to 0.625 must refresh the existing workspace's resolved
# config. The old column retains its owned width, while the next one starts at
# exactly 800 pixels.
sed -i \
  's/^layout\.scrolling\.default_width_fraction = 0\.75$/layout.scrolling.default_width_fraction = 0.625/' \
  "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

spawn_client output-width-right-new
wait_for_window output-width-right-new 800 HEADLESS-2
wait_for_window output-width-right-old 960 HEADLESS-2
wait_for_window output-width-left 320 HEADLESS-1

echo "per-output scrolling defaults sized new columns at 320, 960, then 800 after reload without resizing existing columns"
