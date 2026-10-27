#!/usr/bin/env bash
# A scrolling column may span several viewports: window-set-primary-extent 2.0 configures the client to about twice the
# output's width, the strip is longer than the output and pans through layout-scroll-*, and the pan stops at the
# column's ends. Clipping to the output is scene-root machinery and needs a second output to observe; it stays a
# manual check.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

windows() { "$UMBRIEL" windows --json; }

wait_for_window() {
  local title=$1
  for _ in $(seq 80); do
    if windows | jq -e --arg title "$title" 'any(.[]; .title == $title)' > /dev/null; then
      return 0
    fi
    sleep 0.025
  done
  echo "window $title did not map: $(windows)"
  return 1
}

field() {
  local title=$1 key=$2
  windows | jq -r --arg title "$title" --arg key "$key" '.[] | select(.title == $title) | .[$key]'
}

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[layout]
mode = "scrolling"

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

env APP_ID=wide-column FILL_COLOR=0xFFFF0000 RESIZE_FILL_COLOR=0xFFFF0000 \
  "$UMBRIEL_UNMAP_CLIENT" wide-a 400 400 > "$UMBRIEL_RUNTIME_DIR/wide-a.log" 2>&1 &
wait_for_window wide-a
"$UMBRIEL" settle > /dev/null

# 2.0 spans two viewports with the gap between the would-be slots: a little under twice the 1280-wide output.
"$UMBRIEL" msg window-set-primary-extent:2.0 > /dev/null
"$UMBRIEL" settle > /dev/null
width=$(field wide-a w)
if (( width < 2450 || width > 2650 )); then
  echo "window-set-primary-extent:2.0 configured ${width}px, expected about twice the 1280-wide output: $(windows)"
  exit 1
fi

# The strip is longer than the output: panning moves the column's on-screen x.
start_x=$(field wide-a x)
"$UMBRIEL" msg layout-scroll-right > /dev/null
"$UMBRIEL" settle > /dev/null
panned_x=$(field wide-a x)
if (( panned_x >= start_x )); then
  echo "layout-scroll-right did not pan the wide column (x ${start_x} -> ${panned_x}): $(windows)"
  exit 1
fi

# The pan range stops at the column's trailing edge instead of scrolling the column away entirely.
for _ in $(seq 30); do
  "$UMBRIEL" msg layout-scroll-right > /dev/null
done
"$UMBRIEL" settle > /dev/null
end_x=$(field wide-a x)
for _ in $(seq 5); do
  "$UMBRIEL" msg layout-scroll-right > /dev/null
done
"$UMBRIEL" settle > /dev/null
if (( $(field wide-a x) != end_x )); then
  echo "the pan ran past the wide column's trailing edge (x $end_x -> $(field wide-a x)): $(windows)"
  exit 1
fi

# The fraction round-trips: back to one viewport the column fits the output again.
"$UMBRIEL" msg window-set-primary-extent:1.0 > /dev/null
"$UMBRIEL" settle > /dev/null
narrow=$(field wide-a w)
if (( narrow < 1230 || narrow > 1330 )); then
  echo "window-set-primary-extent:1.0 configured ${narrow}px, expected about one output width: $(windows)"
  exit 1
fi

echo "a wide scrolling column spans about two viewports, pans through them, and clamps at its ends"
