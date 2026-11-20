#!/usr/bin/env bash
# A lock client that dies while locked must leave the session locked behind the opaque blank, never the desktop, and a
# fresh lock client must still be able to take over and unlock.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly LAYER_CLIENT="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly LOCK_CLIENT="${UMBRIEL_LOCK_CLIENT:-./build-debug/tests/lock-client}"
readonly PROBE="${UMBRIEL_PIXEL_PROBE:-./build-debug/tests/pixel-probe}"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/frame.png"

assert_pixel() {
  local r g b
  grim -o HEADLESS-1 "$IMAGE"
  read -r r g b < <("$PROBE" "$IMAGE" pixel 32 32)
  if ((r < $1 - 2 || r > $1 + 2 || g < $2 - 2 || g > $2 + 2 || b < $3 - 2 || b > $3 + 2)); then
    echo "$4: expected $1 $2 $3, got $r $g $b"
    exit 1
  fi
}

"$LAYER_CLIENT" HEADLESS-1 0 > "$UMBRIEL_RUNTIME_DIR/background.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/background.log" 'ready$' 1
"$UMBRIEL" settle > /dev/null
assert_pixel 85 119 170 "background layer not shown before locking"

readonly FIRST_LOG="$UMBRIEL_RUNTIME_DIR/lock-first.log"
mkfifo "$UMBRIEL_RUNTIME_DIR/lock-first-control"
exec {first_fd}<> "$UMBRIEL_RUNTIME_DIR/lock-first-control"
"$LOCK_CLIENT" <&"$first_fd" > "$FIRST_LOG" 2>&1 &
first_pid=$!
await_lines "$FIRST_LOG" 'locked$' 1
kill -9 "$first_pid"
wait "$first_pid" 2> /dev/null || true
await_lines "$UMBRIEL_LOG" '.*session lock client destroyed while locked' 1
"$UMBRIEL" settle > /dev/null
assert_pixel 0 0 0 "the desktop showed after the lock client died"

readonly SECOND_LOG="$UMBRIEL_RUNTIME_DIR/lock-second.log"
mkfifo "$UMBRIEL_RUNTIME_DIR/lock-control"
exec {lock_fd}<> "$UMBRIEL_RUNTIME_DIR/lock-control"
"$LOCK_CLIENT" <&"$lock_fd" > "$SECOND_LOG" 2>&1 &
await_lines "$SECOND_LOG" 'locked$' 1
"$UMBRIEL" settle > /dev/null
assert_pixel 16 32 48 "the replacement lock surface was not presented"

echo unlock >&"$lock_fd"
await_lines "$SECOND_LOG" 'unlocked$' 1
"$UMBRIEL" settle > /dev/null
assert_pixel 85 119 170 "the desktop did not return after the replacement client unlocked"
echo "a crashed lock client kept the session blank and a replacement client relocked and unlocked"
