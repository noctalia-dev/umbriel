#!/usr/bin/env bash
# harness: outputs=1
# output-create accepts an optional mode and starts the virtual output at it,
# refresh included, so a streaming setup needs no output-management round trip.
# Without one the output keeps the 1280x720 default, and a malformed mode or a
# stray third argument is refused without leaving an output behind.
set -euo pipefail

current_mode() {
  "$UMBRIEL" outputs --json |
    jq -r --arg name "$1" \
      '.[] | select(.name == $name) | .modes[] | select(.current)
        | "\(.width)x\(.height)@\(.refresh_mhz)"'
}

wait_for_mode() {
  local name=$1 expected=$2 actual=
  for _ in $(seq 80); do
    actual=$(current_mode "$name")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$name' mode '$expected', got '$actual'"
  return 1
}

created=$("$UMBRIEL" output-create stream 400x300@50)
if [[ $created != stream ]]; then
  echo "output-create printed '$created', expected 'stream'"
  exit 1
fi
wait_for_mode stream 400x300@50000

"$UMBRIEL" output-create plain 640x360 > /dev/null
wait_for_mode plain 640x360@0

"$UMBRIEL" output-create default > /dev/null
wait_for_mode default 1280x720@0

if "$UMBRIEL" output-create broken 1920x1080p 2> /dev/null; then
  echo "output-create accepted a malformed mode"
  exit 1
fi

if "$UMBRIEL" output-create crowded 1920x1080 extra 2> /dev/null; then
  echo "output-create accepted a third argument"
  exit 1
fi

if "$UMBRIEL" outputs --json | jq -e '.[] | select(.name == "broken" or .name == "crowded")' > /dev/null; then
  echo "a rejected output-create left an output behind"
  exit 1
fi
