#!/usr/bin/env bash
# The overview steps workspaces from a wheel notch, and stops at the ends. While the overview is up the real window
# trees are hidden, so switching is a discrete step down the filmstrip rather than the animated slide it is outside.
# The wheel, the middle-button drag and the three-finger swipe all reach Overview::selectRelativeWorkspace. This check
# exercises the wheel path, while 346_overview_keybind_actions covers arrow input, which navigates cards first. The
# headless backend has no touchpad, and zwlr_virtual_pointer_v1 carries motion, buttons and axes but no gesture
# events, so the gesture state machine is not reachable without a real device. The active workspace is observed
# through ext-workspace-v1.
set -euo pipefail

readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}"

if [[ ! -x $POINTER ]]; then
  echo "pointer client not built at $POINTER"
  exit 1
fi
if [[ ! -x $WORKSPACE ]]; then
  echo "workspace client not built at $WORKSPACE"
  exit 1
fi

# The pointer client needs the output size to normalise absolute coordinates.
pointer() {
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" "$@"
}

# Send one notch and report which workspace it activated, or "none".
notch_activates() {
  local before after
  before=$("$WORKSPACE")
  pointer notch "$1"
  for _ in $(seq 20); do
    after=$("$WORKSPACE")
    [[ $after != "$before" ]] && { echo "$after"; return 0; }
    sleep 0.1
  done
  echo none
}

expect_notch() {
  local dir=$1 want=$2 got
  got=$(notch_activates "$dir")
  if [[ $got != "$want" ]]; then
    echo "notch $dir: expected workspace '$want', got '$got'"
    return 1
  fi
}

# One window, so the group holds workspace 1 (occupied) and a dynamic 2.
foot sh -c 'sleep 120' > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.25
done
if [[ $("$UMBRIEL" windows --json | jq 'length') -ne 1 ]]; then
  echo "timed out waiting for the window to map"
  exit 1
fi

# Park the cursor over the output so the notch resolves to this group.
pointer move $((OUTPUT_W / 2)) $((OUTPUT_H / 2))
"$UMBRIEL" msg overview-open > /dev/null
sleep 0.6

expect_notch 1 2  # down the filmstrip
expect_notch -1 1 # and back up

# At the top row there is nowhere further up: the step is refused rather than wrapping or running off the end of the group. This asserts the behaviour, not the
# bounds check that implements it: deleting that check still passes here, because workspaceAt() then returns null and select(null) is already a no-op.
if [[ $(notch_activates -1) != "none" ]]; then
  echo "a notch past the first workspace was not clamped"
  exit 1
fi

# A successful source change with no overview-invalidating runtime effect is
# equally inert. back_and_forth is read directly when switching workspaces.
printf '\n[workspaces]\nback_and_forth = true\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
expect_notch 1 2
expect_notch -1 1

# Failed reloads keep both the committed config and live overview state. The malformed write clobbers the file, so it
# comes after the valid variant above rather than before it.
printf '[layout\n' > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
expect_notch 1 2
expect_notch -1 1

echo "wheel steps survive failed and irrelevant reloads, and clamp at the top"
