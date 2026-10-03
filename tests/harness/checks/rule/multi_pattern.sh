#!/usr/bin/env bash
# A pattern selector takes one pattern or an array of them, and any one of them matching is enough. Each window below
# is selected by a different entry of the same array, and windows matching none of the entries are left alone, so an
# entry dropped while reading an array would show up as an unstyled window.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

if [[ ! -x $CLIENT ]]; then
  echo "unmap-client is not built at $CLIENT"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[[window_rule]]
match.title = ["^multi-first$", "^multi-second$"]
default_floating = true
default_floating_size_px = { width = 400, height = 300 }

[[window_rule]]
match.app_id = ["^multi-app-a$", "^multi-app-b$"]
default_floating = true

[[window_rule]]
match.xdg_tag = ["^multi-tag-a$", "^multi-tag-b$"]
default_floating = true
EOF
"$UMBRIEL" msg config-reload > /dev/null
# The inotify watcher reloads this append 150ms later. Its generation bump invalidates the per-identity rule cache, so
# let it land before the clients: an assertion here must observe the array itself, not a reload.
sleep 0.3 # real time: inotify reload timer

field_of() {
  "$UMBRIEL" windows --json | jq -r --arg t "$1" --arg f "$2" '.[] | select(.title == $t) | .[$f]'
}

wait_mapped() {
  local title=$1
  for _ in $(seq 80); do
    [[ -n $(field_of "$title" w) ]] && return 0
    sleep 0.1
  done
  echo "window '$title' never mapped"
  exit 1
}

open() {
  local title=$1
  shift
  env "$@" "$CLIENT" "$title" > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
  wait_mapped "$title"
}

open multi-first
open multi-second
open multi-untouched
# Each of these is selected by the second entry of its array, the one a reader keeping only the first would drop.
open multi-app-b-window APP_ID=multi-app-b
open multi-tag-window APP_ID=multi-tag-app XDG_TAG=multi-tag-b

for title in multi-first multi-second; do
  if [[ $(field_of "$title" floating) != true ]]; then
    echo "'$title' did not match the title array"
    exit 1
  fi
  width=$(field_of "$title" w)
  if [[ $width != 400 ]]; then
    echo "'$title' did not adopt the rule's floating size: w=$width"
    exit 1
  fi
done

for title in multi-app-b-window multi-tag-window; do
  if [[ $(field_of "$title" floating) != true ]]; then
    echo "'$title' did not match its app_id or xdg_tag array"
    exit 1
  fi
done

# A window matching no entry of any array is unaffected by the rules.
if [[ $(field_of multi-untouched floating) != false ]]; then
  echo "a window matching no pattern was styled by a rule"
  exit 1
fi

echo "the second entry of the title, app_id, and xdg_tag arrays each selected its window; a window matching none did not"
