#!/usr/bin/env bash
# A hidden tab is told it is suspended, so a client that honours it can stop drawing; the tab on show is told it is
# not, and untabbing the column resumes every tab.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

accepts() {
  if ! out=$("$UMBRIEL" msg "$@" 2>&1); then
    echo "expected '$*' to be accepted, got: $out"
    return 1
  fi
}

spawn_client() {
  LOG_SUSPENDED=1 "$CLIENT" "$1" 400 300 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
}

wait_for_windows() {
  local expected=$1 count=
  for _ in $(seq 60); do
    count=$("$UMBRIEL" windows --json | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected window(s), got $count"
  return 1
}

# The last suspended-state line a client printed, or "resumed" before it printed any.
last_state() {
  local state
  state=$(grep -E '^(suspended|resumed)$' "$UMBRIEL_RUNTIME_DIR/$1.log" | tail -n 1) || true
  echo "${state:-resumed}"
}

wait_for_state() {
  local title=$1 expected=$2
  for _ in $(seq 50); do
    [[ $(last_state "$title") == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $title to be $expected, got $(last_state "$title"): $(< "$UMBRIEL_RUNTIME_DIR/$title.log")"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.title = "^tab-"
default_scrolling_column = "tabs"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client tab-a
wait_for_windows 1
spawn_client tab-b
wait_for_windows 2
"$UMBRIEL" settle

# Stacked, both show.
wait_for_state tab-a resumed
wait_for_state tab-b resumed

# tab-b opened last and has focus, so it shows and tab-a is hidden.
accepts column-toggle-tabbed
wait_for_state tab-a suspended
wait_for_state tab-b resumed

accepts column-focus-tab 1
wait_for_state tab-a resumed
wait_for_state tab-b suspended

accepts column-set-display normal
wait_for_state tab-a resumed
wait_for_state tab-b resumed

echo "hidden tabs were suspended and resumed as they hid and showed"
