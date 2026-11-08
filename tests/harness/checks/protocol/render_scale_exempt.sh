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
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
# The hint goes out ahead of the fullscreen configure, so the eager test
# client can present once at the old size with the new scale. Wait for the
# fullscreen presentation itself, then assert on it.
await_lines "$PLAIN_LOG" "mapped .*buffer $output_size " 1 0.05 > /dev/null || {
  echo "no fullscreen presentation in $PLAIN_LOG: $(tr '\n' '|' < "$PLAIN_LOG")" >&2
  exit 1
}
presented=$(grep '^mapped ' "$PLAIN_LOG" | grep "buffer $output_size " | tail -1)
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

# A content type can change after mapping: a browser or media player becomes
# `video` when playback starts in an existing window. That path never sees a new
# initial commit, so only View::syncContentType can move the hint.
readonly DYNAMIC_LOG="$UMBRIEL_RUNTIME_DIR/render-scale-dynamic.log"
readonly DYNAMIC_FIFO="$UMBRIEL_RUNTIME_DIR/render-scale-dynamic.fifo"
mkfifo "$DYNAMIC_FIFO"
exec {dynamic}<> "$DYNAMIC_FIFO"
CONTENT_TYPE_CONTROL=1 "$CLIENT" rs-dynamic 400 300 <&"$dynamic" > "$DYNAMIC_LOG" 2>&1 &
presented=$(next_presentation "$DYNAMIC_LOG" 0)
echo "dynamic content window: $presented"
expect "$presented" "scale 240/120" "a plain window was not supersampled"

set_content_type() { # type, expected hint, message
  local before
  before=$(grep -c '^mapped ' "$DYNAMIC_LOG")
  printf 'content-type %s\n' "$1" >&"$dynamic"
  local presented
  presented=$(next_presentation "$DYNAMIC_LOG" "$before")
  echo "content-type $1 on a mapped window: $presented"
  expect "$presented" "$2" "$3"
}
# Alternate exempt and normal so every toggle changes the hint; a same-value
# re-notify is deduped below the wire and would assert nothing.
set_content_type game "scale 150/120" "a game content type set after mapping did not exempt the window"
set_content_type none "scale 240/120" "clearing the content type did not supersample again"
set_content_type video "scale 150/120" "a video content type set after mapping did not exempt the window"
set_content_type none "scale 240/120" "clearing the video content type did not supersample again"

echo "fullscreen and game windows skip the supersample, rules override both ways, and a later content type follows"
