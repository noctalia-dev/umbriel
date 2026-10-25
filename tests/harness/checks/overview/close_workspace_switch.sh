#!/usr/bin/env bash
# A workspace switch during the closing zoom runs as the filmstrip row scroll, since the real trees stay hidden until
# the overview tears down. The close still ends with the zoom (keybind_actions holds binds to that timing); a row
# scroll still travelling when the zoom lands carries on as the real workspace slide instead of stopping there.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/close-workspace-switch.png"

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.05
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

active_workspace() {
  "$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name'
}

# Mean red and blue of the whole frame, in percent.
red_blue() {
  magick "$1" -format '%[fx:round(100*mean.r)] %[fx:round(100*mean.b)]' info:
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation.overview]
duration_ms = 1000
curve = "linear"
workspace_curve = "linear"

[animation.workspaces]
duration_ms = 1000
curve = "linear"

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[overview]
shortcuts = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

FILL_COLOR=0xFFCC0000 "$CLIENT" close-switch-red 1200 700 > /dev/null 2>&1 &
wait_for_count 1
"$UMBRIEL" msg workspace-switch:2 > /dev/null
FILL_COLOR=0xFF0000CC "$CLIENT" close-switch-blue 1200 700 > /dev/null 2>&1 &
wait_for_count 2
"$UMBRIEL" msg workspace-switch:1 > /dev/null
"$UMBRIEL" settle

"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle

# The switch starts 750 ms into the 1000 ms close, so the zoom lands with the 1000 ms row scroll a quarter done and the
# slide has the remaining three quarters to cover.
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" clock-advance 750
"$UMBRIEL" msg workspace-next > /dev/null
"$UMBRIEL" clock-advance 300
grim "$SHOT"
read -r red blue <<< "$(red_blue "$SHOT")"
if ((red < 3 || blue < 3)); then
  echo "the switch ended with the closing zoom instead of sliding on: red=$red% blue=$blue% 50 ms after it landed"
  "$UMBRIEL" clock-resume
  exit 1
fi

"$UMBRIEL" clock-advance 1000
"$UMBRIEL" clock-resume
"$UMBRIEL" settle
if [[ $(active_workspace) != 2 ]]; then
  echo "expected workspace 2 after the switch, got: $("$UMBRIEL" workspaces --json)"
  exit 1
fi
grim "$SHOT"
read -r red blue <<< "$(red_blue "$SHOT")"
if ((red > 0 || blue < 3)); then
  echo "the overview did not land on workspace 2 once the switch settled: red=$red% blue=$blue%"
  exit 1
fi

echo "a workspace switch during the overview close slides to its end instead of stopping with the zoom"
