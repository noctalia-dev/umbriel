#!/usr/bin/env bash
# A configured dynamic name must not consume either sentinel when all 64 slots are in use. It stays pending while
# every ordinary workspace is occupied, then claims the exact ordinary workspace that later becomes empty.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly BASELINE="$(< "$UMBRIEL_CONFIG")"
declare -a CLIENT_PIDS=()

cleanup() {
  local pid
  for pid in "${CLIENT_PIDS[@]}"; do
    if [[ -n $pid ]]; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
}
trap cleanup EXIT

workspace_snapshot() {
  "$UMBRIEL" workspaces --json |
    jq -c '[.[] | select(.output == "HEADLESS-1")] | sort_by(.index)'
}

wait_for_window_count() {
  local expected=$1 snapshot=
  for _ in $(seq 240); do
    snapshot=$("$UMBRIEL" windows --json)
    [[ $(jq 'length' <<< "$snapshot") -eq $expected ]] && return 0
    sleep 0.05
  done
  echo "expected $expected capacity clients, got: $snapshot"
  exit 1
}

printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[workspaces]
empty_above = true

[output.HEADLESS-1]
workspaces = "dynamic"
min_workspaces = 64
EOF

for index in $(seq 2 63); do
  cat >> "$UMBRIEL_CONFIG" <<EOF

[[window_rule]]
match.app_id = "^dynamic-capacity-$index$"
default_output = "HEADLESS-1"
default_workspace = $index
default_focused = false
EOF
done
"$UMBRIEL" msg config-reload > /dev/null

initial=$(workspace_snapshot)
if [[ $(jq 'length' <<< "$initial") -ne 64 ]]; then
  echo "min_workspaces did not create 64 anonymous entries: $initial"
  exit 1
fi
if ! jq -e 'all(.[]; .named == false)' <<< "$initial" > /dev/null; then
  echo "capacity precondition unexpectedly contained a named workspace: $initial"
  exit 1
fi

for index in $(seq 2 63); do
  APP_ID="dynamic-capacity-$index" "$CLIENT" "dynamic-capacity-$index" > /dev/null 2>&1 &
  CLIENT_PIDS[$index]=$!
done
wait_for_window_count 62

occupied_ids=$("$UMBRIEL" windows --json | jq -r 'map(.workspace) | unique | length')
if [[ $occupied_ids -ne 62 ]]; then
  echo "capacity clients did not occupy 62 distinct workspaces: $("$UMBRIEL" windows --json)"
  exit 1
fi

before_name=$(workspace_snapshot)
leading_id=$(jq -r '.[] | select(.index == 1) | .id' <<< "$before_name")
candidate_id=$(jq -r '.[] | select(.index == 32) | .id' <<< "$before_name")
sentinel_id=$(jq -r '.[] | select(.index == 64) | .id' <<< "$before_name")
occupied_workspace_ids=$("$UMBRIEL" windows --json | jq -c 'map(.workspace)')
if [[ -z $leading_id || -z $candidate_id || -z $sentinel_id ]] \
    || ! jq -e --argjson occupied "$occupied_workspace_ids" '
      (.[] | select(.index == 1) | .id) as $leading
      | (.[] | select(.index == 32) | .id) as $candidate
      | (.[] | select(.index == 64) | .id) as $sentinel
      | ($occupied | index($leading)) == null
        and ($occupied | index($candidate)) != null
        and ($occupied | index($sentinel)) == null
    ' <<< "$before_name" > /dev/null; then
  echo "capacity occupancy did not leave only the edge sentinels empty: workspaces=$before_name"
  exit 1
fi

readonly FULL_CONFIG="$(< "$UMBRIEL_CONFIG")"
printf '%s\n' "$FULL_CONFIG" > "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[workspace]]
name = "PENDING"
output = "HEADLESS-1"
EOF
"$UMBRIEL" msg config-reload > /dev/null

pending=$(workspace_snapshot)
if jq -e 'any(.[]; .name == "PENDING" or .named)' <<< "$pending" > /dev/null; then
  echo "pending name consumed an occupied workspace or an edge sentinel: $pending"
  exit 1
fi
if [[ $(jq -r '.[] | select(.index == 1) | .id' <<< "$pending") != "$leading_id" \
      || $(jq -r '.[] | select(.index == 64) | .id' <<< "$pending") != "$sentinel_id" ]]; then
  echo "pending name replaced an edge sentinel: $pending"
  exit 1
fi

kill "${CLIENT_PIDS[32]}"
wait "${CLIENT_PIDS[32]}" 2>/dev/null || true
CLIENT_PIDS[32]=
wait_for_window_count 61

settled=
for _ in $(seq 120); do
  settled=$(workspace_snapshot)
  if jq -e --arg leading "$leading_id" --arg candidate "$candidate_id" --arg sentinel "$sentinel_id" '
    length == 64
    and any(.[]; .id == $leading and .index == 1 and .name == "1" and (.named | not))
    and any(.[]; .id == $candidate and .name == "PENDING" and .index == 32 and .named)
    and any(.[]; .id == $sentinel and .index == 64 and .name == "64" and (.named | not))
  ' <<< "$settled" > /dev/null; then
    echo "dynamic names stay pending at capacity, then reuse an ordinary empty workspace"
    exit 0
  fi
  sleep 0.05
done

echo "pending name did not claim the ordinary workspace that became empty: $settled"
exit 1
