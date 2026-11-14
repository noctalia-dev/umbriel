#!/usr/bin/env bash
# A spawned application inherits a clean signal state. The compositor handles SIGINT and SIGTERM on the event loop, which means libwayland blocks both process-wide via sigprocmask. A blocked signal mask survives fork and exec, so without an explicit reset in the child every application launched from a keybind would silently ignore Ctrl+C and systemd's stop signal.
set -euo pipefail

MARKER="umbriel-harness-spawn-$$"
CHILD=

"$UMBRIEL" msg spawn "python3 -c 'import time; time.sleep(120)' $MARKER" > /dev/null

for _ in $(seq 40); do
  CHILD=$(pgrep -f "$MARKER" | head -1 || true)
  [[ -n $CHILD ]] && break
  sleep 0.25
done
if [[ -z $CHILD ]]; then
  echo "spawned process never appeared"
  exit 1
fi

# SigBlk is a hex bitmask; signal N occupies bit N-1.
blocked=$(awk '/^SigBlk:/ {print $2}' "/proc/$CHILD/status")
if [[ -z $blocked ]]; then
  echo "could not read SigBlk for pid $CHILD"
  exit 1
fi
sigint_bit=$(( 0x$blocked >> 1 & 1 ))   # SIGINT  = 2
sigterm_bit=$(( 0x$blocked >> 14 & 1 )) # SIGTERM = 15

if [[ $sigint_bit -ne 0 || $sigterm_bit -ne 0 ]]; then
  echo "spawned child inherited a blocked signal mask (SigBlk=$blocked)"
  echo "  SIGINT blocked: $sigint_bit, SIGTERM blocked: $sigterm_bit"
  exit 1
fi

# The mask is only half the story: prove the child actually dies on SIGTERM.
kill -TERM "$CHILD"
for _ in $(seq 20); do
  kill -0 "$CHILD" 2>/dev/null || break
  sleep 0.2
done
if kill -0 "$CHILD" 2>/dev/null; then
  echo "spawned child survived SIGTERM"
  exit 1
fi

echo "child signal mask clean, dies on SIGTERM"

# A command that names a program nothing can execute fails the request instead of reporting a launch. Without this
# the shell's own "command not found" is the only trace, on the compositor's stderr.
if "$UMBRIEL" msg spawn umbriel-harness-no-such-program-$$ > "$UMBRIEL_RUNTIME_DIR/spawn-missing.err" 2>&1; then
  echo "spawning a program that does not exist reported success: $(< "$UMBRIEL_RUNTIME_DIR/spawn-missing.err")"
  exit 1
fi
if ! grep -q "umbriel-harness-no-such-program-$$" "$UMBRIEL_RUNTIME_DIR/spawn-missing.err"; then
  echo "the failure did not name the missing program: $(< "$UMBRIEL_RUNTIME_DIR/spawn-missing.err")"
  exit 1
fi

# A command the shell has to interpret still runs; its own failure stays the shell's, so the request succeeds.
"$UMBRIEL" msg spawn "sh -c 'exit 1'" > /dev/null

echo "a missing program fails the request; a shell command still runs"

# A refused spawn from a keybind is consumed like any other rejected action: the chord must not fall through to the
# focused client as a plain key.
readonly KEY_ENTER=28
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly BIND_PROGRAM=umbriel-harness-no-such-bind-program
readonly OBSERVER="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly LEAK_LOG="$UMBRIEL_RUNTIME_DIR/spawn-keybind-leak.log"

printf '\n[keybinds]\n"Mod+Return" = "spawn %s"\n' "$BIND_PROGRAM" >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

"$OBSERVER" spawn-keybind-leak > "$LEAK_LOG" 2>&1 &
for _ in $(seq 60); do
  "$UMBRIEL" windows --json | jq -e 'length == 1' > /dev/null && break
  sleep 0.1
done

"$POINTER" "$OUTPUT_W" "$OUTPUT_H" mod logo pause 300 tap "$KEY_ENTER"
if ! grep -q "cannot find '$BIND_PROGRAM' on PATH" "$UMBRIEL_LOG"; then
  echo "the refused spawn bind never reached the compositor, so the leg proves nothing"
  exit 1
fi
for _ in $(seq 50); do
  grep -q "keyboard-enter" "$LEAK_LOG" && break
  sleep 0.02
done
if ! grep -q "keyboard-enter" "$LEAK_LOG"; then
  echo "the observer never received keyboard focus"
  exit 1
fi
if grep -q "keyboard-key code=$KEY_ENTER" "$LEAK_LOG"; then
  echo "a refused spawn bind handed its key to the focused client: $(grep "keyboard-key code=$KEY_ENTER" "$LEAK_LOG")"
  exit 1
fi

echo "a refused spawn bind keeps its chord"
