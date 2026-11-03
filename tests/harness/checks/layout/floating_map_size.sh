#!/usr/bin/env bash
# harness: outputs=2
# A title settling between the opening configure and map applies new floating
# dimensions once, without replaying unchanged axes over the client's first size.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[layout]
gap = 0
[appearance]
border_width = 0
outer_border_width = 0
[output.HEADLESS-1]
position = [0, 0]
[output.HEADLESS-2]
position = [1280, 0]
scale = 2.0

[[window_rule]]
default_output = "HEADLESS-1"
default_scrolling_extent = 1.0

[[window_rule]]
match.title = "^opening-"
default_floating = true

[[window_rule]]
match.title = "^opening-(late|early|stable-before|stable-after|changed-after|postmap)$"
default_floating_size_px = { width = 800, height = 625 }

[[window_rule]]
match.title = "^opening-(axis-before|unit-before)$"
default_floating_size_px = { width = 800 }

[[window_rule]]
match.title = "^opening-axis-after$"
default_floating_size_px = { width = 800, height = 625 }

[[window_rule]]
match.title = "^opening-changed-before$"
default_floating_size_px = { width = 600, height = 400 }

[[window_rule]]
match.title = "^opening-changed-after$"
default_position = { x = 20, y = 30, anchor = "bottom_right" }

[[window_rule]]
match.title = "^opening-unit-after$"
default_floating_size = { width = 0.625, height = 0.5 }

[[window_rule]]
match.title = "^opening-scaled$"
default_output = "HEADLESS-2"
default_floating_size = { width = 0.5, height = 0.5 }
default_floating_size_px = { width = 300 }
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

open_case() {
  local title=$1 initial_title=$2 configured=$3 first=$4 expected=$5 resize=$6 phase=$7
  local log="$UMBRIEL_RUNTIME_DIR/$title.log" control_fd actual
  local args=(LOG_CONFIGURES=1 DECORATION_CONTROL=1)
  [[ $resize == false ]] || args+=(RESIZE_BEFORE_MAP=1)
  [[ $phase != before ]] || args+=("TITLE_AFTER_CONFIGURE=$title")
  mkfifo "$UMBRIEL_RUNTIME_DIR/$title.fifo"
  exec {control_fd}<> "$UMBRIEL_RUNTIME_DIR/$title.fifo"
  env "${args[@]}" "$UMBRIEL_FRACTIONAL_CLIENT" "$initial_title" 700 500 \
    <&"$control_fd" > "$log" 2>&1 &

  await_lines "$log" "mapped $first buffer " 1
  actual=$(awk '$1 == "configure" { print $2; exit }' "$log")
  if [[ $actual != "$configured" ]]; then
    echo "$title: opening configure was $actual, expected $configured"
    cat "$log"
    return 1
  fi
  if [[ $phase == after ]]; then
    "$UMBRIEL" settle > /dev/null
    printf 'title %s\n' "$title" >&"$control_fd"
  elif [[ $phase == before ]]; then
    await_lines "$log" "title-after-configure $title" 1
  fi
  await_lines "$log" "mapped $expected buffer " 1
  "$UMBRIEL" settle > /dev/null
  actual=$("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)')
  if ! jq -e --arg size "$expected" '
    .floating and (.fullscreen | not) and (.maximized | not)
      and "\(.w)x\(.h)" == $size
  ' <<< "$actual" > /dev/null; then
    echo "$title: expected a floating $expected window, got $actual"
    cat "$log"
    return 1
  fi
  if [[ $phase != after ]]; then
    local width=${expected%x*} height=${expected#*x} x y
    x=$(((1280 - width) / 2))
    y=$(((720 - height) / 2))
    if [[ $title == opening-changed-after ]]; then
      x=$((1280 - width - 20))
      y=$((720 - height - 30))
    elif [[ $title == opening-scaled ]]; then
      x=$((1280 + (640 - width) / 2))
      y=$(((360 - height) / 2))
    fi
    if ! jq -e --argjson x "$x" --argjson y "$y" '.x == $x and .y == $y' <<< "$actual" > /dev/null; then
      echo "$title: expected placement at $x,$y for $expected, got $actual"
      return 1
    fi
  fi
  if [[ $expected != "$first" ]] && ! awk -v expected="$expected" '
    $1 == "mapped" { mapped = 1 }
    mapped && $1 == "configure" && $2 == expected { resized = 1 }
    END { exit !resized }
  ' "$log"; then
    echo "$title: no configure changed the first committed size $first to $expected"
    cat "$log"
    return 1
  fi
  printf 'resize 540 330\n' >&"$control_fd"
  await_lines "$log" 'mapped 540x330 buffer ' 1
  "$UMBRIEL" settle > /dev/null
  actual=$("$UMBRIEL" windows --json | jq -c --arg title "$title" '.[] | select(.title == $title)')
  if ! jq -e '.floating and .w == 540 and .h == 330' <<< "$actual" > /dev/null; then
    echo "$title: the opening rule overwrote a later client resize: $actual"
    return 1
  fi
  exec {control_fd}>&-
}

# A tiled opening becomes floating before map, and receives the missing size configure.
open_case opening-late waiting-late 1280x720 1280x720 800x625 false before

# Already applied defaults must not overwrite a content-driven first buffer.
open_case opening-early opening-early 800x625 700x500 700x500 true early
open_case opening-stable-after opening-stable-before 800x625 700x500 700x500 true before

# Changing one axis preserves the client's size on the unchanged axis.
open_case opening-axis-after opening-axis-before 800x0 700x500 700x625 true before
open_case opening-changed-after opening-changed-before 600x400 600x400 800x625 false before

# Equivalent pixel and fractional widths are not replayed when a new height is selected.
open_case opening-unit-after opening-unit-before 800x0 700x500 700x360 true before

# The existing post-map identity path continues to apply the same opening rules.
open_case opening-postmap waiting-postmap 1280x720 1280x720 800x625 false after

# Fractions use the final output's logical usable area; pixels still take precedence.
open_case opening-scaled waiting-scaled 1280x720 1280x720 300x180 false before
"$UMBRIEL" windows --json | jq -e '
  any(.[]; .title == "opening-scaled" and .output == "HEADLESS-2")
' > /dev/null

echo "map-time floating rules resized newly selected axes, preserved client sizes, and used logical output geometry"
