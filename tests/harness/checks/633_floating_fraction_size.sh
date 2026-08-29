#!/usr/bin/env bash
# harness: outputs=1
# Floating windows honor default_width/default_height as usable-area fractions,
# and the resize actions resize a focused float. The default 1280x720 output
# keeps the expected pixels exact; a post-boot mode reload races the first
# client commit's usable-area snapshot, so no custom mode here.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/fractional-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/float-fraction.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[[window_rule]]
match.title = "^float-fraction$"
default_floating = true
default_width = 0.5
default_height = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

wait_for_count() {
  local expected=$1 count=
  for _ in $(seq 80); do
    count=$("$UMBRIEL" windows --json | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected windows, got $count"
  return 1
}

field_of() {
  "$UMBRIEL" windows --json \
    | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

wait_for_field() {
  local title=$1 field=$2 expected=$3 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$title" "$field")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' field '$field' to be '$expected', got '$actual'"
  return 1
}

# The client adopts every configured size, so IPC geometry reports what the
# compositor asked for, overriding its own 800x600 preference.
"$CLIENT" float-fraction > "$CLIENT_LOG" 2>&1 &

wait_for_count 1
wait_for_field float-fraction floating true
wait_for_field float-fraction w 640
wait_for_field float-fraction h 360

"$UMBRIEL" msg "window-focus-warp:$(field_of float-fraction id)" > /dev/null

# modify: 0.5 + 0.1 of 1280 -> 768
"$UMBRIEL" msg window-modify-width:0.1 > /dev/null
wait_for_field float-fraction w 768
# cycle: the next preset past 0.6 is 2/3 of 1280 -> 853
"$UMBRIEL" msg window-cycle-width > /dev/null
wait_for_field float-fraction w 853
# cycle back: the previous preset under 2/3 is 0.5 of 1280 -> 640
"$UMBRIEL" msg window-cycle-width-back > /dev/null
wait_for_field float-fraction w 640
# cycle: the next preset past 0.5 is 2/3 of 720 -> 480
"$UMBRIEL" msg window-cycle-height > /dev/null
wait_for_field float-fraction h 480
# cycle back: the previous preset under 2/3 is 0.5 of 720 -> 360
"$UMBRIEL" msg window-cycle-height-back > /dev/null
wait_for_field float-fraction h 360

echo "floating fraction sizing and the keyboard resize actions work on floating windows"
