#!/usr/bin/env bash
# A direct window-focus-warp request must summon a hidden scratchpad target,
# then focus that exact window. The explicit visible-to-hidden-to-visible
# sequence distinguishes this from a target that merely remained shown.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly TITLE=scratchpad-focus-warp
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/${TITLE}.log"
readonly SCRATCHPAD=focus-target

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[animation.scratchpad]
scale = 0

[[scratchpad]]
name = "focus-target"

[[window_rule]]
match.title = "^scratchpad-focus-warp$"
default_floating = true
default_size = [420, 260]
EOF
"$UMBRIEL" msg config-reload > /dev/null

windows() { "$UMBRIEL" windows --json; }

wait_for_scratchpad_state() {
  local active=$1 state=
  for _ in $(seq 80); do
    state=$(windows)
    if jq -e --arg title "$TITLE" --arg scratchpad "$SCRATCHPAD" --argjson active "$active" '
      any(.[];
        .title == $title
        and .scratchpad == $scratchpad
        and .workspace == ""
        and .active == $active)
    ' <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected '$TITLE' in hidden=$([[ $active == false ]] && echo true || echo false) scratchpad state: $state"
  return 1
}

APP_ID="$TITLE" "$CLIENT" "$TITLE" 420 260 > "$CLIENT_LOG" 2>&1 &

target_id=
for _ in $(seq 80); do
  target_id=$(windows | jq -r --arg title "$TITLE" '.[] | select(.title == $title) | .id')
  [[ -n $target_id && $target_id != null ]] && break
  sleep 0.1
done
if [[ -z $target_id || $target_id == null ]]; then
  echo "scratchpad target did not map: $(windows)"
  exit 1
fi

"$UMBRIEL" msg "window-focus:$target_id" > /dev/null
"$UMBRIEL" msg "window-move-to-scratchpad:$SCRATCHPAD" > /dev/null
wait_for_scratchpad_state false

# Exercise the report's hidden-scratchpad state explicitly rather than relying
# on move-to-scratchpad's initial hidden presentation.
"$UMBRIEL" msg "scratchpad-toggle:$SCRATCHPAD" > /dev/null
wait_for_scratchpad_state true
"$UMBRIEL" msg "scratchpad-toggle:$SCRATCHPAD" > /dev/null
wait_for_scratchpad_state false

"$UMBRIEL" msg "window-focus-warp:$target_id" > /dev/null
wait_for_scratchpad_state true

echo "window-focus-warp summoned and focused the hidden scratchpad target"
