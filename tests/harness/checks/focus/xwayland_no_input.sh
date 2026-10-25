#!/usr/bin/env bash
# harness: xwayland=true
# Match Steam's keyboard hints: Input=False, no WM_TAKE_FOCUS, and UTILITY type. The overlay receives pointer input
# while native keyboard ownership survives mapping, hovering, clicking, explicit focus requests, and replacement.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/xwayland_focus_lib.sh"

readonly NATIVE_TITLE=focus-native-target
readonly NATIVE_LOG="$UMBRIEL_RUNTIME_DIR/focus-native-target.log"
readonly NONE_TITLE=focus-model-none

x_focus_fixture_config
"$NATIVE_CLIENT" "$NATIVE_TITLE" > "$NATIVE_LOG" 2>&1 &
eis_wait_window "$NATIVE_TITLE"
eis_wait_active "$NATIVE_TITLE"
eis_start sender umbriel-no-input-focus
eis_approve sender umbriel-no-input-focus
x_focus_native_key "$NATIVE_TITLE" "$NATIVE_LOG" 31
native_leaves=$(events "$NATIVE_LOG" keyboard-leave)

assert_native_held() {
  if [[ $(events "$NATIVE_LOG" keyboard-leave) -ne $native_leaves ]]; then
    echo "a NoInput window changed keyboard ownership during $1: $(cat "$NATIVE_LOG")"
    exit 1
  fi
}

x_focus_start none "$NONE_TITLE" none
assert_native_held mapping
x_focus_native_key "$NATIVE_TITLE" "$NATIVE_LOG" 32

window=$(eis_window "$NONE_TITLE")
pointer_x=$(jq -r '.x + .w / 2 | floor' <<< "$window")
pointer_y=$(jq -r '.y + .h / 2 | floor' <<< "$window")
"$UMBRIEL_POINTER_CLIENT" 1280 720 move "$pointer_x" "$pointer_y" > /dev/null
"$UMBRIEL" settle > /dev/null
assert_native_held hovering
x_focus_native_key "$NATIVE_TITLE" "$NATIVE_LOG" 33

"$UMBRIEL_POINTER_CLIENT" 1280 720 click 272 > /dev/null
await_events "${X_FOCUS_LOGS[none]}" "button window=$NONE_TITLE code=1 state=pressed" 1 "the NoInput overlay's pointer click"
assert_native_held clicking
x_focus_native_key "$NATIVE_TITLE" "$NATIVE_LOG" 34

"$UMBRIEL" msg "window-focus:$(x_focus_id "$NONE_TITLE")" > /dev/null
assert_native_held "explicit focusing"
x_focus_native_key "$NATIVE_TITLE" "$NATIVE_LOG" 35

# An ordinary X11 window still takes the keyboard. Closing it must choose the native target, skipping NoInput.
x_focus_start passive focus-model-passive passive
eis_wait_active focus-model-passive
eis_send sender 'tap 36'
await_events "${X_FOCUS_LOGS[passive]}" 'key window=focus-model-passive code=44 state=pressed' 1 "the passive X11 keyboard"
x_focus_send passive unmap
x_focus_wait_unmapped focus-model-passive
x_focus_native_key "$NATIVE_TITLE" "$NATIVE_LOG" 37
x_focus_stop passive

# A mapped client can change its input hint. Once it withdraws keyboard input, focus returns to an eligible window.
x_focus_send none 'input true'
await_events "${X_FOCUS_LOGS[none]}" input-requested=true 1 "the overlay allowing keyboard input"
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg "window-focus:$(x_focus_id "$NONE_TITLE")" > /dev/null
eis_wait_active "$NONE_TITLE"
eis_send sender 'tap 38'
await_events "${X_FOCUS_LOGS[none]}" "key window=$NONE_TITLE code=46 state=pressed" 1 "the updated input hint's keyboard"
native_enters=$(events "$NATIVE_LOG" keyboard-enter)
x_focus_send none 'input false'
await_events "$NATIVE_LOG" keyboard-enter "$((native_enters + 1))" "keyboard ownership after the overlay withdraws input"
x_focus_native_key "$NATIVE_TITLE" "$NATIVE_LOG" 39

native_leaves=$(events "$NATIVE_LOG" keyboard-leave)
x_focus_stop none
"$UMBRIEL" settle > /dev/null
assert_native_held "closing the overlay"
x_focus_native_key "$NATIVE_TITLE" "$NATIVE_LOG" 40
eis_stop sender

echo "NoInput X11 overlays retain pointer interaction without replacing the native keyboard target"
