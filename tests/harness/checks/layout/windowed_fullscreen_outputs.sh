#!/usr/bin/env bash
# harness: outputs=2
# Windowed fullscreen follows an output move between a scaled and an unscaled output: the client stays configured
# fullscreen at its logical tile size, while IPC and foreign-toplevel report the window as not fullscreen.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly FOREIGN="${UMBRIEL_FOREIGN_TOPLEVEL_CLIENT:-./build-debug/tests/foreign-toplevel-client}"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[layout]
mode = "scrolling"

[output.HEADLESS-1]
position = [0, 0]

[output.HEADLESS-2]
position = [1280, 0]
scale = 1.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

declare -A control_fd=()

spawn() {
  local title=$1 fifo="$UMBRIEL_RUNTIME_DIR/$1.ctl" fd
  mkfifo "$fifo"
  exec {fd}<>"$fifo"
  control_fd[$title]=$fd
  FULLSCREEN_ON_STDIN=1 LOG_CONFIGURES=1 FOLLOW_CONFIGURES=1 "$CLIENT" "$title" \
    <&"$fd" > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  for _ in $(seq 60); do
    [[ -n $(field "$title" id) ]] && break
    sleep 0.05
  done
  "$UMBRIEL" settle
}

field() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

focus() {
  "$UMBRIEL" msg window-focus-warp "$(field "$1" id)" > /dev/null
}

# The client's latest configure must be fullscreen at exactly the box IPC reports for the window.
expect_windowed_fullscreen() {
  local title=$1 want= configured=
  "$UMBRIEL" settle
  want="$(field "$title" w)x$(field "$title" h) fullscreen"
  for _ in $(seq 60); do
    configured=$(grep '^configured-state=' "$UMBRIEL_RUNTIME_DIR/$title.log" | tail -1 || true)
    [[ $configured == "configured-state=$want" ]] && break
    sleep 0.05
  done
  if [[ $configured != "configured-state=$want" ]]; then
    echo "$title: expected client configure '$want', got '${configured:-none}'"
    exit 1
  fi
  if [[ $(field "$title" windowed_fullscreen) != true || $(field "$title" fullscreen) != false ]]; then
    echo "$title: expected windowed_fullscreen=true fullscreen=false: $("$UMBRIEL" windows --json)"
    exit 1
  fi
  local output reply
  output=$(field "$title" output)
  reply=$("$FOREIGN" "$title" "$output" fullscreen)
  if [[ $reply != "fullscreen=0" ]]; then
    echo "$title: foreign-toplevel reported '$reply', expected fullscreen=0"
    exit 1
  fi
}

expect_width_below() {
  local title=$1 limit=$2 width
  width=$(field "$title" w)
  if (( width >= limit )); then
    echo "$title: expected a tile narrower than $limit, got $width"
    exit 1
  fi
}

# Scrolling, then an output move between the scaled and unscaled outputs: the configure follows each logical tile.
# 1280 physical pixels at scale 1.5 leave 853 logical pixels.
expect_scaled_width() {
  if [[ $(field scroll-video output) == HEADLESS-2 ]]; then
    expect_width_below scroll-video 854
  fi
}
spawn scroll-video
focus scroll-video
"$UMBRIEL" msg window-toggle-windowed-fullscreen > /dev/null
expect_windowed_fullscreen scroll-video
expect_width_below scroll-video 1280
expect_scaled_width
start_output=$(field scroll-video output)
before="$(field scroll-video w)x$(field scroll-video h)"
"$UMBRIEL" msg window-move-to-output-next > /dev/null
for _ in $(seq 60); do
  [[ $(field scroll-video output) != "$start_output" ]] && break
  sleep 0.05
done
if [[ $(field scroll-video output) == "$start_output" ]]; then
  echo "scroll-video did not leave $start_output"
  exit 1
fi
expect_windowed_fullscreen scroll-video
expect_scaled_width
if [[ "$(field scroll-video w)x$(field scroll-video h)" == "$before" ]]; then
  echo "scroll-video kept its $start_output size $before after moving to $(field scroll-video output)"
  exit 1
fi

# Real fullscreen is reported as fullscreen, so the foreign-toplevel check above is not vacuous.
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
"$UMBRIEL" settle
reply=$("$FOREIGN" scroll-video "$(field scroll-video output)" fullscreen)
if [[ $reply != "fullscreen=1" || $(field scroll-video fullscreen) != true ]]; then
  echo "real fullscreen: foreign-toplevel reported '$reply', IPC fullscreen=$(field scroll-video fullscreen)"
  exit 1
fi
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null

echo "windowed fullscreen follows output moves and scaled outputs; only the client sees fullscreen"
