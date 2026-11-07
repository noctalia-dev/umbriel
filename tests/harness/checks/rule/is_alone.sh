#!/usr/bin/env bash
# `match.is_alone` selects rules from the workspace's tiled columns, which only a
# running layout produces. A window that opens in the only column is configured
# in its alone state before its first buffer, at the alone width and maximized.
# The column can stack another window without ending that state, while a new
# column does. The width comes back when the new column goes away. Fullscreen
# and maximize-to-edges need the window to be the only tile, so a window joining
# the column ends those. A pinned window floats, so it never opens in the alone
# state, whether its rules leave the tiled decision to the client or ask for
# tiling on unpin.
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
default_extent_fraction = 0.5

[[window_rule]]
match.title = "^alone-width$"
match.is_alone = true
default_scrolling_extent = 0.75

[[window_rule]]
match.title = "^alone-max$"
match.is_alone = true
default_maximize = true

[[window_rule]]
match.title = "^pinned-"
match.is_alone = true
default_maximize = true

[[window_rule]]
match.title = "^pinned-"
default_pinned = true

[[window_rule]]
match.title = "^pinned-tiled$"
default_floating = false

[[window_rule]]
match.title = "^column-wide$"
match.is_alone = true
default_scrolling_column = "alone-column"
default_scrolling_extent = 0.75

[[window_rule]]
match.title = "^column-peer$"
default_scrolling_column = "alone-column"

[[window_rule]]
match.title = "^max-column-"
match.is_alone = true
default_scrolling_column = "max-column"
default_maximize = true

[[window_rule]]
match.title = "^column-edge$"
match.is_alone = true
default_scrolling_column = "alone-edge"
default_maximize_to_edges = true

[[window_rule]]
match.title = "^column-edge-peer$"
default_scrolling_column = "alone-edge"

[[window_rule]]
match.title = "^column-edge-peer$"
match.is_alone = true
default_maximize_to_edges = true
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
  "$UMBRIEL" settle
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
alone_max_pid=$!
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
max_neighbor_pid=$!
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

kill "$alone_max_pid" "$max_neighbor_pid"
wait_gone alone-max
wait_gone max-neighbor

for title in pinned-default pinned-tiled; do
  log="$UMBRIEL_RUNTIME_DIR/$title.log"
  env LOG_CONFIGURES=1 "$STATE_CLIENT" "$title" > "$log" 2>&1 &
  pid=$!
  wait_mapped "$title"
  if grep -q '^configured-maximized$' "$log"; then
    echo "the pinned window '$title' opened in the alone state:"
    cat "$log"
    exit 1
  fi
  if [[ $(field_of "$title" floating) != true ]]; then
    echo "the pinned window '$title' did not open floating"
    exit 1
  fi
  "$UMBRIEL" msg window-toggle-pinned > /dev/null
  for _ in $(seq 40); do
    [[ $(field_of "$title" floating) == false ]] && break
    sleep 0.1
  done
  if [[ $(field_of "$title" floating) != false ]]; then
    echo "unpinning '$title' did not return it to tiling"
    exit 1
  fi
  kill "$pid"
  wait_gone "$title"
done

# A second window that joins the same scrolling column keeps is_alone true, so the alone width stays. A new column
# ends it, and the width comes back once that column is gone.
"$CLIENT" column-wide > "$UMBRIEL_RUNTIME_DIR/column-wide.log" 2>&1 &
column_wide_pid=$!
wait_mapped column-wide
column_alone=$(width_of column-wide)

"$CLIENT" column-peer > "$UMBRIEL_RUNTIME_DIR/column-peer.log" 2>&1 &
column_peer_pid=$!
wait_mapped column-peer
column_shared=$(width_of column-wide)
if ((column_shared != column_alone)); then
  echo "a window joining the only column changed the alone width: alone=$column_alone shared=$column_shared"
  exit 1
fi

# The owner leaving the column while the peer stays must take the column-wide effect with it. The peer has no
# is_alone extent of its own, so the column returns to its normal width and stays there.
column_wide_id=$(field_of column-wide id)
"$UMBRIEL" msg window-toggle-floating "$column_wide_id" > /dev/null
for _ in $(seq 40); do
  [[ $(field_of column-wide floating) == true ]] && break
  sleep 0.1
done
if [[ $(field_of column-wide floating) != true ]]; then
  echo "column-wide did not float"
  exit 1
fi
column_released=$(width_of column-peer)
if ((column_released >= column_alone)); then
  echo "the alone width outlived its owner leaving the column: alone=$column_alone released=$column_released"
  exit 1
fi
"$UMBRIEL" msg window-toggle-floating "$column_wide_id" > /dev/null
for _ in $(seq 40); do
  [[ $(field_of column-wide floating) == false ]] && break
  sleep 0.1
done
if [[ $(field_of column-wide floating) != false ]]; then
  echo "column-wide did not return to tiling"
  exit 1
