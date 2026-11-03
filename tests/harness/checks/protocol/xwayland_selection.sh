#!/usr/bin/env bash
# harness: xwayland=true
# Eager X11 readers must recover native selections on focus return without a
# paste-time retry. Background reads remain denied and X11 owners remain intact.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly X_CLIENT="${UMBRIEL_XWAYLAND_SELECTION_CLIENT:-./build-debug/tests/xwayland-selection-client}"
readonly NATIVE_CLIENT="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly X_LOG="$UMBRIEL_RUNTIME_DIR/xwayland-selection.log"
readonly NATIVE_LOG="$UMBRIEL_RUNTIME_DIR/selection-native.log"
readonly X_CONTROL="$UMBRIEL_RUNTIME_DIR/xwayland-selection-control"
readonly X_TITLE=xwayland-selection
readonly NATIVE_TITLE=selection-native

if [[ ! -x $X_CLIENT || ! -x $NATIVE_CLIENT || -z ${DISPLAY:-} ]]; then
  echo "Xwayland selection helpers or the private DISPLAY are not available"
  exit 1
fi
for command in wl-copy wl-paste; do
  if ! command -v "$command" > /dev/null; then
    echo "$command is required for the Xwayland selection check"
    exit 1
  fi
done

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

wait_for_window() {
  local title=$1
  for _ in $(seq 80); do
    if [[ $("$UMBRIEL" windows --json | jq -r --arg title "$title" \
      '[.[] | select(.title == $title)] | length') == 1 ]]; then
      return 0
    fi
    sleep 0.02
  done
  echo "$title did not map: $("$UMBRIEL" windows --json)"
  return 1
}

window_id() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | .id'
}

focus_window() {
  local id=$1
  "$UMBRIEL" msg window-focus "$id" > /dev/null
  for _ in $(seq 60); do
    if [[ $("$UMBRIEL" windows --json | jq -r --arg id "$id" \
      '.[] | select(.id == $id) | (.focused and .active)') == true ]]; then
      return 0
    fi
    sleep 0.02
  done
  echo "$id did not become focused and active: $("$UMBRIEL" windows --json)"
  return 1
}

paste_selection() {
  local name=$1
  local -a args=(--no-newline)
  [[ $name == primary ]] && args+=(--primary)
  timeout 1 wl-paste "${args[@]}" 2>/dev/null
}

await_paste() {
  local name=$1 expected=$2 actual=
  for _ in $(seq 20); do
    if actual=$(paste_selection "$name") && [[ $actual == "$expected" ]]; then
      return 0
    fi
    sleep 0.02
  done
  echo "$name did not contain '$expected', last native read: '$actual'"
  return 1
}

await_empty_selection() {
  local name=$1 actual= status=0
  for _ in $(seq 20); do
    if actual=$(paste_selection "$name"); then
      status=0
    else
      status=$?
    fi
    # A timed-out transfer is not evidence of an empty selection.
    if ((status != 0 && status != 124)) && [[ -z $actual ]]; then
      return 0
    fi
    sleep 0.02
  done
  echo "$name still has a selection after clear: '$actual' (status $status)"
  return 1
}

source_sequence=0
declare -A native_source_pid=()
start_native_source() {
  local name=$1 payload=$2
  local -a args=(--foreground --type 'text/plain;charset=utf-8')
  [[ $name == primary ]] && args+=(--primary)
  source_sequence=$((source_sequence + 1))
  local file="$UMBRIEL_RUNTIME_DIR/selection-source-$source_sequence"
  printf '%s' "$payload" > "$file"
  wl-copy "${args[@]}" < "$file" > "$file.log" 2>&1 &
  native_source_pid[$name]=$!
}

publish_native_pair() {
  local clipboard=$1 primary=$2
  start_native_source clipboard "$clipboard"
  start_native_source primary "$primary"
  await_paste clipboard "$clipboard"
  await_paste primary "$primary"
  for name in clipboard primary; do
    if ! kill -0 "${native_source_pid[$name]}" 2>/dev/null; then
      echo "the foreground $name source exited while it still owned the selection"
      return 1
    fi
  done
}

cache_sequence=0
await_cache() {
  local phase=$1 clipboard=$2 primary=$3
  cache_sequence=$((cache_sequence + 1))
  local label="${phase}_${cache_sequence}"
  local expected="cache label=$label clipboard=$clipboard primary=$primary"
  for _ in $(seq 80); do
    # state only drains queued notifications; it must never request a selection.
    printf 'state %s\n' "$label" >&"$x_control_fd"
    if grep -Fxq "$expected" "$X_LOG"; then
      return 0
    fi
    sleep 0.02
  done
  echo "$phase did not reach '$expected': $(tr '\n' '|' < "$X_LOG")"
  return 1
}

clipboard_denied=0
primary_denied=0
mark_denied() {
  clipboard_denied=$(events "$X_LOG" 'selection name=clipboard denied$')
  primary_denied=$(events "$X_LOG" 'selection name=primary denied$')
}

