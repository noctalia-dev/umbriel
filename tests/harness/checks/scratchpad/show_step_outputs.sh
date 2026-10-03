#!/usr/bin/env bash
# harness: outputs=2
# A scratchpad showing one window that is called to another output, here by window-focus on a member it was not
# showing, brings that member there visible rather than focused but still faded out.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly FRAME="$UMBRIEL_RUNTIME_DIR/show-step-outputs.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[animation.scratchpad]
dim = 0.0
blur = false
scale = 0

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[output.HEADLESS-1]
mode = "1280x720"
position = [0, 0]

[output.HEADLESS-2]
mode = "1280x720"
position = [1280, 0]

[[window_rule]]
match.title = "^outputs-"
default_output = "HEADLESS-1"
default_floating = true
default_floating_size_px = { width = 300, height = 200 }
EOF
"$UMBRIEL" msg config-reload > /dev/null

windows() { "$UMBRIEL" windows --json; }
window_of() { windows | jq -c --arg title "$1" '.[] | select(.title == $title)'; }

spawn() {
  FILL_COLOR="$2" "$CLIENT" "$1" 300 200 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 80); do
    [[ -n "$(window_of "$1")" ]] && return 0
    sleep 0.05
  done
  echo "window '$1' never appeared: $(windows)"
  return 1
}

# Which of the two colours are on screen across both outputs.
shown() {
  "$UMBRIEL" settle
  grim "$FRAME"
  local visible=() w h
  read -r _ _ w h < <("$UMBRIEL_PIXEL_PROBE" "$FRAME" bbox 'r > 0.9 && g < 0.1 && b < 0.1')
  ((w > 0 && h > 0)) && visible+=(red)
  read -r _ _ w h < <("$UMBRIEL_PIXEL_PROBE" "$FRAME" bbox 'g > 0.9 && r < 0.1 && b < 0.1')
  ((w > 0 && h > 0)) && visible+=(green)
  echo "${visible[*]}"
}

"$POINTER" 2560 720 move 640 360
spawn outputs-red 0xFFFF0000
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null
spawn outputs-green 0xFF00FF00
"$UMBRIEL" msg window-move-to-scratchpad > /dev/null

"$UMBRIEL" msg scratchpad-show-next > /dev/null
first=$(shown)
if [[ $first != red && $first != green ]]; then
  echo "the scratchpad did not show exactly one window: [$first]"
  exit 1
fi
other=red
[[ $first == red ]] && other=green

# With the pointer on the other output, focus the member the scratchpad was not showing.
"$POINTER" 2560 720 move 1920 360
"$UMBRIEL" msg "window-focus:$(window_of "outputs-$other" | jq -r .id)" > /dev/null
actual=$(shown)
if [[ $actual != "$other" ]]; then
  echo "window-focus on the other output showed [$actual] instead of [$other]: $(windows)"
  exit 1
fi
if [[ $(window_of "outputs-$other" | jq -r .active) != true ]]; then
  echo "the window shown on the other output did not take focus: $(windows)"
  exit 1
fi

echo "a single-window scratchpad called to another output showed the window it was asked for"
