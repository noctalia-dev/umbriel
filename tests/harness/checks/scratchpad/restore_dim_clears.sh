#!/usr/bin/env bash
# Restoring the last scratchpad window on an output (window-toggle-scratchpad) must fade the
# backdrop dim to 0 and refresh the dim/blur nodes, as hiding it through scratchpad-toggle does.
# Hiding the pad itself clears its visibility at once, but the dim must keep following the
# animated fade to zero instead of cutting above the still-visible windows.
set -euo pipefail

readonly BEFORE="$UMBRIEL_RUNTIME_DIR/scratchpad-restore-before.png"
readonly AFTER="$UMBRIEL_RUNTIME_DIR/scratchpad-restore-after.png"
readonly HIDING="$UMBRIEL_RUNTIME_DIR/scratchpad-restore-hiding.png"
readonly HIDDEN="$UMBRIEL_RUNTIME_DIR/scratchpad-restore-hidden.png"
readonly OTHER="$UMBRIEL_RUNTIME_DIR/scratchpad-restore-other.png"
readonly NONMODAL="$UMBRIEL_RUNTIME_DIR/scratchpad-restore-nonmodal.png"
readonly FADE_MS=400

sample_corner() {
  # Top-left 20x20, well outside where a centered foot window lands on a 1280x720 output.
  "$UMBRIEL_PIXEL_PROBE" "$1" mean 20x20+5+5 | awk '{print $1}'
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
duration_ms = 400
curve = "linear"

# A modal pad with a backdrop, and a non-modal one without.
[[scratchpad]]
name = "modal"

[[scratchpad]]
name = "nonmodal"
modal = false

[[window_rule]]
default_floating = true
default_floating_size_px = { width = 300, height = 200 }

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
"$UMBRIEL" msg window-move-to-scratchpad modal > /dev/null

foot --title=scratch-b sh -c 'sleep 120' > /dev/null 2>&1 &
wait_for_count 2
"$UMBRIEL" msg window-move-to-scratchpad modal > /dev/null

# Show both scratchpad windows: the backdrop dims.
"$UMBRIEL" msg scratchpad-toggle modal > /dev/null
"$UMBRIEL" settle
grim "$BEFORE"
before=$(sample_corner "$BEFORE")
if (( before > 200 )); then
  echo "backdrop did not dim while the scratchpad was showing two windows: corner=$before"
  exit 1
fi

# Restore the first (currently scratchpad-focused) window. One scratchpad window remains, so the
# dim must stay.
"$UMBRIEL" msg window-toggle-scratchpad modal > /dev/null
"$UMBRIEL" settle
if ! wait_for_count 2; then
  echo "restoring the first window changed the window count unexpectedly"
  exit 1
fi

# Focus the remaining scratchpad window, then restore it too. The scratchpad is now empty on this
# output, so the dim must clear.
"$UMBRIEL" msg scratchpad-focus-next modal > /dev/null
sleep 0.1
"$UMBRIEL" msg window-toggle-scratchpad modal > /dev/null
"$UMBRIEL" settle

grim "$AFTER"
after=$(sample_corner "$AFTER")
if (( after < 240 )); then
  echo "backdrop dim remained stuck after every scratchpad window on the output was restored: before=$before after=$after"
  exit 1
fi

# Animation time moves only by clock-advance from here, so the hide sample lands on a known fraction of the fade.
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg scratchpad-toggle modal > /dev/null
"$UMBRIEL" clock-advance "$FADE_MS"

# Hiding the pad clears its visibility flag at once. The dim must still follow its fade: 160 ms of the 400 ms linear
# fade leaves 0.6 of it, so the 0.6 dim reads about 0.64 white instead of the shown 0.4 or the undimmed 1.0.
"$UMBRIEL" msg scratchpad-toggle modal > /dev/null
"$UMBRIEL" clock-advance 160
grim "$HIDING"
hiding=$(sample_corner "$HIDING")
if (( hiding < 130 || hiding > 205 )); then
  echo "backdrop dim did not follow the pad's hide fade: shown=$before hiding=$hiding"
  exit 1
fi
"$UMBRIEL" clock-advance "$FADE_MS"
grim "$HIDDEN"
hidden=$(sample_corner "$HIDDEN")
if (( hidden < 240 )); then
  echo "backdrop dim remained after the pad's fade finished: hiding=$hiding hidden=$hidden"
  exit 1
fi

# A non-modal pad shown while the modal pad is still fading out must not take over the backdrop: the dim keeps
# following the modal pad's own fade, never a pad that has none.
"$UMBRIEL" msg scratchpad-toggle modal > /dev/null
"$UMBRIEL" clock-advance "$FADE_MS"
"$UMBRIEL" msg scratchpad-toggle modal > /dev/null
"$UMBRIEL" clock-advance 160
"$UMBRIEL" msg scratchpad-toggle nonmodal > /dev/null
grim "$OTHER"
other=$(sample_corner "$OTHER")
if (( other < 130 || other > 205 )); then
  echo "a non-modal pad took over the backdrop of a modal pad that was still fading: hiding=$hiding other=$other"
  exit 1
fi

# With the modal pad's fade finished, the non-modal pad alone must leave the backdrop undimmed.
"$UMBRIEL" clock-advance "$FADE_MS"
grim "$NONMODAL"
nonmodal=$(sample_corner "$NONMODAL")
if (( nonmodal < 240 )); then
  echo "a non-modal pad dimmed the backdrop: other=$other nonmodal=$nonmodal"
  exit 1
fi
"$UMBRIEL" msg scratchpad-toggle nonmodal > /dev/null
"$UMBRIEL" clock-advance "$FADE_MS"

echo "backdrop dim cleared once the last scratchpad window was restored and followed the pad's own hide: corner $before -> $after, hide $hiding -> $hidden, non-modal $other/$nonmodal"
