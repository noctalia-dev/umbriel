#!/usr/bin/env bash
# harness: outputs=2
# Overview shortcut selection follows the focused window only when configured, across and within outputs. Other close
# paths leave the cursor where the user put it, and keyboard input after a shortcut reaches the selected window.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly BTN_LEFT=272
readonly WIDTH=2560
readonly HEIGHT=720
readonly OVERVIEW_EVENTS="$UMBRIEL_RUNTIME_DIR/shortcut-overview-events.log"

pointer() { "$POINTER" "$WIDTH" "$HEIGHT" "$@"; }

count_windows() { "$UMBRIEL" windows --json | jq 'length'; }
focused_title() {
  "$UMBRIEL" windows --json | jq -r '[.[] | select(.active) | .title] | if length == 1 then .[0] else "none" end'
}

wait_for_count() {
  for _ in $(seq 80); do
    [[ $(count_windows) -eq $1 ]] && return 0
    sleep 0.05
  done
  echo "expected $1 windows: $($UMBRIEL windows --json)"
  return 1
}

wait_for_focus() {
  for _ in $(seq 80); do
    [[ $(focused_title) == "$1" ]] && return 0
    sleep 0.05
  done
  echo "expected focus on '$1': $($UMBRIEL windows --json)"
  return 1
}

overview_closes() {
  jq -s '[.[] | select(.event == "overview" and .data.open == false)] | length' "$OVERVIEW_EVENTS" 2>/dev/null || echo 0
}

wait_for_overview_close() {
  local before=$1
  for _ in $(seq 40); do
    [[ $(overview_closes) -gt $before ]] && return 0
    sleep 0.05
  done
  echo "overview did not close: $(< "$OVERVIEW_EVENTS")"
  return 1
}

center_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | "\(.x + (.w / 2 | floor)) \(.y + (.h / 2 | floor))"'
}

inset_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | "\(.x + 100) \(.y + 100)"'
}

origin_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | "\(.x) \(.y)"'
}

pointer_enters() { grep -c '^pointer-enter ' "$1" 2>/dev/null || true; }

wait_for_pointer_enter() {
  local log=$1 expected=$2 before=$3
  for _ in $(seq 40); do
    [[ $(pointer_enters "$log") -gt $before ]] && grep -Fq "pointer-enter $expected" "$log" && return 0
    sleep 0.05
  done
  echo "expected new pointer enter '$expected' in $log: $(< "$log")"
  return 1
}

overview_center_of() {
  local geometry output output_x center_x center_y
  geometry=$("$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title == $title) | "\(.output) \(.x + (.w / 2)) \(.y + (.h / 2))"')
  read -r output center_x center_y <<< "$geometry"
  output_x=$("$UMBRIEL" outputs | awk -v name="$output" '$1 == name {found = 1; next} found && /Position:/ {split($2, p, ","); print p[1]; exit}')
  awk -v ox="$output_x" -v cx="$center_x" -v cy="$center_y" 'BEGIN {printf "%d %d\n", ox + 640 + (cx - ox - 640) * 0.5, 360 + (cy - 360) * 0.5}'
}

focus_at_pointer() {
  local title=$1 point
  point=$(center_of "$title")
  read -r x y <<< "$point"
  pointer move "$x" "$y"
  "$UMBRIEL" settle
}

append_config() {
  cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation.overview]
duration_ms = 100
curve = "linear"

[overview]
shortcut_keys = "1234567890"

[input.cursor]
follows_focus = true

[input.focus]
follows_mouse = true

[[window_rule]]
match.app_id = "^warp-decoy$"
default_output = "HEADLESS-1"

[[window_rule]]
match.app_id = "^warp-cross$"
default_output = "HEADLESS-2"

[[window_rule]]
match.app_id = "^warp-same-a$"
default_output = "HEADLESS-2"

[[window_rule]]
match.app_id = "^warp-same-b$"
default_output = "HEADLESS-2"

[[window_rule]]
match.title = "^transient-parent$"
default_output = "HEADLESS-2"
EOF
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" settle
}

