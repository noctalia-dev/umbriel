#!/usr/bin/env bash
# Client fullscreen requests follow the matching rule at initial configure and after reload; explicit real fullscreen wins.
set -euo pipefail

readonly CLIENT="$UMBRIEL_UNMAP_CLIENT"
readonly BASE="$UMBRIEL_RUNTIME_DIR/rule-base.toml"
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = false
CONFIG
cp "$UMBRIEL_CONFIG" "$BASE"

policy() {
  cat "$BASE" > "$UMBRIEL_CONFIG"
  cat >> "$UMBRIEL_CONFIG" <<CONFIG

[[window_rule]]
match.app_id = "^windowed-rule-player$"
match.is_floating = false
match.is_alone = true
windowed_fullscreen = $1

[[window_rule]]
match.title = "^rule-real$"
windowed_fullscreen = false

[[window_rule]]
match.title = "^rule-default-real$"
default_fullscreen = true
CONFIG
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" settle
}

window() {
  "$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)'
}

expect() {
  local wanted=$1 state
  for _ in $(seq 60); do
    state=$(window)
    if [[ -n $state ]] && jq -e "$wanted" <<< "$state" > /dev/null; then
      "$UMBRIEL" settle
      return
    fi
    sleep 0.05
  done
  echo "$title expected $wanted: $(window); $(cat "$client_log")"
  exit 1
}

configured() {
  local expected=$1 actual
  for _ in $(seq 60); do
    actual=$(grep '^configured-state=' "$client_log" | tail -1 || true)
    [[ $actual == "configured-state=$expected" ]] && return
    sleep 0.05
  done
  echo "$title expected client configure '$expected': $(cat "$client_log")"
  exit 1
}

request() {
  local before
  before=$(grep -c '^surface-committed$' "$client_log" || true)
  # The existing surface-commit command supplies a client round trip after the fullscreen request.
  printf '%ss' "$1" >&"$control_fd"
  for _ in $(seq 60); do
    [[ $(grep -c '^surface-committed$' "$client_log" || true) -gt $before ]] && return
    sleep 0.05
  done
  echo "$title did not complete the request round trip: $(cat "$client_log")"
  exit 1
}

spawn() {
  title=$1
  client_log="$UMBRIEL_RUNTIME_DIR/$title.log"
  local fifo="$UMBRIEL_RUNTIME_DIR/$title.ctl"
  local initial=${3:-REQUEST_FULLSCREEN_BEFORE_MAP}
  mkfifo "$fifo"
  exec {control_fd}<>"$fifo"
  env "$initial=1" APP_ID="$2" FULLSCREEN_ON_STDIN=1 MAXIMIZE_ON_STDIN=1 \
    LOG_CONFIGURES=1 FOLLOW_CONFIGURES=1 "$CLIENT" "$title" <&"$control_fd" > "$client_log" 2>&1 &
}

policy true
spawn rule-video windowed-rule-player
expect '.windowed_fullscreen and (.fullscreen | not) and .h == 700 and .w < 1280'
first=$(grep '^configured-state=' "$client_log" | head -1)
if [[ $first != *x700\ fullscreen || $first == 'configured-state=1280x720 fullscreen' ]]; then
  echo "the initial fullscreen configure did not retain ordinary geometry: $first"
  exit 1
fi
tile=$(window | jq -r '"\(.w)x\(.h)"')
configured "$tile fullscreen"
request u
expect '(.windowed_fullscreen | not) and (.fullscreen | not)'
configured "$tile windowed"
request f
expect '.windowed_fullscreen and (.fullscreen | not)'
configured "$tile fullscreen"

"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
expect '.fullscreen and (.windowed_fullscreen | not)'
request f
expect '.fullscreen and (.windowed_fullscreen | not)'
configured '1280x720 fullscreen'
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
expect '(.fullscreen | not) and (.windowed_fullscreen | not)'
request f
expect '.windowed_fullscreen and (.fullscreen | not)'

policy false
expect '.windowed_fullscreen and (.fullscreen | not)'
request u
expect '(.fullscreen | not) and (.windowed_fullscreen | not)'
request f
expect '.fullscreen and (.windowed_fullscreen | not)'
policy true
expect '.fullscreen and (.windowed_fullscreen | not)'
request u
expect '(.fullscreen | not) and (.windowed_fullscreen | not)'
policy false
policy true
expect '(.fullscreen | not) and (.windowed_fullscreen | not)'
request f
expect '.windowed_fullscreen and (.fullscreen | not)'

# Neither an unmatched app, an explicit false override, nor compositor-default fullscreen is converted at startup.
workspace=1
for title in rule-real rule-unmatched rule-default-real; do
  workspace=$((workspace + 1))
  "$UMBRIEL" msg workspace-switch "$workspace" > /dev/null
  app=windowed-rule-player
  [[ $title == rule-unmatched ]] && app=other-player
  spawn "$title" "$app"
  expect '.fullscreen and (.windowed_fullscreen | not)'
  configured '1280x720 fullscreen'
done

"$UMBRIEL" msg workspace-switch 5 > /dev/null
spawn rule-after-configure windowed-rule-player REQUEST_FULLSCREEN_AFTER_CONFIGURE
expect '.windowed_fullscreen and (.fullscreen | not) and .h == 700 and .w < 1280'
configured "$(window | jq -r '"\(.w)x\(.h)"') fullscreen"
if grep -q '^configured-state=1280x720 fullscreen$' "$client_log"; then
  echo "the initialized, unmapped client received output-sized fullscreen before its rule applied: $(cat "$client_log")"
  exit 1
fi

# Ordinary fullscreen keeps the floating restore size when requested between configure and first map.
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[[window_rule]]
match.title = "^rule-late-float-"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }

[[window_rule]]
match.app_id = "^floating-rule-player$"
windowed_fullscreen = true

[[window_rule]]
match.title = "^rule-late-float-false$"
windowed_fullscreen = false
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
for title in rule-late-float-unmatched rule-late-float-false; do
  app=other-player
  [[ $title == rule-late-float-false ]] && app=floating-rule-player
  spawn "$title" "$app" REQUEST_FULLSCREEN_AFTER_CONFIGURE
  expect '.floating and .fullscreen and (.windowed_fullscreen | not)'
  configured '1280x720 fullscreen'
  first=$(grep '^configured-state=' "$client_log" | head -1)
  if [[ $first != 'configured-state=400x300 windowed' ]]; then
    echo "$title did not receive the floating restore size before fullscreen: $first"
    exit 1
  fi
  request u
  expect '.floating and (.fullscreen | not) and (.windowed_fullscreen | not) and .w == 400 and .h == 300'
  configured '400x300 windowed'
done

echo 'windowed fullscreen rules govern initial and later Wayland requests, explicit fullscreen, overrides and reloads'
