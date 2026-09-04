#!/usr/bin/env bash
set -euo pipefail

readonly BTN_RIGHT=273
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/pointer-client}"

cat >> "$UMBRIEL_CONFIG" <<'EOF'
[layout.scrolling]
default_width_fraction = 0.25
EOF
"$UMBRIEL" msg config-reload > /dev/null

pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

window() {
  "$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)'
}

wait_for_window() {
  local title=$1
  for _ in $(seq 60); do
    [[ -n $(window "$title") ]] && return 0
    sleep 0.1
  done
  echo "timed out waiting for $title"
  return 1
}

foot --title=resize-min-a sh -c 'sleep 120' > /dev/null 2>&1 &
wait_for_window resize-min-a
foot --title=resize-min-b sh -c 'sleep 120' > /dev/null 2>&1 &
wait_for_window resize-min-b

a=$(window resize-min-a)
pointer move "$(jq -r '.x + .w - 20 | floor' <<< "$a")" "$(jq -r '.y + .h / 2 | floor' <<< "$a")" \
  mod logo press "$BTN_RIGHT" move 200 360 release "$BTN_RIGHT" mod none

for _ in $(seq 60); do
  a=$(window resize-min-a)
  [[ $(jq -r '.w' <<< "$a") -eq 189 ]] && break
  sleep 0.1
done
if [[ $(jq -r '.w' <<< "$a") -ne 189 ]]; then
  echo "left window did not reach its 189px resize minimum: $a"
  exit 1
fi

b_before=$(window resize-min-b)
pointer move "$(jq -r '.x + 20 | floor' <<< "$b_before")" "$(jq -r '.y + .h / 2 | floor' <<< "$b_before")" \
  mod logo press "$BTN_RIGHT" move "$(jq -r '.x - 80 | floor' <<< "$b_before")" 360 release "$BTN_RIGHT" mod none

for _ in $(seq 60); do
  a=$(window resize-min-a)
  b_after=$(window resize-min-b)
  [[ $(jq -r '.w' <<< "$a") -eq 189 \
    && $(jq -r '.x' <<< "$b_after") -lt $(jq -r '.x' <<< "$b_before") \
    && $(jq -r '.x + .w' <<< "$b_after") -eq $(jq -r '.x + .w' <<< "$b_before") ]] && break
  sleep 0.1
done

if [[ $(jq -r '.w' <<< "$a") -ne 189 \
  || $(jq -r '.x' <<< "$b_after") -ge $(jq -r '.x' <<< "$b_before") \
  || $(jq -r '.x + .w' <<< "$b_after") -ne $(jq -r '.x + .w' <<< "$b_before") ]]; then
  echo "leftward resize did not grow B after A reached minimum: before=$b_before after=$b_after windows=$($UMBRIEL windows --json)"
  exit 1
fi

echo "leftward Mod+Right-drag grows B after A reaches its minimum width"
