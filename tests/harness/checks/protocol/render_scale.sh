#!/usr/bin/env bash
# harness: outputs=1
# render_scale supersedes the logical scale in the client scale hints alone: a
# fractional client must be asked to render at render_scale (its buffer grows to
# floor(logical x render_scale)) while the output keeps presenting the logical
# scale. Dropping the key on a reload must re-notify the client back to the
# logical scale.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/render-scale-client.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.title = "^render-scale$"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 40, y = 40, anchor = "top_left" }

[output."HEADLESS-1"]
scale = 1.25
render_scale = 2.0
workspaces = 2
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" render-scale 400 300 > "$CLIENT_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -q '^mapped ' "$CLIENT_LOG" && break
  sleep 0.05
done

presented=$(grep '^mapped ' "$CLIENT_LOG" | tail -1)
echo "presented at render_scale: $presented"
if [[ $presented != *"buffer 800x600"* || $presented != *"scale 240/120"* ]]; then
  echo "the client was not asked to render at render_scale 2.0"
  exit 1
fi

scale_shown=$("$UMBRIEL" outputs | awk '/Scale:/ { print $2; exit }')
render_shown=$("$UMBRIEL" outputs | awk '/Render scale:/ { print $3; exit }')
echo "outputs: Scale=$scale_shown Render scale=$render_shown"
awk -v a="$scale_shown" 'BEGIN { exit !(a + 0 == 1.25) }' || {
  echo "the output stopped presenting the logical scale"
  exit 1
}
awk -v a="$render_shown" 'BEGIN { exit !(a + 0 == 2.0) }' || {
  echo "umbriel outputs does not report the render scale"
  exit 1
}

before=$(grep -c '^mapped ' "$CLIENT_LOG")
sed -i '/render_scale/d' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
if ! await_lines "$CLIENT_LOG" 'mapped ' $((before + 1)) 0.05; then
  echo "dropping render_scale produced no new presentation to assert on"
  exit 1
fi

presented=$(grep '^mapped ' "$CLIENT_LOG" | tail -1)
echo "presented after reload: $presented"
if [[ $presented != *"buffer 500x375"* || $presented != *"scale 150/120"* ]]; then
  echo "dropping render_scale did not re-notify the client back to the logical scale"
  exit 1
fi

# A render scale below the logical scale is legal and must reach the client
# unchanged.
before=$(grep -c '^mapped ' "$CLIENT_LOG")
printf 'render_scale = 0.5\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
if ! await_lines "$CLIENT_LOG" 'mapped ' $((before + 1)) 0.05; then
  echo "a render_scale below the logical scale produced no new presentation to assert on"
  exit 1
fi

presented=$(grep '^mapped ' "$CLIENT_LOG" | tail -1)
echo "presented below the logical scale: $presented"
if [[ $presented != *"buffer 200x150"* || $presented != *"scale 60/120"* ]]; then
  echo "a render_scale below the logical scale did not reach the client"
  exit 1
fi

# Hiding and revealing the surface (a workspace round-trip) makes the scene
# re-derive the hint: a sub-1 render scale must survive the re-derivation too.
# The reveal re-presents the surface, so assert on a FRESH mapped line
# (count-polled): a same-scale re-notify is deduped below the wire, and reading
# the previous line would pass vacuously without the round-trip doing anything.
before=$(grep -c '^mapped ' "$CLIENT_LOG")
"$UMBRIEL" msg workspace-next > /dev/null
"$UMBRIEL" msg workspace-previous > /dev/null
if ! await_lines "$CLIENT_LOG" 'mapped ' $((before + 1)) 0.05; then
  echo "the workspace round-trip produced no new presentation to assert on"
  exit 1
fi
presented=$(grep '^mapped ' "$CLIENT_LOG" | tail -1)
echo "presented after a workspace round-trip: $presented"
if [[ $presented != *"scale 60/120"* ]]; then
  echo "the scene re-derivation dropped the below-logical render scale ($presented)"
  exit 1
fi
echo "render_scale drives the client hints and falls back to the logical scale on reload"
