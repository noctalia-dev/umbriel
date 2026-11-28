#!/usr/bin/env bash
# harness: xwayland=true
# X11 clients see the EWMH fullscreen atom at tile size, can leave that state themselves, and reset it on remap.
set -euo pipefail

readonly CLIENT="$UMBRIEL_XWAYLAND_FOCUS_CLIENT"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/windowed-fullscreen-x11.log"
readonly CONTROL="$UMBRIEL_RUNTIME_DIR/windowed-fullscreen-x11-control"
readonly TITLE=windowed-video-x11

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = false
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
mkfifo "$CONTROL"
exec {control_fd}<>"$CONTROL"
"$CLIENT" "$TITLE" <&"$control_fd" > "$CLIENT_LOG" 2>&1 &

window() {
  "$UMBRIEL" windows --json | jq -c --arg title "$TITLE" '.[] | select(.title == $title)'
}

expect_state() {
  local expected=$1 state
  for _ in $(seq 60); do
    state=$(window)
    if [[ -n $state ]] && jq -e "$expected" <<< "$state" > /dev/null; then
      "$UMBRIEL" settle
      return
    fi
    sleep 0.05
  done
  echo "expected $expected, got $(window); client: $(cat "$CLIENT_LOG")"
  exit 1
}

query() {
  local command=$1 expected=$2 before
  before=$(wc -l < "$CLIENT_LOG")
  for _ in $(seq 60); do
    printf '%s\n' "$command" >&"$control_fd"
    if tail -n +"$((before + 1))" "$CLIENT_LOG" | grep -Fxq "$expected"; then
      return
    fi
    sleep 0.05
  done
  echo "expected fresh '$expected': $(cat "$CLIENT_LOG")"
  exit 1
}

expect_state '.fullscreen == false and .windowed_fullscreen == false'
tile=$(window | jq -r '"\(.x) \(.y) \(.w) \(.h)"')
query geometry "geometry=$tile"
"$UMBRIEL" msg window-toggle-windowed-fullscreen > /dev/null
expect_state '.windowed_fullscreen and (.fullscreen | not)'
query fullscreen fullscreen=true
query geometry "geometry=$tile"

# Reasserting the role on activation must retain the client-facing fullscreen atom.
"$UMBRIEL_UNMAP_CLIENT" x11-focus-neighbour > "$UMBRIEL_RUNTIME_DIR/neighbour.log" 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq '[.[] | select(.title == "x11-focus-neighbour" and .active)] | length') == 1 ]] && break
  sleep 0.05
done
if [[ $("$UMBRIEL" windows --json | jq '[.[] | select(.title == "x11-focus-neighbour" and .active)] | length') != 1 ]]; then
  echo "the neighbour did not take focus: $("$UMBRIEL" windows --json)"
  exit 1
fi
"$UMBRIEL" msg window-focus-warp "$(window | jq -r .id)" > /dev/null
"$UMBRIEL" settle
query fullscreen fullscreen=true

printf 'fullscreen false\n' >&"$control_fd"
expect_state '(.windowed_fullscreen | not) and (.fullscreen | not)'
query fullscreen fullscreen=false
printf 'fullscreen true\n' >&"$control_fd"
expect_state '.fullscreen and (.windowed_fullscreen | not)'
query fullscreen fullscreen=true
printf 'fullscreen false\n' >&"$control_fd"
expect_state '(.fullscreen | not) and (.windowed_fullscreen | not)'

"$UMBRIEL" msg window-toggle-windowed-fullscreen > /dev/null
expect_state '.windowed_fullscreen and (.fullscreen | not)'
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
expect_state '.fullscreen and (.windowed_fullscreen | not)'
query fullscreen fullscreen=true
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
expect_state '(.fullscreen | not) and (.windowed_fullscreen | not)'
query fullscreen fullscreen=false

"$UMBRIEL" msg window-toggle-windowed-fullscreen > /dev/null
expect_state '.windowed_fullscreen and (.fullscreen | not)'
printf 'unmap\n' >&"$control_fd"
for _ in $(seq 60); do
  [[ -z $(window) ]] && break
  sleep 0.05
done
if [[ -n $(window) ]]; then
  echo "the X11 fixture did not unmap: $(window)"
  exit 1
fi
printf 'map\n' >&"$control_fd"
expect_state '(.fullscreen | not) and (.windowed_fullscreen | not)'
query fullscreen fullscreen=false

echo 'X11 windowed fullscreen preserves tile geometry, client state, focus reassertion and remap lifecycle'
