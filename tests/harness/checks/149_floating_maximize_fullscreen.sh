#!/usr/bin/env bash
# harness: outputs=1
# Maximizing a fullscreen float drops fullscreen first, and the restore box it
# captures is the size and position the window is heading back to, not the
# fullscreen box it is leaving. Getting that wrong strands the float at output
# size with no way back. The default 1280x720 output keeps the expected pixels
# exact; a post-boot mode reload races the first client commit's usable-area
# snapshot, so no custom mode here.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/float-maximize-fullscreen.log"

# Animations off, as elsewhere: an animated move settles on the target pixel
# only to within rounding, so asserting an exact origin while one runs is
# flaky. The stranded size this covers does not need one.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[[window_rule]]
match.title = "^float-fs-maximize$"
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

box_of() {
  "$UMBRIEL" windows --json \
    | jq -r '.[] | select(.title == "float-fs-maximize") | "\(.w)x\(.h)+\(.x)+\(.y)"'
}

# All four fields at once, and only once the box has stopped moving: the
# animated move must finish before the next toggle, or the following restore
# box is captured mid-flight and the check measures the race, not the fix.
assert_box() {
  local expected="$1x$2+$3+$4" actual= previous=
  for _ in $(seq 80); do
    actual=$(box_of)
    if [[ $actual == "$expected" && $actual == "$previous" ]]; then
      return 0
    fi
    previous=$actual
    sleep 0.1
  done
  echo "expected box $expected, got $actual"
  return 1
}

"$CLIENT" float-fs-maximize > "$CLIENT_LOG" 2>&1 &

wait_for_field float-fs-maximize floating true
assert_box 640 360 320 180

"$UMBRIEL" msg "window-focus-warp:$(field_of float-fs-maximize id)" > /dev/null

"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
assert_box 1280 720 0 0

# Maximizing out of fullscreen fills the usable area, which looks identical to
# fullscreen. The transition that matters is the next one.
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
assert_box 1280 720 0 0

# Unmaximizing has to land on the pre-fullscreen box. A restore box captured
# from the leaving geometry keeps 1280x720 here, and the window can never get
# back to its own size.
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
assert_box 640 360 320 180

# Still a working toggle afterwards, rather than a one-way trip.
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
assert_box 1280 720 0 0
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
assert_box 640 360 320 180

echo "maximizing a fullscreen float restores the box it had before fullscreen"
