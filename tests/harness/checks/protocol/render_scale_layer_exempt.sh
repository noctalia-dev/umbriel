#!/usr/bin/env bash
# harness: outputs=1
# Layer surfaces supersample like windows, and a `supersample = false` layer rule
# keeps one at the logical scale: a screenshot overlay showing a frozen capture
# would otherwise scale that capture up to the render scale and back down.
# Reloading the rule away supersamples the mapped surface again, and a surface
# that was unmapped when the rule arrived carries it from the commit that maps
# it. A popup follows its layer surface, but the test client logs no popup hint,
# so that leg is not pinned here.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
source "$UMBRIEL_REPO/tests/harness/scale_lib.sh"

readonly LOG="$UMBRIEL_RUNTIME_DIR/render-scale-layer.log"

# Appends the layer rule the client's namespace matches, supersampling or not.
write_rule() { # true or false
  cat >> "$UMBRIEL_CONFIG" <<RULE

[[layer_rule]]
match.namespace = "^umbriel-output-restore-regression$"
supersample = $1
RULE
}

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[output."HEADLESS-1"]
scale = 1.25
render_scale = 2.0
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 overlay-layer log-scale > "$LOG" 2>&1 &
hint=$(nth_hint "$LOG" 'scale ' 1)
echo "overlay layer: $hint"
expect_hint "$hint" "scale 240/120" "a layer surface was not supersampled"

# A rule added after the surface mapped reaches it on reload.
before=$(grep -c 'scale ' "$LOG")
write_rule false
"$UMBRIEL" msg config-reload > /dev/null
hint=$(nth_hint "$LOG" 'scale ' $((before + 1)))
echo "overlay layer with supersample = false: $hint"
expect_hint "$hint" "scale 150/120" "supersample = false did not keep the layer surface at the logical scale"

before=$(grep -c 'scale ' "$LOG")
sed -i 's/^supersample = false$/supersample = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
hint=$(nth_hint "$LOG" 'scale ' $((before + 1)))
echo "overlay layer with supersample = true: $hint"
expect_hint "$hint" "scale 240/120" "lifting the layer rule did not supersample the surface again"

# A rule already in effect when the surface is created is the usual case: a config file, not a reload.
sed -i 's/^supersample = true$/supersample = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
readonly MAPPED_LOG="$UMBRIEL_RUNTIME_DIR/render-scale-layer-mapped.log"
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 overlay-layer log-scale > "$MAPPED_LOG" 2>&1 &
hint=$(nth_hint "$MAPPED_LOG" 'scale ' 1)
echo "overlay layer created under the rule: $hint"
expect_hint "$hint" "scale 150/120" "a rule in place at creation did not keep the layer surface at the logical scale"

# A reload only reaches mapped surfaces, so a rule that arrives while the surface is still uncommitted has to be
# resolved again by the commit that maps it.
sed -i 's/^supersample = false$/supersample = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
readonly HELD_LOG="$UMBRIEL_RUNTIME_DIR/render-scale-layer-held.log"
readonly HELD_FIFO="$UMBRIEL_RUNTIME_DIR/render-scale-layer-held.fifo"
mkfifo "$HELD_FIFO"
LAYER_CONTROL=1 "$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 overlay-layer log-scale hold-map < "$HELD_FIFO" > "$HELD_LOG" 2>&1 &
exec 5> "$HELD_FIFO"
await_lines "$HELD_LOG" 'held$' 1 || exit 1
hint=$(nth_hint "$HELD_LOG" 'scale ' 1)
expect_hint "$hint" "scale 240/120" "an uncommitted layer surface was not supersampled"
sed -i 's/^supersample = true$/supersample = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
printf 'map\n' >&5
hint=$(nth_hint "$HELD_LOG" 'scale ' 2)
echo "overlay layer mapped after the rule arrived: $hint"
expect_hint "$hint" "scale 150/120" "a rule that arrived before the first commit did not reach the surface"

echo "layer surfaces supersample by default, and a supersample layer rule reaches them on reload, at creation, and on the commit that maps them"