append_config
"$UMBRIEL" subscribe overview > "$OVERVIEW_EVENTS" &
for _ in $(seq 40); do
  [[ -f $OVERVIEW_EVENTS ]] && break
  sleep 0.05
done
UMBRIEL_LOG_POINTER=1 APP_ID=warp-decoy "$CLIENT" warp-decoy 900 600 > "$UMBRIEL_RUNTIME_DIR/warp-decoy.log" 2>&1 &
UMBRIEL_LOG_POINTER=1 APP_ID=warp-cross "$CLIENT" warp-cross 900 600 > "$UMBRIEL_RUNTIME_DIR/warp-cross.log" 2>&1 &
wait_for_count 2
focus_at_pointer warp-decoy

# The pointer is on output 1, so the second badge selects the output 2 window. The closing animation must finish before
# the warp, then a click without motion proves the pointer and seat both arrived at the selected client.
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
pointer tap 3
wait_for_focus warp-cross
"$UMBRIEL" settle
wait_for_pointer_enter "$UMBRIEL_RUNTIME_DIR/warp-cross.log" 'x=450.000 y=350.000' 0
pointer tap 30
for _ in $(seq 40); do
  grep -q '^key 30 1$' "$UMBRIEL_RUNTIME_DIR/warp-cross.log" 2>/dev/null && break
  sleep 0.05
done
grep -q '^key 30 1$' "$UMBRIEL_RUNTIME_DIR/warp-cross.log" || {
  echo "post-shortcut key did not reach the cross-output selection: $(< "$UMBRIEL_RUNTIME_DIR/warp-cross.log")"
  exit 1
}
# Keep the no-motion click as a separate pointer-position check after keyboard delivery.
pointer click "$BTN_LEFT"
wait_for_focus warp-cross

# With follows_focus disabled, the same selection restores keyboard focus but leaves the pointer on output 1.
sed -i 's/follows_focus = true/follows_focus = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
focus_at_pointer warp-decoy
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
cross_enters_before=$(pointer_enters "$UMBRIEL_RUNTIME_DIR/warp-cross.log")
pointer tap 3
wait_for_focus warp-cross
"$UMBRIEL" settle
pointer click "$BTN_LEFT"
wait_for_focus warp-decoy
if [[ $(pointer_enters "$UMBRIEL_RUNTIME_DIR/warp-cross.log") -ne $cross_enters_before ]]; then
  echo "follows_focus=false moved the pointer into warp-cross: $(< "$UMBRIEL_RUNTIME_DIR/warp-cross.log")"
  exit 1
fi

# Escape closes without selecting or warping, even when follows_focus is enabled.
sed -i 's/follows_focus = false/follows_focus = true/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
point=$(inset_of warp-decoy)
read -r x y <<< "$point"
pointer move "$x" "$y"
"$UMBRIEL" settle
overview_closes_before=$(overview_closes)
decoy_enters_before=$(pointer_enters "$UMBRIEL_RUNTIME_DIR/warp-decoy.log")
cross_enters_before=$(pointer_enters "$UMBRIEL_RUNTIME_DIR/warp-cross.log")
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
pointer tap 1
wait_for_overview_close "$overview_closes_before"
"$UMBRIEL" settle
wait_for_pointer_enter "$UMBRIEL_RUNTIME_DIR/warp-decoy.log" 'x=100.000 y=100.000' "$decoy_enters_before"
if [[ $(pointer_enters "$UMBRIEL_RUNTIME_DIR/warp-cross.log") -ne $cross_enters_before ]]; then
  echo "Escape moved the cursor to warp-cross: $(< "$UMBRIEL_RUNTIME_DIR/warp-cross.log")"
  exit 1
fi

