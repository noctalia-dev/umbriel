#!/usr/bin/env bash
# Configured directional bindings navigate overview cards and workspace rows through the same action dispatcher used
# outside overview. Ordinary actions keep targeting the selected card without closing the overview.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly BTN_LEFT=272
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly OVERVIEW_EVENTS="$UMBRIEL_RUNTIME_DIR/overview-events.log"

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

chord() {
  pointer mod control tap "$1" mod none
}

window_count() {
  "$UMBRIEL" windows --json | jq 'length'
}

selected_title() {
  "$UMBRIEL" windows --json | jq -r '[.[] | select(.active) | .title] | if length == 1 then .[0] else "none" end'
}

active_workspace() {
  "$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name'
}

overview_closed_count() {
  jq -s '[.[] | select(.event == "overview" and .data.open == false)] | length' "$OVERVIEW_EVENTS"
}

overview_open_count() {
  jq -s '[.[] | select(.event == "overview" and .data.open == true)] | length' "$OVERVIEW_EVENTS"
}

wait_for_count() {
  local expected=$1
  for _ in $(seq 60); do
    [[ $(window_count) -eq $expected ]] && return 0
    sleep 0.05
  done
  echo "expected $expected windows, got: $($UMBRIEL windows --json)"
  return 1
}

wait_for_focus() {
  local expected=$1
  for _ in $(seq 60); do
    [[ $(selected_title) == "$expected" ]] && return 0
    sleep 0.05
  done
  echo "expected focus on '$expected', got: $($UMBRIEL windows --json)"
  return 1
}

