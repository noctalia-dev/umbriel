#!/usr/bin/env bash
# A text-input-v3 client that sets surrounding text and content type in a commit
# after the one enabling the text input, as Chromium does, must still have both
# relayed to the input method.
set -euo pipefail

readonly INPUT_METHOD="${UMBRIEL_INPUT_METHOD_CLIENT:-./build-debug/tests/input-method-client}"
readonly OBSERVER="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
readonly TEXT_LOG="$UMBRIEL_RUNTIME_DIR/text-input-window.log"
readonly INPUT_METHOD_LOG="$UMBRIEL_RUNTIME_DIR/input-method.log"
# zwp_text_input_v3 content_hint.spellcheck and content_purpose.url
readonly HINT_SPELLCHECK=2
readonly PURPOSE_URL=5

wait_for_log() {
  local file=$1 pattern=$2
  for _ in $(seq 80); do
    grep -q "$pattern" "$file" 2>/dev/null && return 0
    sleep 0.05
  done
  echo "expected '$pattern' in $file: $(tr '\n' '|' < "$file" 2>/dev/null || true)"
  return 1
}

"$INPUT_METHOD" > "$INPUT_METHOD_LOG" 2>&1 &
wait_for_log "$INPUT_METHOD_LOG" '^grabbed$'
ENABLE_TEXT_INPUT=1 TEXT_INPUT_LATE_STATE=1 "$OBSERVER" text-input-window > "$TEXT_LOG" 2>&1 &
wait_for_log "$TEXT_LOG" '^text-input-enter$'

wait_for_log "$INPUT_METHOD_LOG" "^surrounding-text text='late text' cursor=9 anchor=9$"
wait_for_log "$INPUT_METHOD_LOG" "^content-type hint=$HINT_SPELLCHECK purpose=$PURPOSE_URL$"

echo "surrounding text and content type set after text-input enable reach the input method"
