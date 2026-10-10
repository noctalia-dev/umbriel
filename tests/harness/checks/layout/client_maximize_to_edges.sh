#!/usr/bin/env bash
# Client-requested maximize and unmaximize around [layout] maximize_to_edges: a client's maximize
# fills the column by default and the usable-area edges when configured, while an unmaximize
# request exits whichever mode is active instead of following the configured policy.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly COLUMN_LOG="$UMBRIEL_RUNTIME_DIR/client-maximize-column.log"
readonly RESTORED_LOG="$UMBRIEL_RUNTIME_DIR/client-maximize-restored.log"
readonly FIRST_LOG="$UMBRIEL_RUNTIME_DIR/client-maximize-restored-first.log"

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

wait_for_field() {
  local title=$1 field=$2 expected=$3 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$title" "$field")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' field '$field' to be '$expected', got '$actual'"
  return 1
}

# The request must come post-opening to pass the gates as client intent.
request_maximize() {
  local title=$1 log=$2
  # RESIZE_FILL_COLOR makes the client follow configured sizes; same value keeps the fill unchanged.
  env LOG_CONFIGURES=1 MAXIMIZE_ON_STDIN=1 RESIZE_FILL_COLOR=0xFF5577AA \
    "$CLIENT" "$title" 640 480 <&"$control_fd" > "$log" 2>&1 &
  CLIENT_PID=$!
  await_lines "$log" mapped 1
  # Queue both bytes up front so the client consumes them back-to-back once its poll loop wakes.
  local committed requested
  committed=$(events "$log" surface-committed)
  requested=$(events "$log" maximize-requested)
  printf s >&"$control_fd"
  printf m >&"$control_fd"
  await_lines "$log" surface-committed $((committed + 1))
  await_lines "$log" maximize-requested $((requested + 1))
}

# Sends one control byte to the live client and waits for a fresh acknowledgement line.
send_command() {
  local byte=$1 log=$2 pattern=$3
  local seen
  seen=$(events "$log" "$pattern")
  printf '%s' "$byte" >&"$control_fd"
  await_lines "$log" "$pattern" $((seen + 1))
}

stop_client() {
  kill -KILL "$CLIENT_PID" 2>/dev/null || true
  wait "$CLIENT_PID" 2>/dev/null || true
}

readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/client-maximize-control"
mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"

# honor_restored_maximize accepts a client's opening maximize re-assert (restored-session clients);
# the scrolling extent keeps the tile width distinct from both maximized widths. The key lands in
# the boot config's [general], the file's last table.
cat >> "$UMBRIEL_CONFIG" <<'EOF'
honor_restored_maximize = true

[animation]
enabled = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[layout.scrolling]
default_extent_fraction = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

# Policy off: the client's maximize fills the column (1264x704 usable), not the edges.
request_maximize client-maximize-column "$COLUMN_LOG"
wait_for_field client-maximize-column w 1264
wait_for_field client-maximize-column h 704
wait_for_field client-maximize-column x 8
wait_for_field client-maximize-column y 8

# Policy enabled under the still column-maximized client: the unmaximize request must restore the
# tile instead of routing into the edges mode's no-op exit.
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
maximize_to_edges = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

send_command M "$COLUMN_LOG" 'unmaximize-requested$'
wait_for_field client-maximize-column w 628
wait_for_field client-maximize-column h 704

# A compositor-driven column maximize is undone by the client's unmaximize request too.
"$UMBRIEL" msg window-toggle-maximize > /dev/null
wait_for_field client-maximize-column w 1264
send_command M "$COLUMN_LOG" 'unmaximize-requested$'
wait_for_field client-maximize-column w 628

# With the policy on, the client's own maximize targets the edges.
send_command m "$COLUMN_LOG" 'maximize-requested$'
# IPC x/y report the layout slot, whose cross-axis inset the edges override only fixes at presentation.
wait_for_field client-maximize-column w 1280
wait_for_field client-maximize-column h 720
wait_for_field client-maximize-column x 0

# Leaving the edges with a tile-width column restores the tile in one step.
send_command M "$COLUMN_LOG" 'unmaximize-requested$'
wait_for_field client-maximize-column w 628

# Column-maximize underneath the edges stacks both states; the unmaximize leaves the edges first
# and the full-width column on the next request.
"$UMBRIEL" msg window-toggle-maximize > /dev/null
wait_for_field client-maximize-column w 1264
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
wait_for_field client-maximize-column w 1280
send_command M "$COLUMN_LOG" 'unmaximize-requested$'
wait_for_field client-maximize-column w 1264
send_command M "$COLUMN_LOG" 'unmaximize-requested$'
wait_for_field client-maximize-column w 628
stop_client

# A client that re-asserts maximize while opening (restored session) is honored and lands in the
# edges mode under the policy; its unmaximize request restores the tile in one step, and a fresh
# post-map request re-enters the edges.
env LOG_CONFIGURES=1 MAXIMIZE_ON_STDIN=1 REQUEST_MAXIMIZED_AFTER_MAP=1 RESIZE_FILL_COLOR=0xFF5577AA \
  "$CLIENT" client-maximize-restored 640 480 <&"$control_fd" > "$RESTORED_LOG" 2>&1 &
CLIENT_PID=$!
await_lines "$RESTORED_LOG" mapped 1
wait_for_field client-maximize-restored w 1280
wait_for_field client-maximize-restored h 720
send_command M "$RESTORED_LOG" 'unmaximize-requested$'
wait_for_field client-maximize-restored w 628
send_command m "$RESTORED_LOG" 'maximize-requested$'
wait_for_field client-maximize-restored w 1280
stop_client

# A client that restores maximize before its first commit reaches the same state: the opening configure already carries
# the edge size, so it opens maximized to the edges and one unmaximize restores the tile.
env LOG_CONFIGURES=1 MAXIMIZE_ON_STDIN=1 REQUEST_MAXIMIZED=1 RESIZE_FILL_COLOR=0xFF5577AA \
  "$CLIENT" client-maximize-restored-first 640 480 <&"$control_fd" > "$FIRST_LOG" 2>&1 &
CLIENT_PID=$!
await_lines "$FIRST_LOG" mapped 1
wait_for_field client-maximize-restored-first w 1280
wait_for_field client-maximize-restored-first h 720
send_command M "$FIRST_LOG" 'unmaximize-requested$'
wait_for_field client-maximize-restored-first w 628
stop_client

echo "client maximize targets column width by default (1264x704), edges when configured (1280x720) for post-map and restored requests and unmaximize restores the tile (628x704)"
