#!/usr/bin/env bash
# A hot corner runs its action while the overview is open, the same as on the desktop, including when the shell
# maps its own exclusive-keyboard overview chrome (Noctalia's type-to-launch proxy) over it. That layer is part of
# the overview, not a modal, so it must not swallow corners; on the desktop the same focused modal layer still
# blocks them.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LAYER="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

active_workspace() {
  "$UMBRIEL" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .active) | .index'
}

wait_for_workspace() {
  local want=$1 actual=
  for _ in $(seq 40); do
    actual=$(active_workspace)
    [[ $actual == "$want" ]] && return 0
    sleep 0.05
  done
  echo "$2: active workspace is ${actual:-missing}, wanted $want"
  return 1
}

wait_exclusive_layer() {
  for _ in $(seq 40); do
    if "$UMBRIEL" layers --json | jq -e 'any(.[]; .mapped and .keyboard_interactivity == "exclusive")' > /dev/null; then
      return 0
    fi
    sleep 0.05
  done
  echo "exclusive-keyboard layer did not map: $("$UMBRIEL" layers --json)"
  return 1
}

pointer move 640 360
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output.HEADLESS-1]
workspaces = 2

[animation]
enabled = false

[input.focus]
follows_mouse = false

[hot_corners.top_left]
enabled = true
delay_ms = 100
action = "workspace-switch:2/HEADLESS-1"
EOF
"$UMBRIEL" msg config-reload > /dev/null

"$CLIENT" hot-corner-overview > "$UMBRIEL_RUNTIME_DIR/hot-corner-overview.log" 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.1
done

# Control: the corner works on the desktop.
pointer move 0 0 pause 250
wait_for_workspace 2 "corner did not fire on the desktop"
"$UMBRIEL" msg workspace-switch:1/HEADLESS-1 > /dev/null
wait_for_workspace 1 "could not return to workspace 1"
pointer move 640 360

"$UMBRIEL" msg overview-open > /dev/null
pointer move 0 0 pause 250
wait_for_workspace 2 "corner did not fire in the overview"

# The shell's overview chrome is an exclusive-keyboard layer mapped over the overview (Noctalia's type-to-launch
# keyboard capture). It is part of the overview, not a modal, so the corner must keep firing beside it.
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" msg workspace-switch:1/HEADLESS-1 > /dev/null
wait_for_workspace 1 "could not return to workspace 1"
"$LAYER" HEADLESS-1 40 keyboard=exclusive > "$UMBRIEL_RUNTIME_DIR/hot-corner-layer.log" 2>&1 &
layer_pid=$!
wait_exclusive_layer
"$UMBRIEL" msg overview-open > /dev/null
pointer move 640 360
pointer move 0 0 pause 250
wait_for_workspace 2 "corner did not fire in the overview beside an exclusive-keyboard layer"

# The modal gate itself stays: on the desktop the same focused exclusive layer owns the corner's input.
"$UMBRIEL" msg overview-close > /dev/null
"$UMBRIEL" msg workspace-switch:1/HEADLESS-1 > /dev/null
wait_for_workspace 1 "could not return to workspace 1"
pointer move 640 360
pointer move 0 0 pause 250
if [[ $(active_workspace) != 1 ]]; then
  echo "corner fired on the desktop behind a focused exclusive-keyboard layer"
  exit 1
fi

# The close phase keeps the gate too: the overview is no longer interactive while it animates away, so the corner
# stays suppressed behind the exclusive layer until the close settles. A long close makes the phase observable
# (with animations off the close completes synchronously and nothing can enter the corner mid-close), and the
# corner recovers once the modal is gone.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation.overview]
curve = "linear"
duration_ms = 2000
EOF
sed -i 's/^enabled = false$/enabled = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
pointer move 640 360
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg overview-close > /dev/null
pointer move 0 0 pause 250
if [[ $(active_workspace) != 1 ]]; then
  echo "corner fired behind the exclusive layer while the overview was closing"
  exit 1
fi
"$UMBRIEL" settle > /dev/null
if [[ $(active_workspace) != 1 ]]; then
  echo "corner fired behind the exclusive layer after the overview closed"
  exit 1
fi

kill "$layer_pid" 2>/dev/null || true
for _ in $(seq 40); do
  "$UMBRIEL" layers --json | jq -e 'any(.[]; .mapped and .keyboard_interactivity == "exclusive")' > /dev/null || break
  sleep 0.05
done
pointer move 640 360
pointer move 0 0 pause 250
wait_for_workspace 2 "corner did not recover after the modal layer left"

echo "hot corners fire in the overview, keep the modal gate while it closes, and recover behind no modal"
