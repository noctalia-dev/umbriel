#!/usr/bin/env bash
# harness: outputs=2
# The config `enabled` key must disable an output live: the commit turns the monitor off, its windows move to a live
# output, and re-enabling restores it. That needs a second monitor, which the header directive above asks the harness
# for.
set -euo pipefail

# The disable and re-enable phases replace the whole config rather than
# appending, because re-enabling means the key is gone again. Keeping the
# instance's pristine config in a variable avoids restating the [general] block
# that keeps Xwayland and the cheatsheet out of this instance.
BASELINE=$(< "$UMBRIEL_CONFIG")

log_mark() { wc -l < "$UMBRIEL_LOG"; }

wait_for_log_since() {
  local mark=$1 pattern=$2
  for _ in $(seq 40); do
    if tail -n +"$((mark + 1))" "$UMBRIEL_LOG" | grep -q "$pattern"; then
      return 0
    fi
    sleep 0.25
  done
  return 1
}

spawn_client() {
  foot --title=output-disable-rehome sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_workspace() {
  local expected=$1 workspace= windows=
  for _ in $(seq 40); do
    windows=$("$UMBRIEL" windows --json)
    if [[ $(jq 'length' <<< "$windows") -ne 1 ]]; then
      sleep 0.1
      continue
    fi
    workspace=$(jq -r '.[0].workspace' <<< "$windows")
    [[ $workspace == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected window workspace '$expected', got '$workspace'"
  return 1
}

spawn_client
wait_for_workspace 'HEADLESS-2:1'

# Disable the output through a live reload.
mark=$(log_mark)
{
  printf '%s\n' "$BASELINE"
  printf '\n[output.HEADLESS-2]\nenabled = false\n'
} > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
if ! wait_for_log_since "$mark" "output 'HEADLESS-2': disabled by config"; then
  echo "output was not disabled on reload"
  tail -8 "$UMBRIEL_LOG" | sed 's/^/  | /'
  exit 1
fi
wait_for_workspace 'HEADLESS-1:1'

# Re-enable: the compositor must survive the disable and come back with a mode.
mark=$(log_mark)
printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
if ! wait_for_log_since "$mark" "output 'HEADLESS-2': applied mode="; then
  echo "output was not re-enabled on reload"
  tail -8 "$UMBRIEL_LOG" | sed 's/^/  | /'
  exit 1
fi

wait_for_workspace 'HEADLESS-2:1'

echo "output disabled and re-enabled through live reload, and its window came back"
