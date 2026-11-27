#!/usr/bin/env bash
# A [[scratchpad]] with sticky = true stays up and keeps focus when its output switches or swipes to another workspace,
# and a non-modal one also stays up when focusing a window on another workspace switches there. A pad without it still
# closes on a switch.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly ROAM=sticky-roam
readonly ANCHORED=sticky-anchored
readonly PLAIN=sticky-plain
readonly OTHER=sticky-other

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[[scratchpad]]
name = "roam"
sticky = true

[[scratchpad]]
name = "anchored"
EOF
"$UMBRIEL" msg config-reload > /dev/null

windows() { "$UMBRIEL" windows --json; }

field_of() {
  windows | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

wait_for_field() {
  local title=$1 field=$2 expected=$3 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$title" "$field")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.05
  done
  echo "expected '$title' field '$field' to be '$expected', got '$actual': $(windows)"
  exit 1
}

# `$1` describes the change; `$2` is whether the workspace window should now be visible, proving the switch happened.
expect_shown() {
  "$UMBRIEL" settle
  [[ $(field_of "$PLAIN" visible) == "$2" ]] || {
    echo "$1 did not switch workspace: $(windows)"
    exit 1
  }
  [[ $(field_of "$ROAM" visible) == true && $(field_of "$ROAM" active) == true ]] || {
    echo "$1 closed a sticky scratchpad or took its focus: $(windows)"
    exit 1
  }
}

"$CLIENT" "$PLAIN" 300 200 > "$UMBRIEL_RUNTIME_DIR/$PLAIN.log" 2>&1 &
wait_for_field "$PLAIN" active true
"$UMBRIEL" msg workspace-switch 2 > /dev/null
"$CLIENT" "$OTHER" 300 200 > "$UMBRIEL_RUNTIME_DIR/$OTHER.log" 2>&1 &
wait_for_field "$OTHER" active true
"$UMBRIEL" msg workspace-switch 1 > /dev/null
wait_for_field "$PLAIN" active true
for pad in roam anchored; do
  title=sticky-$pad
  "$CLIENT" "$title" 300 200 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  wait_for_field "$title" active true
  "$UMBRIEL" msg window-move-to-scratchpad "$pad" > /dev/null
  wait_for_field "$title" scratchpad "$pad"
done

"$UMBRIEL" msg scratchpad-toggle roam > /dev/null
wait_for_field "$ROAM" visible true
wait_for_field "$ROAM" active true

"$UMBRIEL" msg workspace-switch 2 > /dev/null
expect_shown "a workspace switch" false

"$UMBRIEL" swipe-test begin 1000 > /dev/null
"$UMBRIEL" swipe-test update 1100 0 240 > /dev/null
"$UMBRIEL" swipe-test end 1600 0 > /dev/null
expect_shown "a workspace swipe" true

sed -i '/^name = "roam"$/a modal = false' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg window-focus "$(field_of "$OTHER" id)" > /dev/null
wait_for_field "$OTHER" active true
"$UMBRIEL" settle
[[ $(field_of "$PLAIN" visible) == false && $(field_of "$ROAM" visible) == true ]] || {
  echo "focusing a window on another workspace did not switch there or closed a sticky scratchpad: $(windows)"
  exit 1
}
"$UMBRIEL" msg workspace-switch 1 > /dev/null

# A pad without sticky still closes on a switch.
"$UMBRIEL" msg scratchpad-toggle roam > /dev/null
wait_for_field "$ROAM" visible false
"$UMBRIEL" msg scratchpad-toggle anchored > /dev/null
wait_for_field "$ANCHORED" visible true
"$UMBRIEL" msg workspace-switch 2 > /dev/null
wait_for_field "$PLAIN" visible false
wait_for_field "$ANCHORED" visible false

echo "a sticky scratchpad stayed up through workspace switches and swipes, and sticky = false kept its pad closing"
