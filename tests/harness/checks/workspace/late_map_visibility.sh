#!/usr/bin/env bash
# A window mapped into the outgoing workspace during a slide must be hidden when the slide finishes.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

cat >> "$UMBRIEL_CONFIG" <<'EOF'
[animation.workspaces]
duration_ms = 1000
curve = "linear"

[colors]
backdrop = "#000000FF"

[output."HEADLESS-1"]
workspaces = 2

[[window_rule]]
match.app_id = "^late-map$"
default_workspace = 1
default_focused = false
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" clock-freeze
"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" workspaces --json | jq -e '.[] | select(.name == "2" and .active)' > /dev/null
APP_ID=late-map "$UMBRIEL_UNMAP_CLIENT" late-map 1200 700 > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" 'mapped' 1
"$UMBRIEL" clock-advance 1200
"$UMBRIEL" clock-resume
"$UMBRIEL" settle
grim "$UMBRIEL_RUNTIME_DIR/late-map.png"
blue=$(magick "$UMBRIEL_RUNTIME_DIR/late-map.png" -format '%[fx:round(255*mean.b)]' info:)
if ((blue != 0)); then
  echo "inactive late-mapped window remains visible: mean blue=$blue"
  exit 1
fi
