#!/usr/bin/env bash
# harness: outputs=2
# Omitting the scrolling default leaves the first width unconstrained, then
# keeps the logical width chosen by the mapped client. Fractional and pixel
# window rules remain authoritative. Initial sizing follows the output selected
# by rules, and default_size outranks both fractional sources.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/tests/subsurface-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

window_width() {
  local title=$1
  "$UMBRIEL" windows --json | jq -r --arg title "$title" '.[] | select(.title == $title) | .w'
}

window_workspace() {
  local title=$1
  "$UMBRIEL" windows --json | jq -r --arg title "$title" '.[] | select(.title == $title) | .workspace'
}

output_x() {
  "$UMBRIEL" outputs | awk -v name="$1" '$1 == name {found = 1; next} found && /Position:/ {split($2, p, ","); print p[1]; exit}'
}

wait_for_width() {
  local title=$1 expected=$2
  for _ in $(seq 60); do
    [[ $(window_width "$title") == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $title width $expected, got: $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'
[[workspace]]
output = "HEADLESS-1"
index = 1
layout.scrolling.default_width_fraction = 0.5

[[window_rule]]
match.app_id = "^target-client-width$"
default_output = "HEADLESS-2"

[[window_rule]]
match.app_id = "^fixed-width$"
default_output = "HEADLESS-2"
default_width = 0.75

[[window_rule]]
match.app_id = "^pixel-width$"
default_output = "HEADLESS-1"
default_size = [1000, 600]
default_width = 0.25
EOF
"$UMBRIEL" msg config-reload > /dev/null

# Keep the preferred output different from the rule-selected output.
headless_one_x=$(output_x HEADLESS-1)
"$POINTER" 2560 720 move "$((headless_one_x + 10))" 10

"$CLIENT" target-client-width 800 400 > "$UMBRIEL_RUNTIME_DIR/target-client-width.log" 2>&1 &
wait_for_width target-client-width 800
sleep 0.3
if [[ $(window_width target-client-width) != 800 ]]; then
  echo "client-selected width changed after first arrange: $("$UMBRIEL" windows --json)"
  exit 1
fi
if [[ $(window_workspace target-client-width) != HEADLESS-2:* ]]; then
  echo "client did not land on the rule-selected output: $("$UMBRIEL" windows --json)"
  exit 1
fi

"$CLIENT" fixed-width 300 400 > "$UMBRIEL_RUNTIME_DIR/fixed-width.log" 2>&1 &
wait_for_width fixed-width 942

# The pixel rule lands on an output whose workspace default is 0.5, and also
# carries a lower-priority 0.25 rule. Neither fraction may replace 1000 pixels.
"$CLIENT" pixel-width 300 400 > "$UMBRIEL_RUNTIME_DIR/pixel-width.log" 2>&1 &
wait_for_width pixel-width 1000
first_configure_width=$(awk '/^first-configure / { print $2; exit }' "$UMBRIEL_RUNTIME_DIR/pixel-width.log")
if [[ $first_configure_width != 1000 ]]; then
  echo "default_size did not set the first configure width: $(<"$UMBRIEL_RUNTIME_DIR/pixel-width.log")"
  exit 1
fi
sleep 0.3
if [[ $(window_width pixel-width) != 1000 ]]; then
  echo "default_size width changed after first arrange: $($UMBRIEL windows --json)"
  exit 1
fi

if [[ $(window_width target-client-width) != 800 ]]; then
  echo "opening the fixed-width client changed the existing client width: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "client width 800, fractional rule width 942, and pixel rule width 1000 were retained"
