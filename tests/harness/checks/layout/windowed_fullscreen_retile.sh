#!/usr/bin/env bash
# harness: outputs=1
# Windowed fullscreen follows dwindle and master retiles: when neighbours open, the client is reconfigured at its new
# tile size and stays fullscreen from its own point of view.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly FOREIGN="${UMBRIEL_FOREIGN_TOPLEVEL_CLIENT:-./build-debug/tests/foreign-toplevel-client}"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[layout]
mode = "scrolling"
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

echo "windowed fullscreen follows dwindle and master retiles"
