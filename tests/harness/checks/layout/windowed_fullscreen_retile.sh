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

# Each layout gets its own workspace, so the first window starts alone and every neighbour forces a retile.
workspace=0
for layout in dwindle master; do
  workspace=$((workspace + 1))
  "$UMBRIEL" msg workspace-switch "$workspace" > /dev/null
  "$UMBRIEL" msg workspace-set-layout "$layout" > /dev/null
  video="video-$layout"
  spawn "$video"
  "$UMBRIEL" msg window-toggle-windowed-fullscreen > /dev/null
  expect_windowed_fullscreen "$video"

  sizes=("$(field "$video" w)x$(field "$video" h)")
  for neighbour in 1 2; do
    spawn "$layout-neighbour-$neighbour"
    focus "$video"
    expect_windowed_fullscreen "$video"
    sizes+=("$(field "$video" w)x$(field "$video" h)")
  done
  # The check means nothing unless a neighbour actually moved the video's tile. A second master neighbour joins the
  # stack and may leave the master tile as it was, so only the first must resize it.
  if [[ ${sizes[0]} == "${sizes[1]}" ]]; then
    echo "$layout: the first neighbour did not resize the video tile: ${sizes[*]}"
    exit 1
  fi
done

echo "windowed fullscreen follows dwindle and master retiles"
