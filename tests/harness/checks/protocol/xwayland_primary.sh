#!/usr/bin/env bash
# harness: xwayland=native
# harness: outputs=3
# A configured primary stays stable across focus, reload, output replacement and lazy Xwayland restarts. With no
# setting, user-selected RandR state is left alone.
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

[[window_rule]]
match.title = "^primary-other$"
default_output = "HEADLESS-2"
CONFIG
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

configure_primary() {
  local name=$1
  sed -i '/^xwayland_primary_output = /d' "$UMBRIEL_CONFIG"
  if [[ -n $name ]]; then
    sed -i "/^\[general\]$/a xwayland_primary_output = \"$name\"" "$UMBRIEL_CONFIG"
  fi
  "$UMBRIEL" msg config-reload > /dev/null
}

start_holder
await_primary ''
"$QUERY" | jq -e 'any(.outputs[]; .name == "HEADLESS-1" and .x == 1201 and .y == 600)
  and any(.outputs[]; .name == "HEADLESS-3" and .x == 0 and .y == 0 and .w == 1200 and .h == 1920)' > /dev/null

# Without a setting, an external primary survives both no-op and unrelated reloads.
"$QUERY" HEADLESS-3
await_primary HEADLESS-3
"$UMBRIEL" msg config-reload > /dev/null
sed -i '/^\[general\]$/a focus_on_activate = true' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
await_primary HEADLESS-3

# Observe a transition away from the origin monitor, rather than accepting its default state.
configure_primary headless-1
await_primary HEADLESS-1
mkfifo "$UMBRIEL_RUNTIME_DIR/primary-other-control"
exec {other_fd}<>"$UMBRIEL_RUNTIME_DIR/primary-other-control"
"$HOLDER" primary-other <&"$other_fd" > "$UMBRIEL_RUNTIME_DIR/primary-other.log" 2>&1 &
other_pid=$!
for _ in $(seq 100); do
  if "$UMBRIEL" windows --json | jq -e 'any(.[]; .title == "primary-other" and .active
    and (.workspace | startswith("HEADLESS-2:")))' > /dev/null; then
    break
  fi
  sleep 0.02
done
"$UMBRIEL" windows --json | jq -e 'any(.[]; .title == "primary-other" and .active
  and (.workspace | startswith("HEADLESS-2:")))' > /dev/null
"$UMBRIEL" settle > /dev/null
await_primary HEADLESS-1
printf 'quit\n' >&"$other_fd"
wait "$other_pid"
"$UMBRIEL" msg workspace-switch 1 --output HEADLESS-3 > /dev/null
"$UMBRIEL" settle > /dev/null
await_primary HEADLESS-1
"$UMBRIEL" msg workspace-switch 1 --output HEADLESS-2 > /dev/null
"$UMBRIEL" settle > /dev/null
await_primary HEADLESS-1
configure_primary HEADLESS-2
await_primary HEADLESS-2
configure_primary HEADLESS-1
await_primary HEADLESS-1

"$UMBRIEL" msg output-disable HEADLESS-1 > /dev/null
await_primary ''
"$UMBRIEL" msg output-enable HEADLESS-1 > /dev/null
await_primary HEADLESS-1
old_id=$("$QUERY" | jq .id)
"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
await_primary ''
"$UMBRIEL" output-create HEADLESS-1 > /dev/null
await_primary HEADLESS-1
[[ $("$QUERY" | jq .id) != "$old_id" ]]

sed -i '/^position = \[0, 0\]$/a scale = 1.25' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
await_primary HEADLESS-1

configure_primary missing-output
await_primary ''
configure_primary HEADLESS-1
await_primary HEADLESS-1
configure_primary ''
await_primary ''
"$QUERY" HEADLESS-2
"$UMBRIEL" msg config-reload > /dev/null
await_primary HEADLESS-2
configure_primary HEADLESS-1
await_primary HEADLESS-1

# Close the last external X client and wait for that server to exit, without opening more X connections.
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

echo 'RandR primary survives reload, focus, scaling, output replacement and lazy restart'
