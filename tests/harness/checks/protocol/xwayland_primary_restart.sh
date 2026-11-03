#!/usr/bin/env bash
# harness: xwayland=native
# harness: outputs=3
# The primary tracker must not keep the lazy Xwayland alive once its last X client closes, and a restarted server must
# publish the configured primary again.
set -euo pipefail

readonly QUERY="$UMBRIEL_XWAYLAND_OUTPUT_CLIENT"
readonly HOLDER="$UMBRIEL_XWAYLAND_FOCUS_CLIENT"
mkfifo "$UMBRIEL_RUNTIME_DIR/primary-control"
exec {control_fd}<>"$UMBRIEL_RUNTIME_DIR/primary-control"

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = false

[output.HEADLESS-1]
mode = "2560x1440@180"
position = [0, 0]

[output.HEADLESS-2]
mode = "2560x1440@60"
position = [2561, 0]

[output.HEADLESS-3]
mode = "1920x1200@60"
position = [-1201, -600]
transform = "90"

[[window_rule]]
match.title = "^primary-holder$"
default_output = "HEADLESS-1"
CONFIG
sed -i '/^\[general\]$/a xwayland_primary_output = "HEADLESS-1"' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

start_holder() {
  "$HOLDER" primary-holder <&"$control_fd" > "$UMBRIEL_RUNTIME_DIR/primary-holder.log" 2>&1 &
  holder_pid=$!
  for _ in $(seq 100); do
    if "$UMBRIEL" windows --json | jq -e 'any(.[]; .title == "primary-holder")' > /dev/null; then
      return
    fi
    sleep 0.02
  done
  echo 'the X11 holder did not map'
  return 1
}

await_primary() {
  local expected=$1 state
  for _ in $(seq 150); do
    state=$("$QUERY")
    if [[ $(jq -r .primary <<< "$state") == "$expected" ]]; then
      return
    fi
    sleep 0.02
  done
  echo "expected primary '$expected', got $state"
  return 1
}

# Finds the server without opening more X connections, which would keep it alive.
xwayland_pid() {
  python3 - "$DISPLAY" <<'PY'
import pathlib
import sys
for path in pathlib.Path('/proc').glob('[0-9]*/cmdline'):
    try:
        args = path.read_bytes().split(b'\0')
        if len(args) > 1 and pathlib.Path(args[0].decode()).name == 'Xwayland' and args[1].decode() == sys.argv[1]:
            print(path.parent.name)
            break
    except (OSError, UnicodeError):
        pass
PY
}

start_holder
await_primary HEADLESS-1

# Close the last external X client and wait for that server to exit.
server_pid=$(xwayland_pid)
[[ -n $server_pid ]]
printf 'quit\n' >&"$control_fd"
wait "$holder_pid"
for _ in $(seq 300); do
  [[ ! -e /proc/$server_pid ]] && break
  sleep 0.1
done
if [[ -e /proc/$server_pid ]]; then
  echo 'the primary tracker kept lazy Xwayland alive'
  exit 1
fi
start_holder
await_primary HEADLESS-1
[[ $(xwayland_pid) != "$server_pid" ]]

echo 'RandR primary lets lazy Xwayland exit and returns after its restart'
