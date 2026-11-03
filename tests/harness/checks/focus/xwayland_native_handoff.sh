#!/usr/bin/env bash
# harness: xwayland=true
# Moving keyboard focus from X11 to native Wayland must release X core input
# focus and the root active window. Moving back must restore both X focus
# signals and key delivery. Re-focusing X is the control for chrome-only
# refreshes that must not clear either signal.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly X_CLIENT="${UMBRIEL_XWAYLAND_FOCUS_CLIENT:-./build-debug/tests/xwayland-focus-client}"
readonly NATIVE_CLIENT="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly KEYBOARD="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly X_LOG="$UMBRIEL_RUNTIME_DIR/xwayland-focus.log"
readonly NATIVE_LOG="$UMBRIEL_RUNTIME_DIR/xwayland-native.log"
readonly X_CONTROL="$UMBRIEL_RUNTIME_DIR/xwayland-focus-control"
readonly X_TITLE_A=xwayland-focus-a
readonly NATIVE_TITLE=xwayland-native

if [[ ! -x $X_CLIENT || ! -x $NATIVE_CLIENT || ! -x $KEYBOARD || -z ${DISPLAY:-} ]]; then
  echo "Xwayland focus helpers or the private DISPLAY are not available"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$X_CONTROL"
exec {x_control_fd}<>"$X_CONTROL"
"$X_CLIENT" "$X_TITLE_A" <&"$x_control_fd" > "$X_LOG" 2>&1 &
"$NATIVE_CLIENT" "$NATIVE_TITLE" > "$NATIVE_LOG" 2>&1 &

wait_for_windows() {
  for _ in $(seq 60); do
    local windows
    windows=$("$UMBRIEL" windows --json)
    if [[ $(jq -r --arg x "$X_TITLE_A" --arg n "$NATIVE_TITLE" \
      '[.[] | select(.title == $x or .title == $n)] | length' <<< "$windows") == 2 ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "the X11 and native focus fixtures did not all map: $("$UMBRIEL" windows --json)"
  echo "X11 log: $(tr '\n' '|' < "$X_LOG")"
  echo "native log: $(tr '\n' '|' < "$NATIVE_LOG")"
  return 1
}

window_id() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | .id'
}

focus_window() {
  local id=$1 title=$2
  "$UMBRIEL" msg window-focus "$id" > /dev/null
  for _ in $(seq 60); do
    if [[ $("$UMBRIEL" windows --json | jq -r --arg id "$id" \
      '.[] | select(.id == $id) | (.focused and .active)') == true ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "$title did not become focused and active: $("$UMBRIEL" windows --json)"
  return 1
}

state_sequence=0
await_x_state() {
  local phase=$1 expected_input=$2 expected_active=$3
  local last="no response"
  for attempt in $(seq 60); do
    state_sequence=$((state_sequence + 1))
    local label="${phase}_${state_sequence}"
    printf 'state %s\n' "$label" >&"$x_control_fd"
    for _ in $(seq 50); do
      last=$(grep "^state label=$label " "$X_LOG" | tail -1 || true)
      [[ -n $last ]] && break
      sleep 0.01
    done
    if [[ $last == "state label=$label input=$expected_input active=$expected_active" ]]; then
      return 0
    fi
    sleep 0.05
  done
  echo "$phase did not reach X input=$expected_input active=$expected_active, last response: $last"
  echo "X11 log: $(tr '\n' '|' < "$X_LOG")"
  return 1
}

await_new_event() {
  local file=$1 pattern=$2 before=$3 label=$4
  for _ in $(seq 60); do
    local count
    count=$(grep -c "$pattern" "$file" || true)
    if ((count > before)); then
      return 0
    fi
    sleep 0.1
  done
  echo "$label did not arrive: $(tr '\n' '|' < "$file")"
  return 1
}

wait_for_windows
readonly X_ID_A=$(window_id "$X_TITLE_A")
readonly NATIVE_ID=$(window_id "$NATIVE_TITLE")

# Establish the X11 side and verify the helper sees real key delivery. The
# second focus call exercises the no-transfer path used by chrome refreshes.
focus_window "$X_ID_A" "$X_TITLE_A"
await_x_state x-focused "$X_TITLE_A" "$X_TITLE_A"
x_keys_before=$(grep -c "^key window=$X_TITLE_A .* state=pressed$" "$X_LOG" || true)
"$KEYBOARD" 1280 720 tap 30 > /dev/null
await_new_event "$X_LOG" "^key window=$X_TITLE_A .* state=pressed$" "$x_keys_before" "the initial X11 key"

focus_window "$X_ID_A" "$X_TITLE_A"
await_x_state x-refreshed "$X_TITLE_A" "$X_TITLE_A"

# The native enter is the transition boundary. After it is delivered, both X
# core input focus and the EWMH active window must be None so the old X11
# application cannot continue handling input intended for the native client.
native_enters_before=$(events "$NATIVE_LOG" keyboard-enter)
focus_window "$NATIVE_ID" "$NATIVE_TITLE"
await_events "$NATIVE_LOG" keyboard-enter "$((native_enters_before + 1))" "$NATIVE_TITLE"
await_x_state native-focused none none

x_keys_before=$(grep -c "^key window=$X_TITLE_A .* state=pressed$" "$X_LOG" || true)
native_keys_before=$(events "$NATIVE_LOG" "keyboard-key code=31 state=pressed")
"$KEYBOARD" 1280 720 tap 31 > /dev/null
await_events "$NATIVE_LOG" "keyboard-key code=31 state=pressed" "$((native_keys_before + 1))" "$NATIVE_TITLE"
x_keys_after=$(grep -c "^key window=$X_TITLE_A .* state=pressed$" "$X_LOG" || true)
if ((x_keys_after != x_keys_before)); then
  echo "the native key also reached the old X11 window: $(tr '\n' '|' < "$X_LOG")"
  exit 1
fi

# Returning to X11 must restore the managed window in both focus channels and
# resume key delivery there.
focus_window "$X_ID_A" "$X_TITLE_A"
await_x_state x-restored "$X_TITLE_A" "$X_TITLE_A"
x_keys_before=$(grep -c "^key window=$X_TITLE_A .* state=pressed$" "$X_LOG" || true)
"$KEYBOARD" 1280 720 tap 32 > /dev/null
await_new_event "$X_LOG" "^key window=$X_TITLE_A .* state=pressed$" "$x_keys_before" "the restored X11 key"

echo "X11 and native keyboard focus hand off by releasing and restoring both X focus channels"