wait_for_workspace() {
  local expected=$1
  for _ in $(seq 60); do
    [[ $(active_workspace) == "$expected" ]] && return 0
    sleep 0.05
  done
  echo "expected workspace '$expected', got: $($UMBRIEL workspaces --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation.overview]
duration_ms = 500

[layout.scrolling]
default_extent_fraction = 0.5

[input.cursor]
follows_focus = true

[overview]
shortcuts = false

[keybinds]
"Ctrl+H" = "window-focus-left"
"Ctrl+J" = "window-focus-down"
"Ctrl+K" = "window-focus-up"
"Ctrl+L" = "window-focus-right"
"Ctrl+N" = "window-focus-or-workspace-down"
"Ctrl+P" = "window-focus-or-workspace-up"
"Ctrl+X" = "window-close"
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" subscribe overview > "$OVERVIEW_EVENTS" &
for _ in $(seq 40); do
  [[ -s $OVERVIEW_EVENTS ]] && break
  sleep 0.05
done

"$CLIENT" overview-vim-first 1200 700 > "$UMBRIEL_RUNTIME_DIR/overview-vim-first.log" 2>&1 &
wait_for_count 1
"$CLIENT" overview-vim-second 1200 700 > "$UMBRIEL_RUNTIME_DIR/overview-vim-second.log" 2>&1 &
wait_for_count 2

second_id=$(
  "$UMBRIEL" windows --json | jq -r '.[] | select(.title == "overview-vim-second") | .id'
)
"$UMBRIEL" msg "window-focus:$second_id" > /dev/null
"$UMBRIEL" msg window-consume-left > /dev/null
stacked=false
for _ in $(seq 60); do
  if "$UMBRIEL" windows --json | jq -e \
      '[.[] | select(.title == "overview-vim-first" or .title == "overview-vim-second")]
       | length == 2 and (.[0].x == .[1].x) and ([.[].y] | unique | length == 2)' > /dev/null; then
    stacked=true
    break
  fi
  sleep 0.05
done
if [[ $stacked != true ]]; then
  echo "expected the first two windows to form a vertical stack: $($UMBRIEL windows --json)"
  exit 1
fi

"$CLIENT" overview-vim-right 1200 700 > "$UMBRIEL_RUNTIME_DIR/overview-vim-right.log" 2>&1 &
wait_for_count 3
"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$CLIENT" overview-vim-row 1200 700 > "$UMBRIEL_RUNTIME_DIR/overview-vim-row.log" 2>&1 &
wait_for_count 4
"$UMBRIEL" msg workspace-switch:1 > /dev/null

read -r top_id top_title bottom_id bottom_title <<< "$("$UMBRIEL" windows --json | jq -r \
  '[.[] | select(.title == "overview-vim-first" or .title == "overview-vim-second")] | sort_by(.y)
   | "\(.[0].id) \(.[0].title) \(.[1].id) \(.[1].title)"')"
"$UMBRIEL" msg "window-focus:$top_id" > /dev/null
wait_for_focus "$top_title"
sleep 0.1

read -r top_x top_y bottom_y <<< "$("$UMBRIEL" windows --json | jq -r --arg top "$top_id" --arg bottom "$bottom_id" \
  '(.[] | select(.id == $top)) as $top_view | (.[] | select(.id == $bottom)) as $bottom_view
   | "\($top_view.x) \($top_view.y) \($bottom_view.y)"')"
# The client reports its buffer size, which can exceed the tiled target. Use the two row origins to choose a point
# safely inside the top tile instead of deriving its center from the client buffer.
pointer move "$((top_x + 100))" "$(((top_y + bottom_y) / 2))"

"$UMBRIEL" msg overview-open > /dev/null
sleep 0.55

# Vim-style custom bindings and the built-in arrow fallback share horizontal card navigation.
chord 38 # L
wait_for_focus overview-vim-right
chord 35 # H
wait_for_focus "$top_title"
pointer tap 106 # Right
wait_for_focus overview-vim-right
pointer tap 105 # Left
wait_for_focus "$top_title"

# Composite vertical actions keep their normal local-first semantics instead of becoming aliases for overview rows.
chord 49 # N
wait_for_focus "$bottom_title"
wait_for_workspace 1
chord 25 # P
wait_for_focus "$top_title"
"$UMBRIEL" msg window-focus-or-output-right > /dev/null
wait_for_focus overview-vim-right
if output_error=$("$UMBRIEL" msg window-focus-or-output-right 2>&1); then
  echo "expected window-focus-or-output-right at the card edge to reach its output fallback"
  exit 1
fi
if [[ $output_error != *"no output to the right"* ]]; then
  echo "expected the output fallback error, got: $output_error"
  exit 1
fi

# Overview focus updates its card selection without applying the normal follows_focus cursor warp. Close the overview,
# focus the top window without a warp, then click without moving: the pointer must still be over that top window.
"$UMBRIEL" msg overview-close > /dev/null
sleep 0.6
"$UMBRIEL" msg "window-focus:$top_id" > /dev/null
wait_for_focus "$top_title"
pointer click "$BTN_LEFT"
wait_for_focus "$top_title"

"$UMBRIEL" msg overview-open > /dev/null
sleep 0.55

# A normal action operates on the selected overview card and leaves the overview interactive.
chord 38 # L
wait_for_focus overview-vim-right
chord 45 # X
wait_for_count 3
wait_for_focus "$top_title"

# Direct vertical focus follows stacked cards within the selected workspace. This transition also proves the close
# above did not close overview.
chord 36 # J
wait_for_focus "$bottom_title"
wait_for_workspace 1
chord 37 # K
wait_for_focus "$top_title"
wait_for_workspace 1

# Unbound vertical arrows follow the same local-first rule as the composite actions: a stacked card first, a
# workspace row only once the column has no card in that direction.
pointer tap 108 # Down
wait_for_focus "$bottom_title"
wait_for_workspace 1
pointer tap 108 # Down
wait_for_workspace 2
wait_for_focus overview-vim-row
pointer tap 103 # Up
wait_for_workspace 1
pointer tap 103 # Up
wait_for_focus "$top_title"
wait_for_workspace 1

# Enter closes toward the selected card. Configured binds remain effective
# during that close, and row retargeting must not restart the zoom timeline.
closed_before=$(overview_closed_count)
pointer tap 28 # Enter
sleep 0.15
chord 49 # N, focus the lower card
wait_for_focus "$bottom_title"
sleep 0.15
chord 49 # N, switch to workspace 2
wait_for_workspace 2
sleep 0.3
if (( $(overview_closed_count) <= closed_before )); then
  echo "workspace binds extended the overview closing timeline"
  exit 1
fi
wait_for_focus overview-vim-row
wait_for_workspace 2

# The same rules follow the output's workspace axis. With workspaces arranged along X the roles swap: Left and Right
# run along the workspace axis, and Up and Down traverse the vertically scrolling lanes.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output."HEADLESS-1"]
workspace_axis = "horizontal"
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg workspace-switch:1 > /dev/null
wait_for_workspace 1

# The stacked pair keeps its column, which now lays its members out along X.
transposed=false
for _ in $(seq 60); do
  read -r left_id left_title right_id right_title order <<< "$("$UMBRIEL" windows --json | jq -r \
    '[.[] | select(.title == "overview-vim-first" or .title == "overview-vim-second")] | sort_by(.x)
     | "\(.[0].id) \(.[0].title) \(.[1].id) \(.[1].title) "
       + (if .[0].y == .[1].y and .[0].x < .[1].x then "side-by-side" else "stacked" end)')"
  [[ $order == side-by-side ]] && transposed=true && break
  sleep 0.05
done
if [[ $transposed != true ]]; then
  echo "stacked column did not transpose onto the horizontal axis: $("$UMBRIEL" windows --json)"
  exit 1
fi

# A second lane, so the perpendicular arrows have somewhere to go inside the workspace. A vertically scrolling strip
# puts that new column above or below the stacked pair, never beside it. Column insertion animates the strip, so both
# lanes have to read the same twice before their order decides which arrow points at the new one.
"$CLIENT" overview-vim-lane 1200 700 > "$UMBRIEL_RUNTIME_DIR/overview-vim-lane.log" 2>&1 &
wait_for_count 4
lanes=
previous=
for _ in $(seq 80); do
  lanes=$("$UMBRIEL" windows --json | jq -r \
    '[(.[] | select(.title == "overview-vim-first") | .y), (.[] | select(.title == "overview-vim-lane") | .y)] | "\(.[0]) \(.[1])"')
  read -r stack_y lane_y <<< "$lanes"
  if [[ $lanes == "$previous" ]] && (( lane_y > stack_y + 100 || lane_y + 100 < stack_y )); then
    break
  fi
  previous=$lanes
  lanes=
  sleep 0.05
done
if [[ -z $lanes ]]; then
  echo "the new column did not settle into a lane of its own along Y: $("$UMBRIEL" windows --json)"
  exit 1
fi
if (( lane_y > stack_y )); then
  toward=108 # Down
  back=103   # Up
else
  toward=103 # Up
  back=108   # Down
fi

"$UMBRIEL" msg "window-focus:$left_id" > /dev/null
wait_for_focus "$left_title"
"$UMBRIEL" msg overview-open > /dev/null
sleep 0.55

# Along the workspace axis: the stacked neighbour first, the next workspace only once the lane has no card that way.
pointer tap 106 # Right
wait_for_focus "$right_title"
wait_for_workspace 1
pointer tap 106 # Right
wait_for_workspace 2
wait_for_focus overview-vim-row
pointer tap 105 # Left
wait_for_workspace 1
wait_for_focus "$right_title"
pointer tap 105 # Left
wait_for_focus "$left_title"
wait_for_workspace 1

# Perpendicular to it, the arrows walk the vertically scrolling lanes and never reach a workspace.
pointer tap "$toward"
wait_for_focus overview-vim-lane
wait_for_workspace 1
pointer tap "$toward"
sleep 0.2
wait_for_focus overview-vim-lane
wait_for_workspace 1
pointer tap "$back"
wait_for_focus "$left_title"
wait_for_workspace 1

# A configured directional action keeps its physical meaning: it moves the focus and stops at the lane's edge.
chord 38 # L
wait_for_focus "$right_title"
wait_for_workspace 1
chord 38 # L again, with nothing to the right
sleep 0.2
wait_for_focus "$right_title"
wait_for_workspace 1

# Retargeting the close with a configured bind must land the focus without revealing the overview a second time.
closed_before=$(overview_closed_count)
opened_before=$(overview_open_count)
pointer tap 28 # Enter
sleep 0.15
chord 35 # H, focus the card to the left during the closing zoom
wait_for_focus "$left_title"
sleep 0.6
if (( $(overview_closed_count) <= closed_before )); then
  echo "the closing zoom never finished on horizontally arranged workspaces"
  exit 1
fi
if (( $(overview_open_count) != opened_before )); then
  echo "focusing during the close revealed the overview again"
  exit 1
fi
wait_for_focus "$left_title"
wait_for_workspace 1

echo "overview arrows navigate cards and workspaces along each output's axis while configured binds keep their physical meaning through the closing zoom"
