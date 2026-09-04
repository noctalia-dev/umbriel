#!/usr/bin/env bash
# A hidden scratchpad view remains rendered while its opacity falls, then leaves the scene when the fade completes.
set -euo pipefail

readonly CLIENT="${UMBRIEL_POPUP_CLIENT:-./build-debug/tests/popup-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/scratchpad-hide-fade-client.log"
readonly BEFORE="$UMBRIEL_RUNTIME_DIR/scratchpad-hide-before.png"
readonly DURING="$UMBRIEL_RUNTIME_DIR/scratchpad-hide-during.png"
readonly AFTER="$UMBRIEL_RUNTIME_DIR/scratchpad-hide-after.png"

sample_blue() {
  magick "$1" -crop 20x20+630+350 -format '%[fx:round(255*mean.b)]' info:
}

pointer() {
  "$POINTER" 1280 720 "$@"
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 2000

[animation.windows_in]
enabled = false

[animation.scratchpad]
enabled = true
duration_ms = 2000
curve = "linear"
dim = 0.0
blur = false

[colors]
backdrop = "#000000FF"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
EOF
"$UMBRIEL" msg config-reload > /dev/null

pointer move 0 0
"$CLIENT" > "$CLIENT_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -q '^ready$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^ready$' "$CLIENT_LOG"; then
  echo "scratchpad fade client never became ready: $(cat "$CLIENT_LOG")"
  exit 1
fi

"$UMBRIEL" msg window-move-to-scratchpad:HEADLESS-1 > /dev/null
sleep 2.1
"$UMBRIEL" msg scratchpad-toggle:HEADLESS-1 > /dev/null
sleep 2.1
grim "$BEFORE"
before_blue=$(sample_blue "$BEFORE")
if (( before_blue < 80 )); then
  echo "scratchpad window was not visible before hide: blue=$before_blue"
  exit 1
fi

# Positive control: the client is pointer-aware and the sampled point is inside the visible scratchpad window.
pointer move 630 350
for _ in $(seq 20); do
  grep -q '^pointer-enter$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^pointer-enter$' "$CLIENT_LOG"; then
  echo "visible scratchpad did not receive the positive-control pointer enter"
  exit 1
fi
enters_before_hide=$(grep -c '^pointer-enter$' "$CLIENT_LOG")
pointer move 0 0

"$UMBRIEL" msg scratchpad-toggle:HEADLESS-1 > /dev/null
sleep 0.3
pointer move 630 350
sleep 0.1
enters_during_fade=$(grep -c '^pointer-enter$' "$CLIENT_LOG")
if (( enters_during_fade != enters_before_hide )); then
  echo "inactive scratchpad received pointer focus during its fade"
  exit 1
fi
grim "$DURING"
during_blue=$(sample_blue "$DURING")
if (( during_blue < 40 )); then
  echo "scratchpad window disappeared before its fade completed: before=$before_blue during=$during_blue"
  exit 1
fi

sleep 1.9
grim "$AFTER"
after_blue=$(sample_blue "$AFTER")
if (( after_blue > 10 )); then
  echo "scratchpad window remained after its fade completed: during=$during_blue after=$after_blue"
  exit 1
fi

echo "scratchpad view faded without pointer focus before scene disable: blue $before_blue -> $during_blue -> $after_blue"
