#!/usr/bin/env bash
# spawn-floating opens the launched window floating where spawn tiles it, and an explicit default_floating rule
# still decides.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="$UMBRIEL_UNMAP_CLIENT"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.title = "^ruled$"
default_floating = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

launch() {
  "$UMBRIEL" msg "$1" "exec '$CLIENT' '$2' 300 200 > '$UMBRIEL_RUNTIME_DIR/$2.log' 2>&1" > /dev/null
  await_window "$2"
}
launch spawn tiled
launch spawn-floating floated
launch spawn-floating ruled
"$UMBRIEL" settle
state=$("$UMBRIEL" windows --json)
floating() { jq -r --arg title "$1" '.[] | select(.title == $title) | .floating' <<< "$state"; }
if [[ $(floating tiled) != false || $(floating floated) != true || $(floating ruled) != false ]]; then
  echo "spawn-floating did not decide the opening floating state: $state"
  exit 1
fi
echo "spawn-floating opens its window floating unless a default_floating rule decides"
