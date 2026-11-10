#!/usr/bin/env bash
# harness: outputs=1
# Layer surfaces supersample like windows, and a `supersample = false` layer rule
# keeps one at the logical scale: a screenshot overlay showing a frozen capture
# would otherwise scale that capture up to the render scale and back down.
# Reloading the rule away supersamples the mapped surface again.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly LOG="$UMBRIEL_RUNTIME_DIR/render-scale-layer.log"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output."HEADLESS-1"]
scale = 1.25
render_scale = 2.0
EOF
"$UMBRIEL" msg config-reload > /dev/null

# Waits until `log` holds `count` scale hints and prints the newest one.
nth_scale() { # log, count
  await_lines "$1" 'scale ' "$2" 0.05 > /dev/null || {
    echo "no scale hint #$2 in $1: $(tr '\n' '|' < "$1")" >&2
    return 1
  }
  grep '^scale ' "$1" | tail -1
}

expect() { # hint, required fragment, failure message
  if [[ $1 != *"$2"* ]]; then
    echo "$3: $1"
    exit 1
  fi
}

"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 overlay-layer log-scale > "$LOG" 2>&1 &
hint=$(nth_scale "$LOG" 1)
echo "overlay layer: $hint"
expect "$hint" "scale 240/120" "a layer surface was not supersampled"

before=$(grep -c '^scale ' "$LOG")
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[layer_rule]]
match.namespace = "^umbriel-output-restore-regression$"
supersample = false
EOF
"$UMBRIEL" msg config-reload > /dev/null
hint=$(nth_scale "$LOG" $((before + 1)))
echo "overlay layer with supersample = false: $hint"
expect "$hint" "scale 150/120" "supersample = false did not keep the layer surface at the logical scale"

before=$(grep -c '^scale ' "$LOG")
sed -i 's/^supersample = false$/supersample = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
hint=$(nth_scale "$LOG" $((before + 1)))
echo "overlay layer with supersample = true: $hint"
expect "$hint" "scale 240/120" "lifting the layer rule did not supersample the surface again"

echo "layer surfaces supersample by default and a supersample layer rule overrides it on reload"
