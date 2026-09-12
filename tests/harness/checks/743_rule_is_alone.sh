#!/usr/bin/env bash
# `match.is_alone` selects rules from the workspace's tiled set, which only a
# running layout produces. A window that opens as the only tiled one is
# configured in its alone state before its first buffer, at the alone width and
# maximized. Both give way when a second window joins, and the width comes back
# when that window goes away.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly STATE_CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

for binary in "$CLIENT" "$STATE_CLIENT"; do
  if [[ ! -x $binary ]]; then
    echo "client not built at $binary"
    exit 1
  fi
done

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[layout.scrolling]
default_width_fraction = 0.5

[[window_rule]]
match.title = "^alone-width$"
match.is_alone = true
default_width = 0.75

[[window_rule]]
match.title = "^alone-max$"
match.is_alone = true
default_maximize = true
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

# The client reports every presentation, so its first line is the size it
# answered the opening configure with, before the window was in the layout.
first_configured_width() {
  sed -n '1s/^mapped \([0-9]\{1,\}\)x.*/\1/p' "$UMBRIEL_RUNTIME_DIR/$1.log"
}

"$CLIENT" alone-width > "$UMBRIEL_RUNTIME_DIR/alone-width.log" 2>&1 &
alone_width_pid=$!
wait_mapped alone-width
alone=$(width_of alone-width)
opening=$(first_configured_width alone-width)
if [[ -z $opening ]]; then
  echo "the client reported no presentation for alone-width"
  exit 1
fi
if ((opening != alone)); then
  echo "the opening configure did not use the alone width: opening=$opening alone=$alone"
  exit 1
fi

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

# An alone rule that maximizes must reach the client the same way: in the
# opening configure, before the first buffer, and the alone effect owns that
# state afterwards rather than leaving the window maximized beside a companion.
# This client reports the state of every configure, and keeps its own buffer
# size, so the configure log is the only place the transition shows.
kill "$alone_width_pid"
wait_gone alone-width

readonly MAX_LOG="$UMBRIEL_RUNTIME_DIR/alone-max.log"
env LOG_CONFIGURES=1 "$STATE_CLIENT" alone-max > "$MAX_LOG" 2>&1 &
wait_mapped alone-max

# Each configure prints its size, then its states, so the lines after the last
# size line are the configure the client is holding.
current_configure() {
  awk '/^configured-size=/ { block = "" } { block = block $0 ORS } END { printf "%s", block }' "$MAX_LOG"
}

maximized_line=$(grep -n '^configured-maximized$' "$MAX_LOG" | sed -n '1s/:.*//p' || true)
mapped_line=$(grep -n '^mapped$' "$MAX_LOG" | sed -n '1s/:.*//p' || true)
if [[ -z $maximized_line || -z $mapped_line || $maximized_line -ge $mapped_line ]]; then
  echo "the alone maximize was not configured before the first buffer:"
  cat "$MAX_LOG"
  exit 1
fi
maximized_width=$(sed -n '1s/^configured-size=\([0-9]\{1,\}\)x.*/\1/p' "$MAX_LOG")

"$STATE_CLIENT" max-neighbor > "$UMBRIEL_RUNTIME_DIR/max-neighbor.log" 2>&1 &
wait_mapped max-neighbor
for _ in $(seq 40); do
  current_configure | grep -q '^configured-maximized$' || break
  sleep 0.1
done
if current_configure | grep -q '^configured-maximized$'; then
  echo "the alone maximize was not undone:"
  cat "$MAX_LOG"
  exit 1
fi
shared_width=$(current_configure | sed -n 's/^configured-size=\([0-9]\{1,\}\)x.*/\1/p')
if ((shared_width >= maximized_width)); then
  echo "the alone maximize never sized the window: maximized=$maximized_width shared=$shared_width"
  exit 1
fi

echo "is_alone configured the width and the maximized state at open: width $alone -> $shared -> $restored, maximized width $maximized_width -> $shared_width"
