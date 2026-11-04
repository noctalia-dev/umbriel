#!/usr/bin/env bash
# harness: outputs=1
# Windows that skip the render_scale supersample: fullscreen windows and game or
# video content are asked to render at the logical scale, so a fullscreen buffer
# is the output's physical mode and never the render-scale size. Leaving
# fullscreen supersamples again, and a `supersample` window rule overrides both
# directions.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly PLAIN_LOG="$UMBRIEL_RUNTIME_DIR/render-scale-plain.log"
readonly GAME_LOG="$UMBRIEL_RUNTIME_DIR/render-scale-game.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.title = "^rs-"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 40, y = 40, anchor = "top_left" }

[output."HEADLESS-1"]
scale = 1.25
render_scale = 2.0
EOF
"$UMBRIEL" msg config-reload > /dev/null

# Waits for one more presentation than `before` and prints the newest one.
next_presentation() {
  local log=$1 before=$2
  await_lines "$log" 'mapped ' $((before + 1)) 0.05 > /dev/null || {
    echo "no new presentation in $log: $(tr '\n' '|' < "$log")" >&2
    return 1
  }
  grep '^mapped ' "$log" | tail -1
}

expect() { # presentation, required fragment, failure message
  if [[ $1 != *"$2"* ]]; then
    echo "$3: $1"
    exit 1
  fi
}

"$CLIENT" rs-plain 400 300 > "$PLAIN_LOG" 2>&1 &
presented=$(next_presentation "$PLAIN_LOG" 0)
echo "plain window: $presented"
expect "$presented" "scale 240/120" "a plain window was not supersampled"

output_size=$("$UMBRIEL" outputs --json | jq -r '.[0].modes[] | select(.current) | "\(.width)x\(.height)"')
before=$(grep -c '^mapped ' "$PLAIN_LOG")
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
presented=$(next_presentation "$PLAIN_LOG" "$before")
echo "fullscreen on a ${output_size} output: $presented"
expect "$presented" "scale 150/120" "a fullscreen window was asked to render at render_scale"
expect "$presented" "buffer $output_size" "a fullscreen buffer is not the output's physical mode"

before=$(grep -c '^mapped ' "$PLAIN_LOG")
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
presented=$(next_presentation "$PLAIN_LOG" "$before")
echo "after fullscreen: $presented"
expect "$presented" "scale 240/120" "leaving fullscreen did not supersample again"

CONTENT_TYPE=game "$CLIENT" rs-game 400 300 > "$GAME_LOG" 2>&1 &
presented=$(next_presentation "$GAME_LOG" 0)
echo "game window: $presented"
expect "$presented" "scale 150/120" "a game window was supersampled"

# Rules decide outright: the game opts in, the plain window opts out.
before_game=$(grep -c '^mapped ' "$GAME_LOG")
before_plain=$(grep -c '^mapped ' "$PLAIN_LOG")
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.title = "^rs-game$"
supersample = true

[[window_rule]]
match.title = "^rs-plain$"
supersample = false
EOF
"$UMBRIEL" msg config-reload > /dev/null
presented=$(next_presentation "$GAME_LOG" "$before_game")
echo "game window with supersample = true: $presented"
expect "$presented" "scale 240/120" "supersample = true did not override the game content type"
presented=$(next_presentation "$PLAIN_LOG" "$before_plain")
echo "plain window with supersample = false: $presented"
expect "$presented" "scale 150/120" "supersample = false did not keep the window at the logical scale"

echo "fullscreen and game windows skip the supersample, rules override both ways"
