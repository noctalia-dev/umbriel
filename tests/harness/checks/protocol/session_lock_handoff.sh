#!/usr/bin/env bash
# harness: outputs=2
# A lock request keeps the current desktop visible until every active output has a mapped lock surface. Once the
# surfaces are ready, the compositor switches both outputs to secure content and confirms the lock only after those
# frames are presented. A client that never commits still completes through the opaque compositor blank after the
# bounded surface deadline.
set -euo pipefail

readonly LAYER_CLIENT="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly LOCK_CLIENT="${UMBRIEL_LOCK_CLIENT:-./build-debug/tests/lock-client}"
readonly PROBE="${UMBRIEL_PIXEL_PROBE:-./build-debug/tests/pixel-probe}"

wait_for_line() {
  local file=$1 line=$2 attempts=${3:-100}
  for _ in $(seq "$attempts"); do
    grep -qx "$line" "$file" 2>/dev/null && return 0
    sleep 0.05
  done
  echo "never saw '$line' in $file: $(cat "$file" 2>/dev/null || true)"
  return 1
}

assert_pixel() {
  local image=$1 expected_r=$2 expected_g=$3 expected_b=$4 description=$5
  local r g b
  read -r r g b < <("$PROBE" "$image" pixel 32 32)
  if ((r < expected_r - 2 || r > expected_r + 2
      || g < expected_g - 2 || g > expected_g + 2
      || b < expected_b - 2 || b > expected_b + 2)); then
    echo "$description: expected $expected_r $expected_g $expected_b, got $r $g $b"
    return 1
  fi
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#00000000"
EOF
"$UMBRIEL" msg config-reload > /dev/null

for output in HEADLESS-1 HEADLESS-2; do
  log="$UMBRIEL_RUNTIME_DIR/background-$output.log"
  "$LAYER_CLIENT" "$output" 0 > "$log" 2>&1 &
  wait_for_line "$log" ready
done

first_log="$UMBRIEL_RUNTIME_DIR/lock-ready.log"
first_fifo="$UMBRIEL_RUNTIME_DIR/lock-ready-control"
mkfifo "$first_fifo"
exec {first_fd}<> "$first_fifo"
"$LOCK_CLIENT" --defer-commit <&"$first_fd" > "$first_log" 2>&1 &
first_pid=$!
wait_for_line "$first_log" configured
if grep -q '^locked$' "$first_log"; then
  echo "the compositor confirmed the lock before the client committed its surfaces"
  exit 1
fi

for output in HEADLESS-1 HEADLESS-2; do
  image="$UMBRIEL_RUNTIME_DIR/pending-$output.png"
  grim -o "$output" "$image"
  assert_pixel "$image" 85 119 170 "pending lock hid the desktop on $output"
done

echo commit >&"$first_fd"
wait_for_line "$first_log" locked
for output in HEADLESS-1 HEADLESS-2; do
  image="$UMBRIEL_RUNTIME_DIR/ready-$output.png"
  grim -o "$output" "$image"
  assert_pixel "$image" 16 32 48 "ready lock surface was not presented on $output"
done

echo unlock >&"$first_fd"
wait_for_line "$first_log" unlocked
wait "$first_pid"
exec {first_fd}>&-

timeout_log="$UMBRIEL_RUNTIME_DIR/lock-timeout.log"
timeout_fifo="$UMBRIEL_RUNTIME_DIR/lock-timeout-control"
mkfifo "$timeout_fifo"
exec {timeout_fd}<> "$timeout_fifo"
"$LOCK_CLIENT" --defer-commit <&"$timeout_fd" > "$timeout_log" 2>&1 &
timeout_pid=$!
wait_for_line "$timeout_log" configured

sleep 2.5 # real time: the three-second lock surface deadline must not fire early
if grep -q '^locked$' "$timeout_log"; then
  echo "the stalled lock client bypassed the surface deadline"
  exit 1
fi
wait_for_line "$timeout_log" locked 200

for output in HEADLESS-1 HEADLESS-2; do
  image="$UMBRIEL_RUNTIME_DIR/timeout-$output.png"
  grim -o "$output" "$image"
  assert_pixel "$image" 0 0 0 "secure timeout blank was not opaque on $output"
done

echo unlock >&"$timeout_fd"
wait_for_line "$timeout_log" unlocked
wait "$timeout_pid"
exec {timeout_fd}>&-

echo "session lock handoff kept both outputs seamless and retained the secure timeout fallback"
