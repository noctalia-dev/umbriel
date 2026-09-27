#!/usr/bin/env bash
# harness: outputs=2
# DPMS is compositor-owned output power, not output removal. A named action must
# remain in effect while another monitor is awake. A bare action must affect
# every configured monitor, ignore the input release that triggered it, then
# wake every monitor on genuinely new input. Two monitors are the whole point,
# which the header directive above asks the harness for.
set -euo pipefail

POINTER=${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}
readonly KEY_O=24

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[keybinds]
"Mod+O" = { action = "dpms-off", repeat = false }
EOF
"$UMBRIEL" msg config-reload > /dev/null

log_mark() { wc -l < "$UMBRIEL_LOG"; }

wait_for_log_since() {
  local mark=$1 pattern=$2
  for _ in $(seq 40); do
    if tail -n +"$((mark + 1))" "$UMBRIEL_LOG" | grep -q "$pattern"; then
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for log: $pattern"
  tail -12 "$UMBRIEL_LOG" | sed 's/^/  | /'
  return 1
}

failures=0

assert_no_log_since() {
  local mark=$1 pattern=$2 message=$3
  sleep 0.1
  if tail -n +"$((mark + 1))" "$UMBRIEL_LOG" | grep -q "$pattern"; then
    echo "$message"
    failures=$((failures + 1))
  fi
}

# A DPMS keybind runs on the key press. Its release and the trailing modifier
# change belong to that same input sequence, so neither may wake the outputs.
mark=$(log_mark)
"$POINTER" 2560 720 mod logo tap "$KEY_O" mod none
wait_for_log_since "$mark" "output 'HEADLESS-1': powered off"
wait_for_log_since "$mark" "output 'HEADLESS-2': powered off"
assert_no_log_since "$mark" "output 'HEADLESS-1': applied mode=" \
  "the key or modifier release that triggered DPMS immediately woke HEADLESS-1"
assert_no_log_since "$mark" "output 'HEADLESS-2': applied mode=" \
  "the key or modifier release that triggered DPMS immediately woke HEADLESS-2"

# Restore a known state even when the assertion above is being sensitivity
# checked against the old behavior, where both outputs are already awake.
"$UMBRIEL" msg dpms-on > /dev/null

# A named action is case-insensitive, changes only the requested monitor, and
# stays in effect during activity on the monitor that remains awake.
mark=$(log_mark)
"$UMBRIEL" msg dpms-off:headless-2 > /dev/null
wait_for_log_since "$mark" "output 'HEADLESS-2': powered off"
if tail -n +"$((mark + 1))" "$UMBRIEL_LOG" | grep -q "output 'HEADLESS-1': powered off"; then
  echo "named DPMS action also powered off HEADLESS-1"
  exit 1
fi

mark=$(log_mark)
"$POINTER" 2560 720 move 10 10
assert_no_log_since "$mark" "output 'HEADLESS-2': applied mode=" \
  "input on the remaining awake monitor woke named DPMS output HEADLESS-2"

# Reapply the named off state so the explicit on transition below is observable
# even during the pre-fix sensitivity run, where pointer motion already woke it.
"$UMBRIEL" msg dpms-off:headless-2 > /dev/null
mark=$(log_mark)
"$UMBRIEL" msg dpms-on:headless-2 > /dev/null
wait_for_log_since "$mark" "output 'HEADLESS-2': applied mode="

# A bare action changes all configured monitors.
mark=$(log_mark)
"$UMBRIEL" msg dpms-off > /dev/null
wait_for_log_since "$mark" "output 'HEADLESS-1': powered off"
wait_for_log_since "$mark" "output 'HEADLESS-2': powered off"

# Pointer motion wakes both without an explicit dpms-on command.
mark=$(log_mark)
"$POINTER" 2560 720 move 20 20
wait_for_log_since "$mark" "output 'HEADLESS-1': applied mode="
wait_for_log_since "$mark" "output 'HEADLESS-2': applied mode="

if ((failures > 0)); then
  exit 1
fi

echo "DPMS ignores triggering releases, preserves named power state, and wakes globally on new input"
