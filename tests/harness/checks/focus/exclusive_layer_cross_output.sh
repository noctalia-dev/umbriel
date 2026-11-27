#!/usr/bin/env bash
# harness: outputs=2
# A window chosen while an exclusive layer (a window switcher, a launcher) holds the keyboard receives focus when that
# layer closes, even when it sits on another output than the pointer. The layer release must not fall back to the
# window last focused on the pointer's output. Closing the layer without a choice still restores the previous focus,
# unless keybinds run under the layer switched away from that window's workspace or minimized it.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LAYER_CLIENT="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly NEAR=cross-output-near
readonly FAR=cross-output-far

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[input.focus]
follows_mouse = "off"

[output.HEADLESS-1]
position = [0, 0]
scale = 1

[output.HEADLESS-2]
position = [1280, 0]
scale = 1

[[window_rule]]
match.title = "^cross-output-near$"
default_output = "HEADLESS-1"
default_floating = true

[[window_rule]]
match.title = "^cross-output-far$"
default_output = "HEADLESS-2"
default_floating = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

windows() { "$UMBRIEL" windows --json; }

field_of() {
  windows | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

wait_for_field() {
  local title=$1 field=$2 expected=$3 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$title" "$field")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.05
  done
  echo "expected '$title' field '$field' to be '$expected', got '$actual': $(windows | jq -c 'map({title, active, output})')"
  return 1
}

wait_for_ready() {
  for _ in $(seq 80); do
    grep -q '^ready$' "$1" && return 0
    sleep 0.05
  done
  echo "exclusive layer did not map: $(< "$1")"
  return 1
}

# Focus must still be where it landed after the release settles, not only for one frame.
hold_field() {
  local title=$1 field=$2 expected=$3 actual=
  for _ in $(seq 10); do
    actual=$(field_of "$title" "$field")
    if [[ $actual != "$expected" ]]; then
      echo "'$title' field '$field' became '$actual' after the layer closed: $(windows | jq -c 'map({title, active})')"
      return 1
    fi
    sleep 0.05
  done
}

"$CLIENT" "$NEAR" > "$UMBRIEL_RUNTIME_DIR/$NEAR.log" 2>&1 &
wait_for_field "$NEAR" output HEADLESS-1
"$CLIENT" "$FAR" > "$UMBRIEL_RUNTIME_DIR/$FAR.log" 2>&1 &
wait_for_field "$FAR" output HEADLESS-2
near_id=$(field_of "$NEAR" id)
far_id=$(field_of "$FAR" id)

# Park the pointer inside HEADLESS-1 (the layout starts it at the shared edge); nothing here warps it afterwards.
"$UMBRIEL_POINTER_CLIENT" 2560 720 move 300 600
"$UMBRIEL" msg window-focus "$near_id" > /dev/null
wait_for_field "$NEAR" active true

# Phase one: choose the far window while the layer holds the keyboard, then close the layer.
"$LAYER_CLIENT" HEADLESS-1 40 keyboard=exclusive > "$UMBRIEL_RUNTIME_DIR/layer-choose.log" 2>&1 &
layer_pid=$!
wait_for_ready "$UMBRIEL_RUNTIME_DIR/layer-choose.log"
wait_for_field "$NEAR" active false
"$UMBRIEL" msg window-focus "$far_id" > /dev/null
kill "$layer_pid" 2> /dev/null || true
wait "$layer_pid" 2> /dev/null || true
if ! wait_for_field "$FAR" active true; then
  echo "closing the layer dropped the window chosen on the other output"
  exit 1
fi
hold_field "$FAR" active true
hold_field "$NEAR" active false

# Phase two: a layer closed without a choice gives focus back to the window that had it.
"$LAYER_CLIENT" HEADLESS-1 40 keyboard=exclusive > "$UMBRIEL_RUNTIME_DIR/layer-plain.log" 2>&1 &
layer_pid=$!
wait_for_ready "$UMBRIEL_RUNTIME_DIR/layer-plain.log"
wait_for_field "$FAR" active false
kill "$layer_pid" 2> /dev/null || true
wait "$layer_pid" 2> /dev/null || true
wait_for_field "$FAR" active true
hold_field "$FAR" active true

# Phase three: shell panels often close by dropping their keyboard interactivity while staying mapped for a fade-out.
"$UMBRIEL" msg window-focus "$near_id" > /dev/null
wait_for_field "$NEAR" active true
"$LAYER_CLIENT" HEADLESS-1 40 keyboard=exclusive release-on-escape > "$UMBRIEL_RUNTIME_DIR/layer-release.log" 2>&1 &
layer_pid=$!
wait_for_ready "$UMBRIEL_RUNTIME_DIR/layer-release.log"
wait_for_field "$NEAR" active false
"$UMBRIEL" msg window-focus "$far_id" > /dev/null
"$UMBRIEL_POINTER_CLIENT" 2560 720 tap 1
for _ in $(seq 80); do
  grep -q keyboard-leave "$UMBRIEL_RUNTIME_DIR/layer-release.log" && break
  sleep 0.05
done
if ! wait_for_field "$FAR" active true; then
  echo "releasing the layer's keyboard dropped the window chosen on the other output"
  exit 1
fi
hold_field "$FAR" active true
kill "$layer_pid" 2> /dev/null || true
wait "$layer_pid" 2> /dev/null || true

active_workspace() {
  "$UMBRIEL" workspaces --json | jq -r '.[] | select(.active and .output == "HEADLESS-1") | .index'
}

# Phase four: switching workspace under the layer leaves the previous window behind instead of switching back to it.
"$UMBRIEL" msg window-focus "$near_id" > /dev/null
wait_for_field "$NEAR" active true
"$LAYER_CLIENT" HEADLESS-1 40 keyboard=exclusive > "$UMBRIEL_RUNTIME_DIR/layer-switch.log" 2>&1 &
layer_pid=$!
wait_for_ready "$UMBRIEL_RUNTIME_DIR/layer-switch.log"
wait_for_field "$NEAR" active false
"$UMBRIEL" msg workspace-switch 2 > /dev/null
[[ $(active_workspace) == 2 ]]
kill "$layer_pid" 2> /dev/null || true
wait "$layer_pid" 2> /dev/null || true
for _ in $(seq 10); do
  if [[ $(active_workspace) != 2 ]]; then
    echo "closing the layer switched back to the workspace of the window focused before it: $("$UMBRIEL" workspaces --json | jq -c 'map(select(.active))')"
    exit 1
  fi
  sleep 0.05
done
hold_field "$NEAR" active false
"$UMBRIEL" msg workspace-switch 1 > /dev/null

# Phase five: a window minimized under the layer stays minimized when the layer closes.
"$UMBRIEL" msg window-focus "$near_id" > /dev/null
wait_for_field "$NEAR" active true
"$LAYER_CLIENT" HEADLESS-1 40 keyboard=exclusive > "$UMBRIEL_RUNTIME_DIR/layer-minimize.log" 2>&1 &
layer_pid=$!
wait_for_ready "$UMBRIEL_RUNTIME_DIR/layer-minimize.log"
wait_for_field "$NEAR" active false
"$UMBRIEL" msg window-minimize "$near_id" > /dev/null
wait_for_field "$NEAR" minimized true
kill "$layer_pid" 2> /dev/null || true
wait "$layer_pid" 2> /dev/null || true
hold_field "$NEAR" minimized true
hold_field "$NEAR" active false

echo "a window chosen under an exclusive layer keeps focus across outputs when the layer closes"
