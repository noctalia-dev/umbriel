#!/usr/bin/env bash
# harness: outputs=1
# window-toggle-maximize fills a focused float's usable area, at the usable
# origin, and restores the box it had before, instead of no-opping through the
# layout column a float never has. The default 1280x720 output keeps the
# expected pixels exact; a post-boot mode reload races the first client commit's
# usable-area snapshot, so no custom mode here.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/float-maximize.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[[window_rule]]
match.title = "^float-maximize$"
default_floating = true
default_floating_size = { width = 0.5, height = 0.5 }
EOF
"$UMBRIEL" msg config-reload > /dev/null

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

assert_box() {
  local w=$1 h=$2 x=$3 y=$4
  wait_for_field float-maximize w "$w"
  wait_for_field float-maximize h "$h"
  wait_for_field float-maximize x "$x"
  wait_for_field float-maximize y "$y"
}

"$CLIENT" float-maximize > "$CLIENT_LOG" 2>&1 &

wait_for_field float-maximize floating true
assert_box 640 360 320 180

"$UMBRIEL" msg "window-focus-warp:$(field_of float-maximize id)" > /dev/null

# The verb reaches the float instead of bailing on the missing column, and the
# window moves to the usable origin rather than growing in place off the edge.
"$UMBRIEL" msg window-toggle-maximize > /dev/null
assert_box 1280 720 0 0

# Restoring returns the exact pre-maximize box, not merely something smaller.
"$UMBRIEL" msg window-toggle-maximize > /dev/null
assert_box 640 360 320 180

# Resizing a maximized float drops maximization and keeps the new size, so the
# next toggle has to maximize. If the state survived, this would restore 640.
"$UMBRIEL" msg window-toggle-maximize > /dev/null
wait_for_field float-maximize w 1280
"$UMBRIEL" msg window-modify-primary-extent:-0.2 > /dev/null
wait_for_field float-maximize w 1024
"$UMBRIEL" msg window-toggle-maximize > /dev/null
wait_for_field float-maximize w 1280

# That maximize captured the resized box, so restoring lands on 1024, not 640.
"$UMBRIEL" msg window-toggle-maximize > /dev/null
wait_for_field float-maximize w 1024
wait_for_field float-maximize h 720

echo "window-toggle-maximize fills and restores a floating window's usable area"
