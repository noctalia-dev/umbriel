#!/usr/bin/env bash
# Real clients exercise master insertion, geometry, consume, and directional focus through compositor seams.
set -euo pipefail

spawn_client() {
  foot --title="harness-master-$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_windows() {
  local want=$1
  for _ in $(seq 40); do
    if [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]]; then
      return 0
    fi
    sleep 0.25
  done
  echo "timed out waiting for $want window(s), saw: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_query() {
  local query=$1
  local message=$2
  for _ in $(seq 40); do
    if "$UMBRIEL" windows --json | jq -e "$query" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "$message: $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "master"

[animation]
duration_ms = 1
EOF
"$UMBRIEL" msg config-reload > /dev/null

# The 1280x720 headless output uses edgePad 10 and totalGap 12. Content is 1260x700. With the default 0.55 master
# fraction, the divisible width is 1248: master round(0.55 * 1248) = 686, stack = 562. Two rows share
# 700 - 12 = 688 pixels, so each is 344 pixels high.
spawn_client a
wait_for_windows 1
wait_for_query \
  '.[0] | .title == "harness-master-a" and .x == 10 and .y == 10 and .w == 1260 and .h == 700' \
  "first master window did not fill the content area"

spawn_client b
wait_for_windows 2
wait_for_query \
  '([.[].w] | sort) == [562,686] and all(.[]; .h == 700) and any(.[]; .title == "harness-master-a" and .x == 10 and .w == 686) and any(.[]; .title == "harness-master-b" and .x == 708 and .w == 562)' \
  "second window did not create the expected stack"

spawn_client c
wait_for_windows 3
wait_for_query \
  'any(.[]; .title == "harness-master-a" and .x == 10 and .w == 686 and .h == 700) and any(.[]; .title == "harness-master-c" and .x == 708 and .y == 10 and .w == 562 and .h == 344) and any(.[]; .title == "harness-master-b" and .x == 708 and .y == 366 and .w == 562 and .h == 344)' \
  "newest window did not join the top of the stack"

"$UMBRIEL" msg window-consume-left > /dev/null
wait_for_query \
  '([.[] | select(.w == 686 and .h == 344)] | length) == 2 and any(.[]; .title == "harness-master-b" and .x == 708 and .w == 562 and .h == 700)' \
  "consume-left did not move the focused stack window into master"

"$UMBRIEL" msg window-focus-right > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-b" and .focused == true)' \
  "focus-right did not cross from master to stack"

"$UMBRIEL" msg window-focus-left > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-a" and .focused == true)' \
  "focus-left did not cross from stack to master"

"$UMBRIEL" msg master-count-decrease > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-a" and .x == 10 and .y == 10 and .w == 686 and .h == 700) and any(.[]; .title == "harness-master-c" and .x == 708 and .y == 10 and .w == 562 and .h == 344) and any(.[]; .title == "harness-master-b" and .x == 708 and .y == 366 and .w == 562 and .h == 344)' \
  "master-count-decrease did not demote the last master window to the stack top"

"$UMBRIEL" msg master-count-increase > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-a" and .x == 10 and .y == 10 and .w == 686 and .h == 344) and any(.[]; .title == "harness-master-c" and .x == 10 and .y == 366 and .w == 686 and .h == 344) and any(.[]; .title == "harness-master-b" and .x == 708 and .y == 10 and .w == 562 and .h == 700)' \
  "master-count-increase did not promote the stack top to the master bottom"

"$UMBRIEL" msg window-focus-next > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-c" and .focused == true)' \
  "window-focus-next did not focus c after a"
"$UMBRIEL" msg window-focus-next > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-b" and .focused == true)' \
  "window-focus-next did not focus b after c"
"$UMBRIEL" msg window-focus-next > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-a" and .focused == true)' \
  "window-focus-next did not wrap from b to a"
"$UMBRIEL" msg window-focus-previous > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-b" and .focused == true)' \
  "window-focus-previous did not wrap from a to b"
"$UMBRIEL" msg window-focus-next > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-a" and .focused == true)' \
  "window-focus-next did not return focus from b to a"

"$UMBRIEL" msg window-swap-next > /dev/null
wait_for_query \
  'any(.[]; .title == "harness-master-c" and .x == 10 and .y == 10 and .w == 686 and .h == 344) and any(.[]; .title == "harness-master-a" and .x == 10 and .y == 366 and .w == 686 and .h == 344 and .focused == true) and any(.[]; .title == "harness-master-b" and .x == 708 and .y == 10 and .w == 562 and .h == 700)' \
  "window-swap-next did not exchange master rows while retaining focus"

"$UMBRIEL" msg window-modify-width:0.05 > /dev/null
wait_for_query \
  'all(.[] | select(.title == "harness-master-a" or .title == "harness-master-c"); .w == 749) and any(.[]; .title == "harness-master-b" and .w == 499)' \
  "window-modify-width did not widen the focused master area by five percent"

"$UMBRIEL" msg window-cycle-width > /dev/null
wait_for_query \
  'all(.[] | select(.title == "harness-master-a" or .title == "harness-master-c"); .w == 832) and any(.[]; .title == "harness-master-b" and .w == 416)' \
  "window-cycle-width did not advance the master area to two thirds"

"$UMBRIEL" msg window-cycle-width-back > /dev/null
wait_for_query \
  'all(.[] | select(.title == "harness-master-a" or .title == "harness-master-c"); .w == 624) and any(.[]; .title == "harness-master-b" and .w == 624)' \
  "window-cycle-width-back did not return the master area to one half"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout.master]
new_on_top = false
EOF
"$UMBRIEL" msg config-reload > /dev/null
spawn_client d
wait_for_windows 4
wait_for_query \
  'any(.[]; .title == "harness-master-b" and .x == 646 and .y == 10 and .w == 624 and .h == 344) and any(.[]; .title == "harness-master-d" and .x == 646 and .y == 366 and .w == 624 and .h == 344)' \
  "new_on_top false did not place the newest window at the bottom of the stack"

echo "master count, focus, swap, and width controls work in layout order"
