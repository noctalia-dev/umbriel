#!/usr/bin/env bash
# Named scrolling-column rules stack real clients in configured order, not launch order.
set -euo pipefail

failed=0

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly DELAYED_LOG="$UMBRIEL_RUNTIME_DIR/named-delayed.log"
readonly FIRST_LOG="$UMBRIEL_RUNTIME_DIR/named-first.log"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/named-column-control"

spawn_client() {
  "$CLIENT" "$1" 800 600 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
}

wait_for_windows() {
  local count=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $count ]] && return 0
    sleep 0.1
  done
  echo "timed out waiting for $count named-column clients: $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout.scrolling]
default_extent_fraction = 0.5

[[window_rule]]
match.title = "^named-later$"
default_scrolling_column = "browser-stack"
default_scrolling_column_order = 20
default_scrolling_extent = 0.6

[[window_rule]]
match.title = "^named-first$"
default_scrolling_column = "browser-stack"
default_floating_size_px = { width = 300, height = 300 }
default_scrolling_column_order = 10
default_scrolling_extent = 0.25

[[window_rule]]
match.title = "^named-delayed$"
default_scrolling_column = "browser-stack"
default_scrolling_column_order = 15
default_scrolling_extent = 0.4

[[window_rule]]
match.title = "^named-max-order$"
default_scrolling_column = "browser-stack"
default_scrolling_column_order = 2147483647

[[window_rule]]
match.title = "^named-unordered$"
default_scrolling_column = "browser-stack"

[[window_rule]]
match.title = "^named-floating-joiner$"
default_floating = true
default_floating_size_px = { width = 300, height = 200 }
default_scrolling_column = "browser-stack"
default_scrolling_column_order = 12
default_scrolling_extent = 0.25
EOF
"$UMBRIEL" msg config-reload > /dev/null

# Start the lower window first. The rule order must correct the final stack.
spawn_client named-later
wait_for_windows 1
spawn_client unrelated
wait_for_windows 2
LOG_CONFIGURES=1 "$CLIENT" named-first 800 600 > "$FIRST_LOG" 2>&1 &
wait_for_windows 3

for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  later_x=$(jq -r '.[] | select(.title == "named-later") | .x' <<< "$windows")
  first_x=$(jq -r '.[] | select(.title == "named-first") | .x' <<< "$windows")
  [[ -n $first_x && $first_x == "$later_x" ]] && break
  sleep 0.1
done

sleep 0.2
first_configure=$(awk -F= '/^configured-size=/{print $2; exit}' "$FIRST_LOG")
first_arranged=$(awk -F= '/^configured-size=/{size=$2} END {print size}' "$FIRST_LOG")
if [[ $first_configure != "$first_arranged" ]]; then
  echo "named client first configure $first_configure did not match joined size $first_arranged"
  failed=1
fi

# INT_MAX is still an explicit order and must sort before an unordered member.
spawn_client named-unordered
wait_for_windows 4
spawn_client named-max-order
wait_for_windows 5

# Fill the strip and focus its end. The placeholder will initially open there,
# then its real title must move focus back to the visible named column.
spawn_client filler-a
wait_for_windows 6
spawn_client filler-b
wait_for_windows 7
spawn_client filler-c
wait_for_windows 8

# Some clients publish their useful title after mapping. The late rule must
# move the window into the named column without changing that column's width.
mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
TITLE_AFTER_MAP=named-delayed "$CLIENT" named-placeholder 800 600 <&"$control_fd" > "$DELAYED_LOG" 2>&1 &
wait_for_windows 9
printf 'u' >&"$control_fd"

for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  later_x=$(jq -r '.[] | select(.title == "named-later") | .x' <<< "$windows")
  later_y=$(jq -r '.[] | select(.title == "named-later") | .y' <<< "$windows")
  first_x=$(jq -r '.[] | select(.title == "named-first") | .x' <<< "$windows")
  first_y=$(jq -r '.[] | select(.title == "named-first") | .y' <<< "$windows")
  delayed_x=$(jq -r '.[] | select(.title == "named-delayed") | .x' <<< "$windows")
  delayed_y=$(jq -r '.[] | select(.title == "named-delayed") | .y' <<< "$windows")
  max_order_y=$(jq -r '.[] | select(.title == "named-max-order") | .y' <<< "$windows")
  unordered_y=$(jq -r '.[] | select(.title == "named-unordered") | .y' <<< "$windows")
  unrelated_x=$(jq -r '.[] | select(.title == "unrelated") | .x' <<< "$windows")
  if [[ -n $delayed_x && $first_x == "$delayed_x" && $delayed_x == "$later_x" && $unrelated_x != "$first_x" ]] \
      && ((first_y < delayed_y && delayed_y < later_y)) \
      && ((later_y < max_order_y && max_order_y < unordered_y)) \
      && ((delayed_x >= 0)) \
      && ((unrelated_x - first_x > 640)); then
    break
  fi
  sleep 0.1
done

if [[ $first_x != "$delayed_x" || $delayed_x != "$later_x" || $unrelated_x == "$first_x" ]]; then
  echo "named clients did not share one column separate from the unrelated client: $windows"
  failed=1
fi
if ((first_y >= delayed_y || delayed_y >= later_y)); then
  echo "default_scrolling_column_order did not place named clients in configured order: $windows"
  failed=1
fi
if ((later_y >= max_order_y || max_order_y >= unordered_y)); then
  echo "explicit maximum order did not precede the unordered client: $windows"
  failed=1
fi
if ((delayed_x < 0)); then
  echo "late named-column relocation left the focused client outside the viewport: $windows"
  failed=1
fi
if ((unrelated_x - first_x <= 640)); then
  echo "a joining client changed the named column extent: $windows"
  failed=1
fi

# A floating member may carry a future scrolling extent, but joining an established named column must not apply it to
# the whole column.
column_gap_before=$((unrelated_x - first_x))
spawn_client named-floating-joiner
wait_for_windows 10
"$UMBRIEL" msg window-toggle-floating > /dev/null
for _ in $(seq 60); do
  windows=$("$UMBRIEL" windows --json)
  first_x=$(jq -r '.[] | select(.title == "named-first") | .x' <<< "$windows")
  joiner_x=$(jq -r '.[] | select(.title == "named-floating-joiner") | .x' <<< "$windows")
  unrelated_x=$(jq -r '.[] | select(.title == "unrelated") | .x' <<< "$windows")
  joiner_floating=$(jq -r '.[] | select(.title == "named-floating-joiner") | .floating' <<< "$windows")
  [[ $joiner_floating == false && $joiner_x == "$first_x" ]] && break
  sleep 0.1
done
if [[ $joiner_floating != false || $joiner_x != "$first_x" ]]; then
  echo "floating named member did not join its established column: $windows"
  failed=1
elif ((unrelated_x - first_x != column_gap_before)); then
  echo "floating named member changed the established column extent: before=$column_gap_before after=$((unrelated_x - first_x))"
  failed=1
fi

((failed == 0)) || exit 1
echo "named members preserve ordering and established column extents"
