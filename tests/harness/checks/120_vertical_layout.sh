#!/usr/bin/env bash
# Real clients map into a vertical scrolling layout with transposed lane geometry.
set -euo pipefail

readonly EXPECT_W=1260
readonly EXPECT_H=344
readonly EXPECT_CENTER_Y=$(( (720 - EXPECT_H) / 2 ))

spawn_client() {
  foot --title="vertical-harness-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_windows() {
  local want=$1
  for _ in $(seq 40); do
    if [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]]; then
      return 0
    fi
    sleep 0.25
  done
  echo "timed out waiting for $want window(s), saw: $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout.scrolling]
direction = "vertical"
default_width_fraction = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client a
wait_for_windows 1
spawn_client b
wait_for_windows 2

windows=$("$UMBRIEL" windows --json)
if ! jq -e 'all(.[]; .floating == false)' <<< "$windows" > /dev/null; then
  echo "expected both windows tiled: $windows"
  exit 1
fi
if ! jq -e --argjson w "$EXPECT_W" 'all(.[]; .w == $w)' <<< "$windows" > /dev/null; then
  echo "expected both widths == $EXPECT_W, got: $(jq -c '[.[].w]' <<< "$windows")"
  exit 1
fi
if ! jq -e --argjson h "$EXPECT_H" 'all(.[]; .h == $h)' <<< "$windows" > /dev/null; then
  echo "expected both heights == $EXPECT_H, got: $(jq -c '[.[].h]' <<< "$windows")"
  exit 1
fi
if ! jq -e '[.[].x] | unique | length == 1' <<< "$windows" > /dev/null; then
  echo "vertical lanes should share a left edge, got: $(jq -c '[.[].x]' <<< "$windows")"
  exit 1
fi
if ! jq -e '[.[].y] | unique | length == 2' <<< "$windows" > /dev/null; then
  echo "vertical lanes should occupy distinct y positions, got: $(jq -c '[.[].y]' <<< "$windows")"
  exit 1
fi

# Column centering follows the vertical layout's primary axis.
"$UMBRIEL" msg column-center > /dev/null
center_y=0
for _ in $(seq 40); do
  center_y=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "vertical-harness-b") | .y')
  [[ $center_y -eq $EXPECT_CENTER_Y ]] && break
  sleep 0.1
done
if [[ $center_y -ne $EXPECT_CENTER_Y ]]; then
  echo "expected last vertical column centered at y=$EXPECT_CENTER_Y, got y=$center_y"
  exit 1
fi

"$UMBRIEL" msg window-focus-up > /dev/null
"$UMBRIEL" msg column-center > /dev/null
center_y=0
for _ in $(seq 40); do
  center_y=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "vertical-harness-a") | .y')
  [[ $center_y -eq $EXPECT_CENTER_Y ]] && break
  sleep 0.1
done
if [[ $center_y -ne $EXPECT_CENTER_Y ]]; then
  echo "expected first vertical column centered at y=$EXPECT_CENTER_Y, got y=$center_y"
  exit 1
fi

echo "2 clients tiled in vertical lanes at ${EXPECT_W}x${EXPECT_H}, edge columns center"
