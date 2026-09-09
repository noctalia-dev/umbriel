#!/usr/bin/env bash
# `match.is_alone` selects rules from the workspace's tiled set, which only a
# running layout produces. The alone width applies while the window is the only
# tiled one, gives way to the configured default when a second window joins, and
# comes back when that window goes away.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"

if [[ ! -x $CLIENT ]]; then
  echo "fractional client not built at $CLIENT"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[[window_rule]]
match.title = "^alone-width$"
match.is_alone = true
default_width = 0.75
EOF
"$UMBRIEL" msg config-reload > /dev/null

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg t "$1" --arg f "$2" '.[] | select(.title == $t) | .[$f]'
}

wait_mapped() {
  local title=$1
  for _ in $(seq 80); do
    [[ -n $(field_of "$title" w) ]] && return 0
    sleep 0.1
  done
  echo "window '$title' never mapped"
  exit 1
}

wait_gone() {
  local title=$1
  for _ in $(seq 80); do
    [[ -z $(field_of "$title" w) ]] && return 0
    sleep 0.1
  done
  echo "window '$title' never closed"
  exit 1
}

width_of() {
  sleep 0.3
  field_of "$1" w
}

"$CLIENT" alone-width > "$UMBRIEL_RUNTIME_DIR/alone-width.log" 2>&1 &
wait_mapped alone-width
alone=$(width_of alone-width)

"$CLIENT" neighbor > "$UMBRIEL_RUNTIME_DIR/neighbor.log" 2>&1 &
neighbor_pid=$!
wait_mapped neighbor
shared=$(width_of alone-width)
neighbor=$(width_of neighbor)

if ((alone <= shared)); then
  echo "the alone width never applied: alone=$alone shared=$shared"
  exit 1
fi
if ((shared != neighbor)); then
  echo "the alone width was not undone: shared=$shared neighbor=$neighbor"
  exit 1
fi

kill "$neighbor_pid"
wait_gone neighbor
restored=$(width_of alone-width)
if ((restored != alone)); then
  echo "the alone width did not come back: restored=$restored alone=$alone"
  exit 1
fi

echo "is_alone width applied, gave way to the default, and came back: $alone -> $shared -> $restored"
