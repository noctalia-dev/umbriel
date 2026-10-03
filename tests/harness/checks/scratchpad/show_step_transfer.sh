#!/usr/bin/env bash
# When the window a scratchpad shows on its own moves to another scratchpad, here through a window rule matching its
# new title, the scratchpad it left hides instead of staying open with nothing in it.
set -euo pipefail

readonly CONTROL="$UMBRIEL_RUNTIME_DIR/transfer-control"
readonly FRAME="$UMBRIEL_RUNTIME_DIR/show-step-transfer.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[colors]
backdrop = "#FFFFFFFF"

[animation.scratchpad]
dim = 0.6
blur = false

[[scratchpad]]
name = "a"

[[scratchpad]]
name = "b"

[[window_rule]]
match.title = "^moved-out$"
default_scratchpad = "b"
EOF
"$UMBRIEL" msg config-reload > /dev/null

windows() { "$UMBRIEL" windows --json; }
field_of() { windows | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'; }

# The backdrop is white away from any window, and the scratchpad dim darkens it while a scratchpad is open.
dimmed() {
  "$UMBRIEL" settle
  grim "$FRAME"
  magick "$FRAME" -crop 20x20+5+5 -format '%[fx:mean < 0.8 ? 1 : 0]' info:
}

mkfifo "$CONTROL"
exec {control_fd}<> "$CONTROL"
foot --title=moving-window sh -c "read -r _ < '$CONTROL'; printf '\\033]2;moved-out\\007'; sleep 120" > /dev/null 2>&1 &
foot_pid=$!
for _ in $(seq 80); do
  [[ -n "$(field_of moving-window id)" ]] && break
  sleep 0.05
done
"$UMBRIEL" msg window-move-to-scratchpad:a > /dev/null

# A second member stays hidden while a shows the moving window on its own.
foot --title=staying-window sh -c "sleep 120" > /dev/null 2>&1 &
stay_pid=$!
for _ in $(seq 80); do
  [[ -n "$(field_of staying-window id)" ]] && break
  sleep 0.05
done
"$UMBRIEL" msg window-move-to-scratchpad:a > /dev/null
"$UMBRIEL" msg scratchpad-show-next:a > /dev/null
if [[ $(windows | jq -r '.[] | select(.active) | .title') != moving-window ]]; then
  "$UMBRIEL" msg scratchpad-show-next:a > /dev/null
fi
if [[ $(dimmed) != 1 ]]; then
  echo "scratchpad a did not open with its window: $(windows)"
  exit 1
fi

# The new title moves the window into the hidden scratchpad b; a must hide rather than stay open and empty.
printf 'go\n' >&"$control_fd"
for _ in $(seq 80); do
  [[ $(field_of moved-out scratchpad) == b ]] && break
  sleep 0.05
done
if [[ $(field_of moved-out scratchpad) != b ]]; then
  echo "the window rule did not move the window into scratchpad b: $(windows)"
  exit 1
fi
if [[ $(dimmed) != 0 ]]; then
  echo "scratchpad a stayed open with nothing in it after its shown window moved to b: $(windows)"
  exit 1
fi

# Closing the moved window and stepping through a again must reach only the window still in it.
kill "$foot_pid"
for _ in $(seq 80); do
  [[ -z "$(field_of moved-out id)" ]] && break
  sleep 0.05
done
"$UMBRIEL" msg scratchpad-show-next:a > /dev/null
if [[ $(windows | jq -r '.[] | select(.active) | .title') != staying-window ]]; then
  echo "stepping through a did not reach the window still in it: $(windows)"
  exit 1
fi
kill "$stay_pid" 2> /dev/null || true

echo "a scratchpad hid when the window it showed on its own moved to another scratchpad"
