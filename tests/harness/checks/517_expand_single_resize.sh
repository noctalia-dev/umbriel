#!/usr/bin/env bash
set -euo pipefail

readonly BTN_RIGHT=273
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

cat >> "$UMBRIEL_CONFIG" <<'EOF'
[animation]
enabled = false
[layout.scrolling]
expand_single_column = true
center_underfull_strip = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

foot --title=expand-single-resize sh -c 'sleep 120' > /dev/null 2>&1 &
for _ in $(seq 60); do
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "expand-single-resize")')
  [[ -n $window ]] && break
  sleep 0.1
done
if [[ -z ${window:-} ]]; then
  echo "timed out waiting for expand-single-resize"
  exit 1
fi

before=$window
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" \
  move "$(jq -r '.x + .w - 20 | floor' <<< "$before")" "$(jq -r '.y + .h / 2 | floor' <<< "$before")" \
  mod logo press "$BTN_RIGHT" move 1000 360 release "$BTN_RIGHT" mod none

for _ in $(seq 60); do
  after=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "expand-single-resize")')
  [[ $(jq -r '.w' <<< "$after") -lt $(jq -r '.w' <<< "$before") ]] && break
  sleep 0.1
done

if [[ $(jq -r '.w' <<< "$after") -ge $(jq -r '.w' <<< "$before") ]]; then
  echo "right-edge drag did not shrink the expanded lone column: before=$before after=$after"
  exit 1
fi

echo "right-edge drag resized an automatically expanded lone column: before=$(jq -r '.w' <<< "$before") after=$(jq -r '.w' <<< "$after")"
