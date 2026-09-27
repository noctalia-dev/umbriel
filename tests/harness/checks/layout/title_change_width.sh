#!/usr/bin/env bash
# A title change after map must not reapply an unchanged default_scrolling_extent rule.
set -euo pipefail

readonly TITLE_FIFO="$UMBRIEL_RUNTIME_DIR/title-width.fifo"

window_field() {
  "$UMBRIEL" windows --json | jq -r --arg field "$1" '.[] | select(.app_id == "helium") | .[$field]'
}

window_width() { window_field w; }

wait_for_title() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq -r '.[] | select(.app_id == "helium") | .title') == helium-navigated ]] && return 0
    sleep 0.1
  done
  echo "timed out waiting for helium title change: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_field() {
  local field=$1 expected=$2 actual=
  for _ in $(seq 60); do
    actual=$(window_field "$field")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected helium field '$field' to be '$expected', got '$actual': $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.app_id = "^helium$"
default_scrolling_extent = 0.75

[[window_rule]]
match.title = "^helium-navigated$"
default_floating_size_px = { width = 300, height = 200 }
default_position = { x = 20, y = 20, anchor = "bottom_right" }

[[window_rule]]
match.title = "^unrelated-title-rule$"
opacity = 0.9
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$TITLE_FIFO"
# TITLE_FIFO is a shell-local, so it still needs an explicit export to the client.
env TITLE_FIFO="$TITLE_FIFO" \
  foot --app-id=helium --title=helium-initial sh -c \
    'IFS= read -r title < "$TITLE_FIFO"; printf "\033]2;%s\007" "$title"; sleep 120' \
    > /dev/null 2>&1 &

for _ in $(seq 60); do
  [[ $(window_width) == 942 ]] && break
  sleep 0.1
done
initial_width=$(window_width)
if [[ $initial_width != 942 ]]; then
  echo "default_scrolling_extent did not produce the expected initial width: $("$UMBRIEL" windows --json)"
  exit 1
fi

"$UMBRIEL" msg window-set-primary-extent:0.25 > /dev/null
"$UMBRIEL" settle
manual_width=$(window_width)
if (( manual_width >= initial_width )); then
  echo "window-set-primary-extent did not shrink helium: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo helium-navigated > "$TITLE_FIFO"
wait_for_title
final_width=$(window_width)
if [[ $final_width != "$manual_width" ]]; then
  echo "title change reset manual width from $manual_width to $final_width: $("$UMBRIEL" windows --json)"
  exit 1
fi

# The late floating geometry stays symbolic while tiled. Its anchor must be evaluated against the saved 300x200 size,
# not the current tiled geometry.
"$UMBRIEL" msg window-toggle-floating > /dev/null
wait_for_field floating true
wait_for_field w 300
wait_for_field h 200
right_gap=$((1280 - $(window_field x) - $(window_field w)))
bottom_gap=$((720 - $(window_field y) - $(window_field h)))
if ((right_gap != 20 || bottom_gap != 20)); then
  echo "late floating geometry missed its bottom-right anchor: right=$right_gap bottom=$bottom_gap"
  exit 1
fi

echo "title changes preserve manual extents and defer floating geometry until first use"
