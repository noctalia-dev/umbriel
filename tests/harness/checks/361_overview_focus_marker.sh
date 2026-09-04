#!/usr/bin/env bash
# harness: outputs=2
# No window holds the seat while the overview is open, so exactly one card may wear the focused border: the one a
# focus or close action would act on, which is the active workspace's focused view on the current output. Other rows
# keep a weaker landing marker. The current output is cursor-defined, and every keybind that changes it warps the
# cursor, so `output-focus-right` must swap which card is strong. The check measures the marker green on each output's
# workspace row: a pure green focused border, a black unfocused border, and a blend for a landing target that is not
# live.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly FIRST_SHOT="$UMBRIEL_RUNTIME_DIR/overview-marker-first"
readonly SECOND_SHOT="$UMBRIEL_RUNTIME_DIR/overview-marker-second"

output_x() {
  "$UMBRIEL" outputs | awk -v name="$1" '$1 == name {found = 1; next} found && /Position:/ {split($2, p, ","); print p[1]; exit}'
}

# The workspace row is the output box scaled by `zoom` and centered, so 1280x800 at 0.5 puts it at 640x400+320+200.
row_green() {
  magick "$1" -crop 640x400+320+200 +repage -colorspace RGB -format '%[fx:round(255*mean.g)]' info:
}

assert_markers() {
  local label=$1 live_name=$2 live=$3 landing_name=$4 landing=$5
  if ((live < 40)); then
    echo "$label: $live_name holds the cursor but its card is not marked live: green=$live"
    return 1
  fi
  if ((landing < 3)); then
    echo "$label: $landing_name lost its landing marker entirely: green=$landing"
    return 1
  fi
  if ((landing * 2 > live)); then
    echo "$label: $landing_name is marked as strongly as the live target: live=$live landing=$landing"
    return 1
  fi
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[colors]
backdrop = "#000000FF"

[colors.border]
focused = "#00FF00FF"
unfocused = "#000000FF"

[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"

[appearance]
animation_ms = 100
border_width = 100
outer_border_width = 0
corner_radius = 0

[appearance.blur]
enabled = false

[overview]
zoom = 0.5

[[window_rule]]
match.app_id = "^overview-marker-first$"
default_output = "HEADLESS-1"

[[window_rule]]
match.app_id = "^overview-marker-second$"
default_output = "HEADLESS-2"
EOF
"$UMBRIEL" msg config-reload > /dev/null

first=HEADLESS-1
second=HEADLESS-2
first_x=$(output_x "$first")
second_x=$(output_x "$second")
if [[ -z $first_x || -z $second_x || $first_x == "$second_x" ]]; then
  echo "could not resolve distinct output positions: $("$UMBRIEL" outputs)"
  exit 1
fi
if ((second_x > first_x)); then
  toward_second=output-focus-right
  toward_first=output-focus-left
else
  toward_second=output-focus-left
  toward_first=output-focus-right
fi

APP_ID=overview-marker-first "$CLIENT" overview-marker-first 900 500 \
  > "$UMBRIEL_RUNTIME_DIR/overview-marker-first-client.log" 2>&1 &
APP_ID=overview-marker-second "$CLIENT" overview-marker-second 900 500 \
  > "$UMBRIEL_RUNTIME_DIR/overview-marker-second-client.log" 2>&1 &

for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 2 ]] && break
  sleep 0.1
done
placement=$("$UMBRIEL" windows --json | jq -r '[.[] | "\(.app_id)@\(.workspace)"] | sort | join(" ")')
if [[ $placement != "overview-marker-first@$first:"*"overview-marker-second@$second:"* ]]; then
  echo "windows did not land one per output: $placement"
  exit 1
fi

# The cursor defines the current output, and the second window was focused last: the marker must follow the cursor's
# output, not the most recently focused window.
"$POINTER" 2560 800 move "$((first_x + 640))" 400
sleep 0.2
"$UMBRIEL" msg overview-open > /dev/null
sleep 0.4

grim -o "$first" "$FIRST_SHOT-a.png"
grim -o "$second" "$SECOND_SHOT-a.png"
first_a=$(row_green "$FIRST_SHOT-a.png")
second_a=$(row_green "$SECOND_SHOT-a.png")
assert_markers "cursor on $first" "$first" "$first_a" "$second" "$second_a"

# A keyboard-only session changes the current output through this action, which warps the cursor. The strong marker
# must move with it while the overview stays open.
"$UMBRIEL" msg "$toward_second" > /dev/null
sleep 0.4
grim -o "$first" "$FIRST_SHOT-b.png"
grim -o "$second" "$SECOND_SHOT-b.png"
first_b=$(row_green "$FIRST_SHOT-b.png")
second_b=$(row_green "$SECOND_SHOT-b.png")
assert_markers "after $toward_second" "$second" "$second_b" "$first" "$first_b"

"$UMBRIEL" msg "$toward_first" > /dev/null
sleep 0.4
grim -o "$first" "$FIRST_SHOT-c.png"
grim -o "$second" "$SECOND_SHOT-c.png"
first_c=$(row_green "$FIRST_SHOT-c.png")
second_c=$(row_green "$SECOND_SHOT-c.png")
assert_markers "after $toward_first" "$first" "$first_c" "$second" "$second_c"

# Plain pointer motion across outputs carries no focus change with `input.focus.follows_mouse` off, so the marker has
# to repaint off the pointer's output alone.
"$POINTER" 2560 800 move "$((second_x + 640))" 400
sleep 0.4
grim -o "$first" "$FIRST_SHOT-d.png"
grim -o "$second" "$SECOND_SHOT-d.png"
first_d=$(row_green "$FIRST_SHOT-d.png")
second_d=$(row_green "$SECOND_SHOT-d.png")
assert_markers "pointer moved to $second" "$second" "$second_d" "$first" "$first_d"

echo "overview marks one live card per session: $first $first_a/$first_b/$first_c/$first_d, $second $second_a/$second_b/$second_c/$second_d"
