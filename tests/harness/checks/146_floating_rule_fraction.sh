#!/usr/bin/env bash
# Floating windows open at fractions of the usable area when their rule carries
# default_width/default_height, and pixel default_size beats those fractions.
# A full-extent probe calibrates the usable area instead of hardcoding it.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/subsurface-client}"

geometry() {
  local title=$1
  "$UMBRIEL" windows --json | jq -r --arg title "$title" '.[] | select(.title == $title) | "\(.w)x\(.h)"'
}

poll_geometry() {
  local title=$1 expected=${2:-} previous=""
  for _ in $(seq 60); do
    local current
    current=$(geometry "$title")
    if [[ -n $expected ]]; then
      [[ $current == "$expected" ]] && return 0
    elif [[ -n $current && $current == "$previous" ]]; then
      echo "$current"
      return 0
    fi
    previous=$current
    sleep 0.1
  done
  if [[ -n $expected ]]; then
    echo "expected $title sized $expected, got: $("$UMBRIEL" windows --json)"
  else
    echo "geometry never stabilized for $title" >&2
  fi
  return 1
}

expect_floating() {
  local title=$1 floating
  floating=$("$UMBRIEL" windows --json | jq -r --arg title "$title" '.[] | select(.title == $title) | .floating')
  if [[ $floating != true ]]; then
    echo "expected $title to be floating: $("$UMBRIEL" windows --json)"
    return 1
  fi
}

round_scaled() {
  awk -v extent="$1" -v factor="$2" 'BEGIN { printf "%d", int(factor * extent + 0.5) }'
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.app_id = "^float-full$"
default_floating = true
default_width = 1.0
default_height = 1.0

[[window_rule]]
match.app_id = "^float-fraction$"
default_floating = true
default_width = 0.5
default_height = 0.6

[[window_rule]]
match.app_id = "^float-pixel$"
default_floating = true
default_size = [600, 400]
default_width = 0.9
default_height = 0.9
EOF
"$UMBRIEL" msg config-reload > /dev/null

APP_ID=float-full "$CLIENT" "float-full" > "$UMBRIEL_RUNTIME_DIR/full.log" 2>&1 &
readonly FULL_SIZE=$(poll_geometry "float-full")
full_w=${FULL_SIZE%x*}
full_h=${FULL_SIZE#*x}
if ((full_w <= 0 || full_h <= 0)); then
  echo "calibration probe reported unusable extents: $FULL_SIZE"
  exit 1
fi

expect_floating "float-full"
readonly FRACTION_SIZE="$(round_scaled "$full_w" 0.5)x$(round_scaled "$full_h" 0.6)"

APP_ID=float-fraction "$CLIENT" "float-fraction" > "$UMBRIEL_RUNTIME_DIR/fraction.log" 2>&1 &
poll_geometry "float-fraction" "$FRACTION_SIZE"
expect_floating "float-fraction"

# Pixels win wherever default_size is present, even alongside fractions.
APP_ID=float-pixel "$CLIENT" "float-pixel" > "$UMBRIEL_RUNTIME_DIR/pixel.log" 2>&1 &
poll_geometry "float-pixel" "600x400"
expect_floating "float-pixel"

# Opening later windows must not disturb either rule's outcome.
poll_geometry "float-full" "$FULL_SIZE"
poll_geometry "float-fraction" "$FRACTION_SIZE"

echo "fractions sized from a ${FULL_SIZE} usable area -> $FRACTION_SIZE; pixels kept 600x400"
