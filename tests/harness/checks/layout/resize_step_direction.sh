#!/usr/bin/env bash
# A resize step never moves a window's size against the step: shrinking a row at its minimum or a float smaller than a
# tenth of the output leaves it as it is, and growing a float larger than the output leaves it as it is.
set -euo pipefail

cat >> "$UMBRIEL_CONFIG" << 'EOF'

[[window_rule]]
match.title = "^tiny$"
default_floating = true
default_floating_size_px = { width = 110, height = 50 }

[[window_rule]]
match.title = "^huge$"
default_floating = true
default_floating_size_px = { width = 1500, height = 900 }
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_windows() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') == "$1" ]] && return 0
    sleep 0.1
  done
  echo "expected $1 window(s): $("$UMBRIEL" windows --json)"
  return 1
}

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

spawn_client top
wait_for_windows 1
spawn_client bottom
wait_for_windows 2
"$UMBRIEL" msg window-consume-left > /dev/null
"$UMBRIEL" settle

# Mod+right-drag the top row's bottom edge to the top of the output, which leaves it at its minimum height.
edge=$(("$(field_of top y)" + "$(field_of top h)" - 2))
"$UMBRIEL_POINTER_CLIENT" 1280 720 mod logo move 600 "$edge" press 273 move 600 1 release 273 mod none
"$UMBRIEL" settle
top_h=$(field_of top h)

"$UMBRIEL" msg window-focus "$(field_of top id)" > /dev/null
"$UMBRIEL" msg window-modify-secondary-extent -0.1 > /dev/null
"$UMBRIEL" settle
if (($(field_of top h) != top_h)); then
  echo "expected the row at its minimum to keep $top_h px: $("$UMBRIEL" windows --json)"
  exit 1
fi

spawn_client tiny
wait_for_windows 3
"$UMBRIEL" msg window-focus "$(field_of tiny id)" > /dev/null
"$UMBRIEL" msg window-modify-secondary-extent -0.1 > /dev/null
"$UMBRIEL" msg window-modify-primary-extent -0.05 > /dev/null
"$UMBRIEL" settle
if (($(field_of tiny w) != 110)) || (($(field_of tiny h) != 50)); then
  echo "expected the small float to stay 110x50: $("$UMBRIEL" windows --json)"
  exit 1
fi

spawn_client huge
wait_for_windows 4
"$UMBRIEL" msg window-focus "$(field_of huge id)" > /dev/null
"$UMBRIEL" msg window-modify-secondary-extent 0.1 > /dev/null
"$UMBRIEL" msg window-modify-primary-extent 0.05 > /dev/null
"$UMBRIEL" settle
if (($(field_of huge w) != 1500)) || (($(field_of huge h) != 900)); then
  echo "expected the large float to stay 1500x900: $("$UMBRIEL" windows --json)"
  exit 1
fi

echo "resize steps left windows past their limits as they were"