fi
column_widened=$(width_of column-wide)
if ((column_widened != column_alone)); then
  echo "the alone width did not return with the owner: restored=$column_widened alone=$column_alone"
  exit 1
fi

"$CLIENT" column-other > "$UMBRIEL_RUNTIME_DIR/column-other.log" 2>&1 &
column_other_pid=$!
wait_mapped column-other
column_split=$(width_of column-wide)
if ((column_split >= column_alone)); then
  echo "the alone width survived a new column: alone=$column_alone split=$column_split"
  exit 1
fi

kill "$column_other_pid"
wait_gone column-other
column_restored=$(width_of column-wide)
if ((column_restored != column_alone)); then
  echo "the alone width did not come back with the only column: restored=$column_restored alone=$column_alone"
  exit 1
fi

# Expelling the owner splits it out inside the layout, without a detach. The column it leaves must not keep its width.
"$UMBRIEL" msg window-focus "$column_wide_id" > /dev/null
"$UMBRIEL" msg window-consume-or-expel-right > /dev/null
column_expelled=$(width_of column-peer)
if ((column_expelled >= column_alone)); then
  echo "the alone width outlived its owner being expelled: alone=$column_alone peer=$column_expelled"
  exit 1
fi

kill "$column_peer_pid" "$column_wide_pid"
wait_gone column-peer
wait_gone column-wide

# Floating one of two maximized owners must not leave an orphaned maximize on the float; re-tiling restores it.
"$CLIENT" max-column-a > "$UMBRIEL_RUNTIME_DIR/max-column-a.log" 2>&1 &
max_column_a_pid=$!
wait_mapped max-column-a
"$CLIENT" max-column-b > "$UMBRIEL_RUNTIME_DIR/max-column-b.log" 2>&1 &
max_column_b_pid=$!
wait_mapped max-column-b
if [[ $(field_of max-column-a maximized) != true ]]; then
  echo "the alone maximize did not apply in the shared column"
  exit 1
fi
max_column_width=$(width_of max-column-b)

max_column_a_id=$(field_of max-column-a id)
"$UMBRIEL" msg window-toggle-floating "$max_column_a_id" > /dev/null
for _ in $(seq 40); do
  [[ $(field_of max-column-a maximized) == false ]] && break
  sleep 0.1
done
if [[ $(field_of max-column-a maximized) != false ]]; then
  echo "a maximized alone window kept its maximize after floating"
  exit 1
fi
if [[ $(field_of max-column-b maximized) != true ]]; then
  echo "floating one owner undid the peer's maximize"
  exit 1
fi
if [[ $(width_of max-column-b) != "$max_column_width" ]]; then
  echo "floating one owner changed the peer's column width"
  exit 1
fi
max_column_float_width=$(width_of max-column-a)
if ((max_column_float_width >= max_column_width)); then
  echo "the floating alone window kept the column width: float=$max_column_float_width column=$max_column_width"
  exit 1
fi

"$UMBRIEL" msg window-toggle-floating "$max_column_a_id" > /dev/null
for _ in $(seq 40); do
  [[ $(field_of max-column-a maximized) == true ]] && break
  sleep 0.1
done
if [[ $(field_of max-column-a maximized) != true ]]; then
  echo "the shared column's maximize did not return with its window"
  exit 1
fi

kill "$max_column_b_pid" "$max_column_a_pid"
wait_gone max-column-b
wait_gone max-column-a

# Fullscreen and maximize-to-edges still need the only tile. A peer joining the column keeps is_alone true but must
# take the output-covering state away.
"$CLIENT" column-edge > "$UMBRIEL_RUNTIME_DIR/column-edge.log" 2>&1 &
column_edge_pid=$!
wait_mapped column-edge
for _ in $(seq 40); do
  [[ $(field_of column-edge maximized_to_edges) == true ]] && break
  sleep 0.1
done
if [[ $(field_of column-edge maximized_to_edges) != true ]]; then
  echo "the alone maximize-to-edges never applied"
  exit 1
fi

"$CLIENT" column-edge-peer > "$UMBRIEL_RUNTIME_DIR/column-edge-peer.log" 2>&1 &
column_edge_peer_pid=$!
wait_mapped column-edge-peer
if [[ $(field_of column-edge-peer maximized_to_edges) != false ]]; then
  echo "a window that opened into a shared column took maximize-to-edges"
  exit 1
fi
for _ in $(seq 40); do
  [[ $(field_of column-edge maximized_to_edges) == false ]] && break
  sleep 0.1
done
if [[ $(field_of column-edge maximized_to_edges) != false ]]; then
  echo "maximize-to-edges survived a second window in the only column"
  exit 1
fi

kill "$column_edge_peer_pid" "$column_edge_pid"
wait_gone column-edge-peer
wait_gone column-edge

echo "is_alone configured the width and the maximized state at open, never on a pinned window: width $alone -> $shared -> $restored, maximized width $maximized_width -> $shared_width; one column kept $column_alone across a peer and lost it to a new column, and its tile-only maximize-to-edges ended with that peer and never reached the peer's own opening"
