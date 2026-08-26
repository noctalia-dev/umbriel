#!/usr/bin/env bash
# Floating overlay close should refocus the last focused window, not the neighbor.
# Overlay only unmaps (View stays alive) so we can catch missing unmap-time refocus.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"

wait_for_window_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_focus() {
  local want=$1
  for _ in $(seq 40); do
    if [[ $("$UMBRIEL" windows --json | jq -r --arg id "$want" '.[] | select(.id == $id) | .focused') == true ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $want to be focused: $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[appearance]
animation_ms = 1

[layout]
mode = "scrolling"

[[window_rule]]
match.app_id = "^float-close-overlay$"
default_floating = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

readonly FIRST_LOG="$UMBRIEL_RUNTIME_DIR/first.log"
readonly ANCHOR_LOG="$UMBRIEL_RUNTIME_DIR/anchor.log"
readonly NEIGHBOR_LOG="$UMBRIEL_RUNTIME_DIR/neighbor.log"

"$CLIENT" "float-close-first" > "$FIRST_LOG" 2>&1 &
wait_for_window_count 1
"$CLIENT" "float-close-anchor" > "$ANCHOR_LOG" 2>&1 &
wait_for_window_count 2
"$CLIENT" "float-close-neighbor" > "$NEIGHBOR_LOG" 2>&1 &
wait_for_window_count 3

windows=$("$UMBRIEL" windows --json)
first_id=$(jq -r '.[] | select(.title == "float-close-first") | .id' <<< "$windows")
anchor_id=$(jq -r '.[] | select(.title == "float-close-anchor") | .id' <<< "$windows")
neighbor_id=$(jq -r '.[] | select(.title == "float-close-neighbor") | .id' <<< "$windows")
if [[ $(jq -r --arg id "$first_id" '.[] | select(.id == $id) | .floating' <<< "$windows") != false
    || $(jq -r --arg id "$anchor_id" '.[] | select(.id == $id) | .floating' <<< "$windows") != false
    || $(jq -r --arg id "$neighbor_id" '.[] | select(.id == $id) | .floating' <<< "$windows") != false ]]; then
  echo "expected three tiles before opening the overlay: $windows"
  exit 1
fi

# Focus the middle-created window so MRU restore differs from creation order.
"$UMBRIEL" msg "window-focus:$anchor_id" > /dev/null
wait_for_focus "$anchor_id"

APP_ID=float-close-overlay "$CLIENT" "float-close-overlay" > "$UMBRIEL_RUNTIME_DIR/overlay.log" 2>&1 &
overlay_pid=$!
wait_for_window_count 4

# Window count 4 can race, so wait until the overlay is actually floating.
overlay_id=""
overlay_floating=""
for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  overlay_id=$(jq -r '.[] | select(.title == "float-close-overlay") | .id // empty' <<< "$windows")
  if [[ -n $overlay_id ]]; then
    overlay_floating=$(jq -r --arg id "$overlay_id" '.[] | select(.id == $id) | .floating // empty' <<< "$windows")
    if [[ $overlay_floating == true ]]; then
      break
    fi
  fi
  sleep 0.1
done
if [[ -z $overlay_id ]]; then
  echo "overlay never appeared: $("$UMBRIEL" windows --json)"
  exit 1
fi
if [[ $overlay_floating != true ]]; then
  echo "overlay did not map floating: $("$UMBRIEL" windows --json)"
  exit 1
fi
wait_for_focus "$overlay_id"

"$UMBRIEL" msg "window-close:$overlay_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/overlay.log" && break
  sleep 0.1
done
if ! grep -q '^unmapped$' "$UMBRIEL_RUNTIME_DIR/overlay.log"; then
  echo "overlay never unmapped in response to the close request: $(cat "$UMBRIEL_RUNTIME_DIR/overlay.log")"
  exit 1
fi
# Must refocus at unmap, and keep overlay alive so destroy fallback cannot hide it.
wait_for_window_count 3
if ! kill -0 "$overlay_pid" 2>/dev/null; then
  echo "overlay client exited instead of keeping the surface alive"
  exit 1
fi
wait_for_focus "$anchor_id"
sleep 0.1
windows=$("$UMBRIEL" windows --json)
if [[ $(jq -r --arg id "$anchor_id" '.[] | select(.id == $id) | .focused' <<< "$windows") != true ]]; then
  echo "focus was not restored to the previously focused window at unmap time: $windows"
  exit 1
fi
if [[ $(jq -r --arg id "$first_id" '.[] | select(.id == $id) | .focused' <<< "$windows") != false
    || $(jq -r --arg id "$neighbor_id" '.[] | select(.id == $id) | .focused' <<< "$windows") != false ]]; then
  echo "an unrelated tile received focus after the overlay closed: $windows"
  exit 1
fi

echo "closing a focused floating overlay restores the previously focused window"
