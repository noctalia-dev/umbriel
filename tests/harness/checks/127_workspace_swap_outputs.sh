#!/usr/bin/env bash
# harness: outputs=2
# Swapping the active workspaces of two outputs exchanges their windows and
# nothing else: every column width, every row split inside a stacked column, and
# each strip's scroll offset arrive on the other output unchanged, which the
# translation of each window box by the output offset observes exactly. Swapping
# back has to land on the original boxes, so an asymmetric transfer fails here
# even when the first swap looked right.
set -euo pipefail

readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}"
readonly OFFSET=1280

accepts() {
  if ! out=$("$UMBRIEL" msg "$1" 2>&1); then
    echo "expected '$1' to be accepted, got: $out"
    exit 1
  fi
}

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" \
    '.[] | select(.title == $title) | .[$field]'
}

wait_for_windows() {
  local expected=$1 count=
  for _ in $(seq 40); do
    count=$("$UMBRIEL" windows --json | jq 'length')
    if [[ $count == "$expected" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $expected window(s), got $count"
  exit 1
}

wait_for_workspace() {
  local title=$1 expected=$2 actual=
  for _ in $(seq 50); do
    actual=$(field_of "$title" workspace)
    if [[ $actual == "$expected" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected '$title' on $expected, got $actual"
  exit 1
}

box_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" \
    '.[] | select(.title == $title) | "\(.workspace) \(.x) \(.y) \(.w) \(.h)"'
}

# Asserts that `title` sits on `workspace` with its box translated by `dx` from `expected`.
assert_box() {
  local title=$1 workspace=$2 dx=$3 expected=$4 actual=
  actual=$(box_of "$title")
  read -r _ x y w h <<< "$expected"
  local want="$workspace $((x + dx)) $y $w $h"
  if [[ $actual != "$want" ]]; then
    echo "$title: expected '$want', got '$actual'"
    exit 1
  fi
}

workspace_id_named() {
  "$WORKSPACE" --all | awk -F'\t' -v name="$1" '$2 == name { print $1; exit }'
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout.scrolling]
default_width_fraction = 0.5

[animation]
enabled = false

[output.HEADLESS-1]
position = [0, 0]
workspaces = ["LEFT"]

[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["RIGHT"]
EOF
"$UMBRIEL" msg config-reload > /dev/null

left=$(workspace_id_named LEFT)
right=$(workspace_id_named RIGHT)
if [[ -z $left || -z $right ]]; then
  echo "named workspaces missing: left='$left' right='$right'"
  exit 1
fi
accepts "workspace-switch:LEFT/HEADLESS-1"

spawn_client stack-top
wait_for_windows 1
spawn_client stack-bottom
wait_for_windows 2
# One column holding an uneven two-row stack, plus a wider second column. Both
# survive only if the transfer replays widths and row weights, not defaults.
accepts "window-focus:$(field_of stack-bottom id)"
accepts window-consume-or-expel-left
accepts "window-modify-height:-0.2"
spawn_client wide
wait_for_windows 3
accepts "window-modify-width:0.15"

accepts "workspace-switch:RIGHT/HEADLESS-2"
spawn_client lone
wait_for_windows 4
accepts "workspace-switch:LEFT/HEADLESS-1"
sleep 0.4

top_before=$(box_of stack-top)
bottom_before=$(box_of stack-bottom)
wide_before=$(box_of wide)
lone_before=$(box_of lone)
if [[ ${top_before#* } == ${bottom_before#* } ]]; then
  echo "the stack rows were not split unevenly: '$top_before' '$bottom_before'"
  exit 1
fi

accepts workspace-swap-active-output-next
wait_for_workspace stack-top "$right"
sleep 0.4
assert_box stack-top "$right" "$OFFSET" "$top_before"
assert_box stack-bottom "$right" "$OFFSET" "$bottom_before"
assert_box wide "$right" "$OFFSET" "$wide_before"
assert_box lone "$left" "-$OFFSET" "$lone_before"

# The directional action reaches the same pair from the other side.
accepts "workspace-switch:RIGHT/HEADLESS-2"
accepts workspace-swap-active-output-left
wait_for_workspace stack-top "$left"
sleep 0.4
assert_box stack-top "$left" 0 "$top_before"
assert_box stack-bottom "$left" 0 "$bottom_before"
assert_box wide "$left" 0 "$wide_before"
assert_box lone "$right" 0 "$lone_before"

# Dwindle keeps a split tree rather than columns, so its ratios only survive if
# the transfer replays the layout's own state instead of rebuilding columns.
accepts "workspace-set-layout:dwindle"
accepts "workspace-switch:LEFT/HEADLESS-1"
accepts "workspace-set-layout:dwindle"
accepts "window-focus:$(field_of wide id)"
accepts "window-modify-width:0.15"
sleep 0.4
top_dwindle=$(box_of stack-top)
bottom_dwindle=$(box_of stack-bottom)
wide_dwindle=$(box_of wide)
lone_dwindle=$(box_of lone)

accepts workspace-swap-active-output-next
wait_for_workspace stack-top "$right"
sleep 0.4
assert_box stack-top "$right" "$OFFSET" "$top_dwindle"
assert_box stack-bottom "$right" "$OFFSET" "$bottom_dwindle"
assert_box wide "$right" "$OFFSET" "$wide_dwindle"
assert_box lone "$left" "-$OFFSET" "$lone_dwindle"

echo "active workspace swap exchanged both outputs' windows with their scrolling widths, row splits, scroll offset and dwindle split ratios intact"
