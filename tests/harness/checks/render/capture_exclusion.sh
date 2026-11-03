#!/usr/bin/env bash
# A floating protected window exposes live tiles in capture
# across each tiling layout; toggles preserve allocation and revoke window sources.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
capture_config
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[[window_rule]]
match.title = "^protected$"
block_capture = false
default_floating = true
default_floating_size_px = { width = 1280, height = 720 }
default_position = { x = 0, y = 0, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/capture.png"
capture_clients 300
id=$(window_by_title protected | jq -r .id)
"$UMBRIEL" settle > /dev/null
[[ $(window_by_title protected | jq -c '[.x,.y,.w,.h]') == '[0,0,1280,720]' ]]

# Two active sessions share the owned source. Protection must stop both, and a
# new request must receive a valid, stopped session rather than a protocol error.
for n in 1 2; do
  "$UMBRIEL_CAPTURE_CLIENT" --window "$id" > "$UMBRIEL_RUNTIME_DIR/window-$n.log" 2>&1 &
  await_lines "$UMBRIEL_RUNTIME_DIR/window-$n.log" 'session-ready ' 1
done
capture_set_protection true
for n in 1 2; do await_lines "$UMBRIEL_RUNTIME_DIR/window-$n.log" 'session-stopped ' 1; done
"$UMBRIEL_CAPTURE_CLIENT" --window "$id" > "$UMBRIEL_RUNTIME_DIR/denied.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/denied.log" 'session-stopped ' 1
if rg '^frame |protocol error|invalid object' "$UMBRIEL_RUNTIME_DIR/denied.log"; then exit 1; fi
"$UMBRIEL_CAPTURE_CLIENT" --output HEADLESS-1 > "$UMBRIEL_RUNTIME_DIR/output.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/output.log" 'frame ' 1

repaints=0
for layout in scrolling dwindle master; do
  "$UMBRIEL" msg workspace-set-layout "$layout" > /dev/null
  "$UMBRIEL" settle > /dev/null
  capture_assert_hidden "$IMAGE"
  # A large central rectangle is covered by the local float. Capture must show
  # the two tiled colours there, not a black replacement or a stale image.
  read -r green blue < <("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'g>0.9&&r<0.1&&b<0.1' 'b>0.9&&r<0.1&&g<0.1' '900x500+150+100')
  ((green > 10000 && blue > 10000)) || { echo "$layout missing underlying tiles: $green $blue"; exit 1; }
  printf R >&8
  printf R >&9
  repaints=$((repaints + 1))
  await_lines "$UMBRIEL_RUNTIME_DIR/left.log" 'repainted ' "$repaints"
  await_lines "$UMBRIEL_RUNTIME_DIR/right.log" 'repainted ' "$repaints"
  "$UMBRIEL" settle > /dev/null
  capture_assert_hidden "$IMAGE"
  red=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r>0.9&&g<0.1&&b<0.1' '900x500+150+100')
  ((red == green + blue)) || { echo "$layout did not update both covered tiles: $red != $green + $blue"; exit 1; }
  "$UMBRIEL" msg window-move-by 20 20 --window "$id" > /dev/null
  "$UMBRIEL" settle > /dev/null
  capture_assert_hidden "$IMAGE"
  "$UMBRIEL" msg window-toggle-floating "$id" > /dev/null
  "$UMBRIEL" settle > /dev/null
  before=$(window_by_title protected | jq -c '[.x,.y,.w,.h,.floating]')
  capture_assert_hidden "$IMAGE"
  capture_set_protection false
  "$UMBRIEL" settle > /dev/null
  [[ $(window_by_title protected | jq -c '[.x,.y,.w,.h,.floating]') == "$before" ]]
  grim "$IMAGE"
  visible=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r>0.9&&b>0.9&&g<0.1')
  ((visible > 10000)) || { echo "unprotect did not restore window"; exit 1; }
  capture_set_protection true
  "$UMBRIEL" msg window-toggle-floating "$id" > /dev/null
  printf G >&8
  printf B >&9
  repaints=$((repaints + 1))
  await_lines "$UMBRIEL_RUNTIME_DIR/left.log" 'repainted ' "$repaints"
  await_lines "$UMBRIEL_RUNTIME_DIR/right.log" 'repainted ' "$repaints"
done
capture_set_protection false
"$UMBRIEL_CAPTURE_CLIENT" --window "$id" > "$UMBRIEL_RUNTIME_DIR/fresh.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/fresh.log" 'frame ' 1
echo 'invisible floating and tiled exclusion, live underlying updates, movement, and source revocation verified'
