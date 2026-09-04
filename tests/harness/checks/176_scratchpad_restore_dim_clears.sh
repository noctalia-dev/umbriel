#!/usr/bin/env bash
# Regression for a stuck backdrop dim: restoring the last scratchpad window on an output (via
# window-toggle-scratchpad, which calls ScratchpadManager::restoreFocused) must retarget the
# backdrop fade to 0 and refresh the dim/blur nodes, same as ScratchpadManager::setVisible does
# when the scratchpad is hidden through scratchpad-toggle. Before the fix, restoreFocused and
# remove() dropped the output from m_visibleOutputs without touching the backdrop fade, so the
# dim rect stayed at its last alpha forever once every scratchpad window on that output was gone.
set -euo pipefail

readonly BEFORE="$UMBRIEL_RUNTIME_DIR/scratchpad-restore-before.png"
readonly AFTER="$UMBRIEL_RUNTIME_DIR/scratchpad-restore-after.png"

sample_corner() {
  # Top-left 20x20, well outside where a centered foot window lands on a 1280x720 output.
  magick "$1" -crop 20x20+5+5 -format '%[fx:round(255*mean.r)]' info:
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#FFFFFFFF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[animation.scratchpad]
dim = 0.6

[[window_rule]]
default_floating = true
default_size = [300, 200]

[window_rule.match]
title = "^scratch-"
EOF
"$UMBRIEL" msg config-reload > /dev/null

wait_for_count() {
  local expected=$1 count=
  for _ in $(seq 40); do
    count=$("$UMBRIEL" windows --json | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected window(s), got $count"
  return 1
}

foot --title=scratch-a sh -c 'sleep 120' > /dev/null 2>&1 &
wait_for_count 1
"$UMBRIEL" msg window-move-to-scratchpad:HEADLESS-1 > /dev/null

foot --title=scratch-b sh -c 'sleep 120' > /dev/null 2>&1 &
wait_for_count 2
"$UMBRIEL" msg window-move-to-scratchpad:HEADLESS-1 > /dev/null

# Show both scratchpad windows: the backdrop dims.
"$UMBRIEL" msg scratchpad-toggle:HEADLESS-1 > /dev/null
sleep 0.3
grim "$BEFORE"
before=$(sample_corner "$BEFORE")
if (( before > 200 )); then
  echo "backdrop did not dim while the scratchpad was showing two windows: corner=$before"
  exit 1
fi

# Restore the first (currently scratchpad-focused) window. One scratchpad window remains, so the
# dim must stay.
"$UMBRIEL" msg window-toggle-scratchpad:HEADLESS-1 > /dev/null
sleep 0.3
if ! wait_for_count 2; then
  echo "restoring the first window changed the window count unexpectedly"
  exit 1
fi

# Focus the remaining scratchpad window, then restore it too. The scratchpad is now empty on this
# output, so the dim must clear.
"$UMBRIEL" msg scratchpad-focus-next:HEADLESS-1 > /dev/null
sleep 0.1
"$UMBRIEL" msg window-toggle-scratchpad:HEADLESS-1 > /dev/null
sleep 0.3

grim "$AFTER"
after=$(sample_corner "$AFTER")
if (( after < 240 )); then
  echo "backdrop dim remained stuck after every scratchpad window on the output was restored: before=$before after=$after"
  exit 1
fi

echo "backdrop dim cleared once the last scratchpad window was restored: corner $before -> $after"
