#!/usr/bin/env bash
# harness: xwayland=true
# X11 startup and later client requests obey the rule without overriding explicit compositor fullscreen.
set -euo pipefail

readonly CLIENT="$UMBRIEL_XWAYLAND_FOCUS_CLIENT"
readonly BASE="$UMBRIEL_RUNTIME_DIR/x11-rule-base.toml"
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
windowed_fullscreen = $1

[[window_rule]]
match.title = "^x11-rule-real$"
windowed_fullscreen = false

[[window_rule]]
match.title = "^x11-rule-default-real$"
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
query() {
  local command=$1 expected=$2 before
  before=$(wc -l < "$client_log")
  for _ in $(seq 60); do
    printf '%s\n' "$command" >&"$control_fd"
    tail -n +"$((before + 1))" "$client_log" | grep -Fxq "$expected" && return
    sleep 0.05
  done
  echo "$title expected fresh '$expected': $(cat "$client_log")"
  exit 1
}
request() {
  local before pattern='^fullscreen-state-updated$'
  # Xwayland filters a request that repeats its current state, so that case has no property acknowledgement.
  [[ ${2:-changed} == unchanged ]] && pattern="^fullscreen-requested=$1\$"
  before=$(grep -c "$pattern" "$client_log" || true)
  printf 'fullscreen %s\n' "$1" >&"$control_fd"
  for _ in $(seq 60); do
    [[ $(grep -c "$pattern" "$client_log" || true) -gt $before ]] && return
    sleep 0.05
  done
  echo "$title: the compositor did not acknowledge the X11 state request: $(cat "$client_log")"
  exit 1
}
spawn() {
  title=$1
  client_log="$UMBRIEL_RUNTIME_DIR/$title.log"
  local fifo="$UMBRIEL_RUNTIME_DIR/$title.ctl"
  mkfifo "$fifo"
  exec {control_fd}<>"$fifo"
  APP_ID="$2" REQUEST_FULLSCREEN=1 "$CLIENT" "$title" <&"$control_fd" > "$client_log" 2>&1 &
}
policy true
spawn x11-rule-video windowed-rule-player
expect '.windowed_fullscreen and (.fullscreen | not) and .h == 700 and .w < 1280'
query fullscreen fullscreen=true
query geometry "geometry=$(window | jq -r '"\(.x) \(.y) \(.w) \(.h)"')"
request false
expect '(.fullscreen | not) and (.windowed_fullscreen | not)'
query fullscreen fullscreen=false
request true
expect '.windowed_fullscreen and (.fullscreen | not)'

"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
expect '.fullscreen and (.windowed_fullscreen | not)'
request true unchanged
expect '.fullscreen and (.windowed_fullscreen | not)'
query geometry 'geometry=0 0 1280 720'
request false
expect '.fullscreen and (.windowed_fullscreen | not)'
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
expect '(.fullscreen | not) and (.windowed_fullscreen | not)'
request true
expect '.windowed_fullscreen and (.fullscreen | not)'

policy false
expect '.windowed_fullscreen and (.fullscreen | not)'
request false
expect '(.fullscreen | not) and (.windowed_fullscreen | not)'
request true
expect '.fullscreen and (.windowed_fullscreen | not)'
request false
expect '(.fullscreen | not) and (.windowed_fullscreen | not)'
policy true
expect '(.fullscreen | not) and (.windowed_fullscreen | not)'
request true
expect '.windowed_fullscreen and (.fullscreen | not)'

workspace=1
for title in x11-rule-real x11-rule-unmatched x11-rule-default-real; do
  workspace=$((workspace + 1))
  "$UMBRIEL" msg workspace-switch "$workspace" > /dev/null
  app=windowed-rule-player
  [[ $title == x11-rule-unmatched ]] && app=other-player
  spawn "$title" "$app"
  expect '.fullscreen and (.windowed_fullscreen | not)'
  query fullscreen fullscreen=true
  query geometry 'geometry=0 0 1280 720'
done

echo 'X11 windowed fullscreen rules follow initial/later client state, explicit fullscreen, overrides and reloads'
