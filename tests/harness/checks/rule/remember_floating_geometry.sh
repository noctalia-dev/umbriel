#!/usr/bin/env bash
# harness: outputs=1
# remember_floating_geometry reopens a floating window at the size and position its app ID last closed at; without
# the key, a reopened window falls back to its size and position rules.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

cat >>"$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false

[[window_rule]]
match.app_id = '^remember-(on|off)$'
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
default_position = { x = 20, y = 20, anchor = "top_left" }

[[window_rule]]
match.app_id = '^remember-on$'
remember_floating_geometry = true
CONFIG
"$UMBRIEL" msg config-reload >/dev/null

CLIENT_PID=
open_client() {
  local app_id=$1
  env APP_ID="$app_id" RESIZE_FILL_COLOR=0xFF5577AA \
    "$UMBRIEL_UNMAP_CLIENT" "$app_id" 200 200 >>"$UMBRIEL_RUNTIME_DIR/$app_id.log" 2>&1 &
  CLIENT_PID=$!
  await_window "$app_id"
}

close_client() {
  kill "$CLIENT_PID"
  wait "$CLIENT_PID" 2>/dev/null || true
  await_window_closed "$1"
}

box() { window_by_title "$1" | jq -r '"\(.w)x\(.h)+\(.x)+\(.y)"'; }

expect_box() {
  local title=$1 expected=$2 context=$3 actual=
  "$UMBRIEL" settle >/dev/null
  for _ in $(seq 80); do
    actual=$(box "$title")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.025
  done
  echo "$context: expected $title at $expected, got $actual"
  return 1
}

# Resize and move away from the rule geometry, then close and reopen.
reshape_and_reopen() {
  local app_id=$1
  open_client "$app_id"
  expect_box "$app_id" 400x300+20+20 "$app_id first open"
  "$UMBRIEL" msg window-set-primary-extent 0.5 >/dev/null
  "$UMBRIEL" msg window-set-secondary-extent 0.5 >/dev/null
  "$UMBRIEL" msg window-move-to top_left 100 80 >/dev/null
  expect_box "$app_id" 640x360+100+80 "$app_id after resize and move"
  close_client "$app_id"
  open_client "$app_id"
}

reshape_and_reopen remember-on
expect_box remember-on 640x360+100+80 "remember-on reopen"
close_client remember-on

reshape_and_reopen remember-off
expect_box remember-off 400x300+20+20 "remember-off reopen"
close_client remember-off
