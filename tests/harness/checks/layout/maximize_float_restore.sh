#!/usr/bin/env bash
# Maximize, float, and unfloat keeps the tile maximized until the maximize binding restores its original tile size.
# Repeated round trips must also preserve the separate floating restore box.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
duration_ms = 120

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[[window_rule]]
match.title = "^maximize-float-(remembered|floating)-"
default_floating = true
default_floating_size_px = { width = 420, height = 270 }
default_position = { x = 160, y = 120, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

field() {
  "$UMBRIEL" windows --json | jq -r --arg title "$TITLE" --arg key "$1" \
    '[.[] | select(.title == $title)] | if length == 0 then "<absent>" else .[0][$key] end'
}

wait_field() {
  local actual=
  for _ in $(seq 80); do
    actual=$(field "$1")
    [[ $actual == "$2" ]] && return 0
    sleep 0.05
  done
  echo "$TITLE: expected $1=$2, got $actual: $("$UMBRIEL" windows --json)"
  return 1
}

for mode in column edges remembered-column remembered-edges; do
  if [[ $mode == remembered-column ]]; then
    sed -i '/^\[animation\]$/ { n; s/^enabled = false$/enabled = true/; }' "$UMBRIEL_CONFIG"
    "$UMBRIEL" msg config-reload > /dev/null
  fi
  TITLE="maximize-float-$mode"
  "$CLIENT" "$TITLE" > "$UMBRIEL_RUNTIME_DIR/$TITLE.log" 2>&1 &
  client_pid=$!
  wait_field title "$TITLE"
  window_id=$(field id)
  "$UMBRIEL" msg window-focus "$window_id" > /dev/null
  float_action=(window-toggle-floating)
  if [[ $mode == remembered-* ]]; then
    float_action=(window-toggle-floating "$window_id")
  fi
  if [[ $mode == remembered-* ]]; then
    wait_field floating true
    wait_field w 420
    wait_field h 270
    wait_field x 160
    wait_field y 120
    "$UMBRIEL" msg "${float_action[@]}" > /dev/null
  fi
  wait_field floating false
  "$UMBRIEL" settle > /dev/null
  tile_width=$(field w)
  tile_height=$(field h)
  restore_width=$tile_width
  restore_height=$tile_height
  if [[ $mode == remembered-* ]]; then
    restore_width=420
    restore_height=270
  fi
  action=window-toggle-maximize
  edges=false
  if [[ $mode == *edges ]]; then
    action=window-toggle-maximize-to-edges
    edges=true
  fi

  "$UMBRIEL" msg "$action" > /dev/null
  wait_field maximized true
  "$UMBRIEL" settle > /dev/null
  max_width=$(field w)
  max_height=$(field h)
  if ((max_width <= tile_width)); then
    echo "$TITLE: maximize did not enlarge the tile ($tile_width to $max_width)"
    exit 1
  fi

  "$UMBRIEL" msg "${float_action[@]}" > /dev/null
  wait_field floating true
  wait_field maximized true
  wait_field maximized_to_edges "$edges"
  wait_field w "$max_width"

  # Unfloating must not restore the normal size automatically. Only the following maximize binding should do that.
  "$UMBRIEL" msg "${float_action[@]}" > /dev/null
  wait_field floating false
  wait_field maximized true
  wait_field maximized_to_edges "$edges"
  "$UMBRIEL" settle > /dev/null
  wait_field w "$max_width"
  wait_field h "$max_height"

  # Keep the same restore extent through another full float/unfloat cycle while still maximized.
  "$UMBRIEL" msg "${float_action[@]}" > /dev/null
  wait_field floating true
  wait_field maximized true
  "$UMBRIEL" msg "${float_action[@]}" > /dev/null
  wait_field floating false
  wait_field maximized true
  "$UMBRIEL" settle > /dev/null
  wait_field w "$max_width"
  wait_field h "$max_height"

  "$UMBRIEL" msg "$action" > /dev/null
  wait_field maximized false
  wait_field w "$tile_width"
  wait_field h "$tile_height"

  # The maximized floating geometry must not overwrite the remembered normal floating size or position.
  "$UMBRIEL" msg "${float_action[@]}" > /dev/null
  wait_field floating true
  wait_field maximized false
  wait_field w "$restore_width"
  wait_field h "$restore_height"
  if [[ $mode == remembered-* ]]; then
    wait_field x 160
    wait_field y 120
  fi

  # A second cycle must capture the restored size, not a stale maximized configure.
  "$UMBRIEL" msg "$action" > /dev/null
  wait_field maximized true
  wait_field w "$max_width"
  "$UMBRIEL" msg "$action" > /dev/null
  wait_field maximized false
  wait_field w "$restore_width"
  wait_field h "$restore_height"

  "$UMBRIEL" msg "${float_action[@]}" > /dev/null
  wait_field floating false
  wait_field w "$tile_width"
  wait_field h "$tile_height"
  kill -TERM "$client_pid"
  wait "$client_pid" 2>/dev/null || true
  wait_field title "<absent>"
done

for mode in floating-column floating-edges; do
  TITLE="maximize-float-$mode"
  "$CLIENT" "$TITLE" > "$UMBRIEL_RUNTIME_DIR/$TITLE.log" 2>&1 &
  client_pid=$!
  wait_field floating true
  wait_field w 420
  wait_field h 270
  "$UMBRIEL" msg window-focus "$(field id)" > /dev/null
  action=window-toggle-maximize
  max_width=1264
  if [[ $mode == *edges ]]; then
    action=window-toggle-maximize-to-edges
    max_width=1280
  fi
  "$UMBRIEL" msg "$action" > /dev/null
  wait_field maximized true
  "$UMBRIEL" settle > /dev/null
  wait_field w "$max_width"
  "$UMBRIEL" msg window-toggle-floating > /dev/null
  wait_field floating false
  wait_field maximized true
  wait_field w "$max_width"
  "$UMBRIEL" msg "$action" > /dev/null
  wait_field maximized false
  wait_field w 420
  kill -TERM "$client_pid"
  wait "$client_pid" 2>/dev/null || true
  wait_field title "<absent>"
done

echo "maximize, float, and unfloat preserved maximization until the binding restored the original tile extent"