await_denied_pair() {
  await_lines "$X_LOG" 'selection name=clipboard denied$' "$((clipboard_denied + 1))" 0.02
  await_lines "$X_LOG" 'selection name=primary denied$' "$((primary_denied + 1))" 0.02
  await_cache background-denied '<none>' '<none>'
}

read_background_pair() {
  mark_denied
  printf 'read clipboard\nread primary\n' >&"$x_control_fd"
  await_denied_pair
}

await_fullscreen() {
  local expected=$1
  for _ in $(seq 60); do
    if [[ $("$UMBRIEL" tearing --json | jq -r --arg title "$X_TITLE" \
      '.surfaces[] | select(.title == $title) | .fullscreen') == "$expected" ]]; then
      return 0
    fi
    sleep 0.02
  done
  echo "X11 fullscreen did not become $expected: $("$UMBRIEL" tearing --json)"
  return 1
}

# Copy before the first X11 consumer exists. Keep both foreground providers
# alive so initial eager TARGETS and UTF8_STRING transfers have real sources.
"$NATIVE_CLIENT" "$NATIVE_TITLE" > "$NATIVE_LOG" 2>&1 &
wait_for_window "$NATIVE_TITLE"
readonly NATIVE_ID=$(window_id "$NATIVE_TITLE")
focus_window "$NATIVE_ID"
publish_native_pair FIRST PRIMARY_FIRST

mkfifo "$X_CONTROL"
exec {x_control_fd}<>"$X_CONTROL"
"$X_CLIENT" "$X_TITLE" <&"$x_control_fd" > "$X_LOG" 2>&1 &
wait_for_window "$X_TITLE"
readonly X_ID=$(window_id "$X_TITLE")
focus_window "$X_ID"
# Establish the startup read after mapping; later returns must recover only through notifications.
printf 'read clipboard\nread primary\n' >&"$x_control_fd"
await_cache initial FIRST PRIMARY_FIRST

# Copies announce ownership while X11 lacks focus. Its eager reads must fail,
# including explicit background attempts, before normal focus return repairs it.
focus_window "$NATIVE_ID"
mark_denied
publish_native_pair SECOND PRIMARY_SECOND
await_denied_pair
read_background_pair
focus_window "$X_ID"
await_cache first-return SECOND PRIMARY_SECOND

focus_window "$NATIVE_ID"
mark_denied
publish_native_pair THIRD PRIMARY_THIRD
await_denied_pair
focus_window "$X_ID"
await_cache repeated-copy THIRD PRIMARY_THIRD

# Clear the consumer cache with denied reads, then return without another copy.
# Only an ownership notification, not a manual read, may recover the payload.
focus_window "$NATIVE_ID"
read_background_pair
focus_window "$X_ID"
await_cache no-new-copy THIRD PRIMARY_THIRD

printf 'fullscreen true\n' >&"$x_control_fd"
await_fullscreen true
focus_window "$NATIVE_ID"
mark_denied
publish_native_pair FULLSCREEN PRIMARY_FULLSCREEN
await_denied_pair
focus_window "$X_ID"
await_cache fullscreen-return FULLSCREEN PRIMARY_FULLSCREEN
await_fullscreen true
printf 'fullscreen false\n' >&"$x_control_fd"
await_fullscreen false

# X11-origin sources must still serve Wayland readers across both transitions.
# Exact content, not compositor log wording, proves ownership was preserved.
printf 'own clipboard X11_CLIPBOARD\nown primary X11_PRIMARY\n' >&"$x_control_fd"
await_lines "$X_LOG" 'own name=clipboard value=X11_CLIPBOARD$' 1 0.02
await_lines "$X_LOG" 'own name=primary value=X11_PRIMARY$' 1 0.02
await_paste clipboard X11_CLIPBOARD
await_paste primary X11_PRIMARY
await_cache x11-owned X11_CLIPBOARD X11_PRIMARY
focus_window "$NATIVE_ID"
await_paste clipboard X11_CLIPBOARD
await_paste primary X11_PRIMARY
focus_window "$X_ID"
await_paste clipboard X11_CLIPBOARD
await_paste primary X11_PRIMARY
await_cache x11-preserved X11_CLIPBOARD X11_PRIMARY

# Replace X11 ownership with native sources, then clear while native is focused.
# Returning to X11 must not resurrect either the old native or X11 payloads.
focus_window "$NATIVE_ID"
mark_denied
publish_native_pair CLEAR_STALE PRIMARY_CLEAR_STALE
await_denied_pair
clipboard_none=$(events "$X_LOG" 'selection name=clipboard none$')
primary_none=$(events "$X_LOG" 'selection name=primary none$')
wl-copy --clear
wl-copy --primary --clear
await_empty_selection clipboard
await_empty_selection primary
await_lines "$X_LOG" 'selection name=clipboard none$' "$((clipboard_none + 1))" 0.02
await_lines "$X_LOG" 'selection name=primary none$' "$((primary_none + 1))" 0.02
focus_window "$X_ID"
await_cache cleared-return '<none>' '<none>'
await_empty_selection clipboard
await_empty_selection primary

echo "eager X11 selection caches recover on focus return, including fullscreen, while background denial, X11 ownership and clears are preserved"