# Mouse selection uses the pointer's card position and keeps its existing focus behavior.
UMBRIEL_LOG_POINTER=1 APP_ID=warp-same-a "$CLIENT" warp-same-a 900 600 > "$UMBRIEL_RUNTIME_DIR/warp-same-a.log" 2>&1 &
wait_for_count 3
UMBRIEL_LOG_POINTER=1 APP_ID=warp-same-b "$CLIENT" warp-same-b 900 600 > "$UMBRIEL_RUNTIME_DIR/warp-same-b.log" 2>&1 &
wait_for_count 4
"$UMBRIEL" settle
focus_at_pointer warp-same-a
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
pointer tap 5
wait_for_focus warp-same-b
"$UMBRIEL" settle
pointer click "$BTN_LEFT"
wait_for_focus warp-same-b

# A click on the output 2 preview selects its card and closes the overview without entering the shortcut close path.
"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
point=$(overview_center_of warp-same-b)
read -r x y <<< "$point"
origin=$(origin_of warp-same-b)
read -r origin_x origin_y <<< "$origin"
expected_local="x=$((x - origin_x)).000 y=$((y - origin_y)).000"
mouse_enters_before=$(pointer_enters "$UMBRIEL_RUNTIME_DIR/warp-same-b.log")
pointer move "$x" "$y" click "$BTN_LEFT"
wait_for_focus warp-same-b
"$UMBRIEL" settle
pointer click "$BTN_LEFT"
wait_for_pointer_enter "$UMBRIEL_RUNTIME_DIR/warp-same-b.log" "$expected_local" "$mouse_enters_before"

# A selected placement root may delegate keyboard focus to an attached modal dialog. The cursor still follows that
# selected root; keyboard delivery identifies its label without relying on the dialog's own label assignment.
readonly DIALOG_LOG="$UMBRIEL_RUNTIME_DIR/warp-dialog.log"
TRANSIENT_SUITE=1 TRANSIENT_MODAL=1 UMBRIEL_LOG_POINTER=1 APP_ID=warp-dialog \
  "$CLIENT" warp-dialog 900 600 > "$DIALOG_LOG" 2>&1 &
wait_for_count 7
"$UMBRIEL" settle
dialog_enters_before=$(pointer_enters "$DIALOG_LOG")
dialog_selected=false
for keycode in $(seq 2 8); do
  point=$(inset_of warp-decoy)
  read -r x y <<< "$point"
  pointer move "$x" "$y"
  "$UMBRIEL" settle
  "$UMBRIEL" msg overview-open > /dev/null
  "$UMBRIEL" settle
  closes_before=$(overview_closes)
  pointer tap "$keycode"
  wait_for_overview_close "$closes_before"
  "$UMBRIEL" settle
  keys_before=$(grep -c '^key 30 1$' "$DIALOG_LOG" 2>/dev/null || true)
  pointer tap 30
  for _ in $(seq 40); do
    [[ $(grep -c '^key 30 1$' "$DIALOG_LOG" 2>/dev/null || true) -gt $keys_before ]] && break
    sleep 0.05
  done
  if [[ $(grep -c '^key 30 1$' "$DIALOG_LOG" 2>/dev/null || true) -gt $keys_before ]]; then
    dialog_selected=true
    break
  fi
done
if [[ $dialog_selected != true ]]; then
  echo "no overview label restored keyboard input to the attached dialog: $(< "$DIALOG_LOG")"
  exit 1
fi
for _ in $(seq 40); do
  [[ $(pointer_enters "$DIALOG_LOG") -gt $dialog_enters_before ]] && break
  sleep 0.05
done
if [[ $(pointer_enters "$DIALOG_LOG") -le $dialog_enters_before ]]; then
  echo "the attached-dialog selection did not warp the cursor to its placement root: $(< "$DIALOG_LOG")"
  exit 1
fi

echo "overview shortcut cursor follow, opt-out, Escape, same-output selection, delegated dialog focus, keyboard delivery, and mouse behavior verified"
