#!/usr/bin/env bash
# A client commit must not discard a compositor-owned window-rule opacity. This
# reproduces browsers that continually redraw after a workspace transition.
set -euo pipefail

readonly SCREENSHOT="$UMBRIEL_RUNTIME_DIR/rule-opacity-commit.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1

[appearance]
border_width = 0
corner_radius = 0
backdrop_color = "#00FF00FF"

[[window_rule]]
match.app_id = "^opacity-commit$"
opacity = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

foot --config=/dev/null --app-id=opacity-commit --override=colors.background=000000 \
  sh -c 'while :; do printf "\\r%08d" "$RANDOM"; sleep 0.02; done' > /dev/null 2>&1 &

for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.1
done
if [[ $("$UMBRIEL" windows --json | jq 'length') -ne 1 ]]; then
  echo "timed out waiting for opacity-commit"
  exit 1
fi

# Switching back applies the view clip, then subsequent client commits must
# preserve the 0.5 compositor opacity rather than restoring opaque black.
"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" msg workspace-switch:1 > /dev/null
sleep 0.5
grim "$SCREENSHOT"

# The sampled pixel is black client content over the solid green compositor
# backdrop. At 0.5 opacity, its encoded green channel is about 128. Opaque
# black, the broken post-commit state, yields zero.
green=$(magick "$SCREENSHOT" -crop 40x40+600+500 -format '%[fx:round(255*mean.g)]' info:)
if (( green < 90 || green > 170 )); then
  echo "rule opacity was lost after the client commit: mean green=$green"
  exit 1
fi

echo "rule opacity survived a post-workspace-switch client commit: mean green=$green"
