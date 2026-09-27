#!/usr/bin/env bash
# harness: outputs=1
# A tiled xdg_toplevel.resize request transfers the pressed pointer away from
# its client. The compositor must resize through the requested layout edge,
# finish on the raw release, and restore ordinary input for the next surface.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly SOURCE=client-resize-source
readonly TARGET=client-resize-target
readonly RESIZE_DX=120
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly OBSERVER="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly SOURCE_LOG="$UMBRIEL_RUNTIME_DIR/client-resize-source.log"
readonly TARGET_LOG="$UMBRIEL_RUNTIME_DIR/client-resize-target.log"
readonly POINTER_LOG="$UMBRIEL_RUNTIME_DIR/client-resize-pointer.log"

if [[ ! -x $POINTER || ! -x $OBSERVER ]]; then
  echo "input helpers are not available"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[layout.scrolling]
default_extent_fraction = 0.35
center_underfull_strip = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

await_windows() {
  local expected=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $expected ]] && return 0
    sleep 0.1
  done
  echo "expected $expected windows, got $("$UMBRIEL" windows --json | jq -c 'map(.title)')"
  return 1
}

window_box() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" \
    '.[] | select(.title == $title) | "\(.x) \(.y) \(.w) \(.h)"'
}

wait_for_layout() {
  local expected_width=$1 expected_target_x=$2 actual_width= actual_target_x=
  for _ in $(seq 40); do
    actual_width=$("$UMBRIEL" windows --json | jq -r --arg title "$SOURCE" \
      '.[] | select(.title == $title) | .w')
    actual_target_x=$("$UMBRIEL" windows --json | jq -r --arg title "$TARGET" \
      '.[] | select(.title == $title) | .x')
    [[ $actual_width == "$expected_width" && $actual_target_x == "$expected_target_x" ]] && return 0
    sleep 0.1
  done
  echo "expected source width $expected_width and target x $expected_target_x, got $actual_width and $actual_target_x"
  echo "windows: $("$UMBRIEL" windows --json | jq -c '[.[] | {title, x, y, w, h}]')"
  return 1
}

"$OBSERVER" "$SOURCE" resize-on-press > "$SOURCE_LOG" 2>&1 &
source_pid=$!
await_windows 1
"$OBSERVER" "$TARGET" > "$TARGET_LOG" 2>&1 &
target_pid=$!
await_windows 2

# Mapping is visible to IPC before the deferred layout places the new neighbor.
for _ in $(seq 40); do
  read -r source_x source_y source_w source_h < <(window_box "$SOURCE")
  read -r target_x target_y target_w target_h < <(window_box "$TARGET")
  (( target_x >= source_x + source_w )) && break
  sleep 0.05
done
resize_start_x=$((source_x + source_w - 20))
resize_y=$((source_y + source_h / 2))
resize_end_x=$((resize_start_x + RESIZE_DX))
target_click_x=$((target_x + RESIZE_DX + target_w / 2))
target_click_y=$((target_y + target_h / 2))

# One virtual device preserves the physical press across the client's request.
# The second pause lets the request reach the compositor before resize motion.
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" \
  move "$resize_start_x" "$resize_y" pause 300 \
  press "$BTN_LEFT" pause 300 \
  move "$resize_end_x" "$resize_y" pause 100 release "$BTN_LEFT" pause 100 \
  move "$target_click_x" "$target_click_y" pause 100 click "$BTN_LEFT" \
  > "$POINTER_LOG" 2>&1 || {
  echo "pointer client failed: $(< "$POINTER_LOG")"
  exit 1
}

expected_source_w=$((source_w + RESIZE_DX))
expected_target_x=$((target_x + RESIZE_DX))
wait_for_layout "$expected_source_w" "$expected_target_x"

source_sequence=$(grep -E 'pointer-(enter|leave|button)|resize-requested' "$SOURCE_LOG" | tr '\n' '|')
if [[ $source_sequence != *"pointer-button code=$BTN_LEFT state=pressed|resize-requested edge=right|pointer-leave"* ]]; then
  echo "the tiled resize request was not accepted with a pointer leave: $source_sequence"
  exit 1
fi

target_sequence=$(grep -E 'pointer-(enter|leave|button)' "$TARGET_LOG" | tr '\n' '|')
if [[ $target_sequence != *"pointer-enter|pointer-button code=$BTN_LEFT state=pressed|pointer-button code=$BTN_LEFT state=released"* ]]; then
  echo "the target did not receive a normal click after the resize release: $target_sequence"
  exit 1
fi
target_presses=$(grep -c "pointer-button code=$BTN_LEFT state=pressed" "$TARGET_LOG" || true)
target_releases=$(grep -c "pointer-button code=$BTN_LEFT state=released" "$TARGET_LOG" || true)
if ((target_presses != 1 || target_releases != 1)); then
  echo "expected one target press and release, got $target_presses and $target_releases"
  exit 1
fi

echo "client-requested tiled resize changed the layout and released input to its neighbor"

# A client may acknowledge a configure without changing its buffer. Releasing a
# resize must restore 1:1 sampling for it and its resized sibling without waiting
# for another client commit.
kill "$source_pid" "$target_pid"
wait "$source_pid" "$target_pid" 2>/dev/null || true
await_windows 0
"$UMBRIEL" msg workspace-set-layout:dwindle > /dev/null
readonly PATTERN="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
HOLD_SIZE=1 "$PATTERN" "$SOURCE" 1200 700 > "$SOURCE_LOG" 2>&1 &
await_windows 1
HOLD_SIZE=1 "$PATTERN" "$TARGET" 1200 700 > "$TARGET_LOG" 2>&1 &
await_windows 2
"$UMBRIEL" settle
read -r source_x source_y source_w source_h < <(window_box "$SOURCE")
read -r target_x target_y target_w target_h < <(window_box "$TARGET")
resize_start_x=$((target_x - 30))
resize_y=$((source_y + 200))
readonly HELD="$UMBRIEL_RUNTIME_DIR/resize-held.png"
readonly RELEASED="$UMBRIEL_RUNTIME_DIR/resize-released.png"

impure_pixels() {
  magick "$1" -alpha off -crop "120x1+$2+$resize_y" +repage -depth 8 txt:- |
    tail -n +2 | grep -c -E -v '#(0000FF|00FF00) ' || true
}

pointer_hold "$OUTPUT_W" "$OUTPUT_H" \
  move "$resize_start_x" "$resize_y" mod logo press 273 move "$((resize_start_x - 160))" "$resize_y" \
  -- release 273 mod none
grim -o HEADLESS-1 "$HELD"
pointer_release
"$UMBRIEL" settle
grim -o HEADLESS-1 "$RELEASED"
read -r target_x target_y target_w target_h < <(window_box "$TARGET")
for sample_x in "$((source_x + 40))" "$((target_x + 40))"; do
  held_impure=$(impure_pixels "$HELD" "$sample_x")
  released_impure=$(impure_pixels "$RELEASED" "$sample_x")
  if (( held_impure == 0 || released_impure != 0 )); then
    echo "resize sampling did not return to 1:1 at x=$sample_x: held=$held_impure released=$released_impure"
    exit 1
  fi
done
echo "resize release restored 1:1 sampling for both fixed-buffer tiles"
