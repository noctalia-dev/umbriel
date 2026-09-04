#!/usr/bin/env bash
# A client that sets an empty title or an empty xdg tag can be matched by a rule whose pattern accepts the empty
# string, while a client that never set one is matched by nothing. Firefox's browser toolbox is the real case: it maps
# titleless, then titles itself with an empty string. That transition must reach dynamic effects, which are re-resolved
# per identity, and opening effects, which wait for the title to settle.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly DIM_LOG="$UMBRIEL_RUNTIME_DIR/dim-blank-title.log"
readonly FLOAT_LOG="$UMBRIEL_RUNTIME_DIR/float-blank-title.log"
readonly TAG_LOG="$UMBRIEL_RUNTIME_DIR/blank-tag.log"
readonly DIM_FIFO="$UMBRIEL_RUNTIME_DIR/dim-blank-title-control"
readonly FLOAT_FIFO="$UMBRIEL_RUNTIME_DIR/float-blank-title-control"
readonly BEFORE_SHOT="$UMBRIEL_RUNTIME_DIR/dim-blank-title-before.png"
readonly AFTER_SHOT="$UMBRIEL_RUNTIME_DIR/dim-blank-title-after.png"

if [[ ! -x $CLIENT ]]; then
  echo "unmap-client is not built"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[colors]
backdrop = "#00FF00FF"

[appearance]
border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false

[[window_rule]]
match.app_id = "^dim-blank-title$"
match.title = "^$"
opacity = 0.5

[[window_rule]]
match.app_id = "^float-blank-title$"
match.title = "^$"
default_floating = true

[[window_rule]]
match.xdg_tag = "^$"
default_floating = true
EOF
"$UMBRIEL" msg config-reload > /dev/null
# The inotify watcher reloads this append 150ms later. Its generation bump invalidates the per-identity rule cache, so
# let it land before the clients: an assertion here must observe the title change, not a reload.
sleep 0.3

await_window() {
  local filter=$1 message=$2 windows=
  for _ in $(seq 80); do
    windows=$("$UMBRIEL" windows --json)
    jq -e "$filter" <<< "$windows" > /dev/null && return 0
    sleep 0.05
  done
  echo "$message: $windows"
  return 1
}

# Mean green of the only visible window, isolated from the solid green backdrop. The client paints 0xFF5577AA, so an
# opaque window reads about 119 and one at 0.5 opacity over the backdrop reads about 187.
window_green() {
  local shot=$1 visual_width visual_height visual_x visual_y
  read -r visual_width visual_height visual_x visual_y < <(
    magick "$shot" -alpha off -fuzz 1% -transparent '#00FF00' -trim -format '%w %h %X %Y\n' info:
  )
  magick "$shot" -crop "20x20+$((visual_x + visual_width / 2 - 10))+$((visual_y + visual_height / 2 - 10))" \
    -format '%[fx:round(255*mean.g)]' info:
}

# An absent title matches no pattern, so this window opens opaque.
mkfifo "$DIM_FIFO"
exec {dim_fd}<>"$DIM_FIFO"
env APP_ID=dim-blank-title NO_TITLE=1 TITLE_AFTER_MAP= \
  "$CLIENT" dim-blank-title 800 600 <&"$dim_fd" > "$DIM_LOG" 2>&1 &
await_window 'length == 1 and .[0].app_id == "dim-blank-title"' "titleless window did not map" || exit 1
sleep 0.1
grim "$BEFORE_SHOT"
before_green=$(window_green "$BEFORE_SHOT")

# The client titles itself with an empty string. Only the dynamic opacity changes, so nothing else can invalidate the
# per-identity rule cache: an unset title has to be a different cache key than an empty one.
printf 'u' >&"$dim_fd"
for _ in $(seq 20); do
  grim "$AFTER_SHOT"
  after_green=$(window_green "$AFTER_SHOT")
  ((after_green >= before_green + 40)) && break
  sleep 0.05
done
if ((before_green > 140 || after_green < before_green + 40)); then
  echo "empty title did not refresh the dynamic opacity rule: green $before_green -> $after_green"
  exit 1
fi

# Opening effects wait for a title to settle, and an empty title settles it.
mkfifo "$FLOAT_FIFO"
exec {float_fd}<>"$FLOAT_FIFO"
env APP_ID=float-blank-title NO_TITLE=1 TITLE_AFTER_MAP= \
  "$CLIENT" float-blank-title 800 600 <&"$float_fd" > "$FLOAT_LOG" 2>&1 &
await_window '
  (.[] | select(.app_id == "float-blank-title") | .floating) == false
' "titleless window did not open tiled" || exit 1
printf 'u' >&"$float_fd"
await_window '
  (.[] | select(.app_id == "float-blank-title") | .floating) == true
' "empty title did not select the opening floating rule" || exit 1

# An empty tag set before the initial commit selects opening rules; this client keeps a non-empty title.
env APP_ID=blank-tag XDG_TAG= "$CLIENT" blank-tag-window 800 600 > "$TAG_LOG" 2>&1 &
await_window '
  (.[] | select(.app_id == "blank-tag") | .xdg_tag) == ""
  and (.[] | select(.app_id == "blank-tag") | .floating) == true
' "empty xdg tag did not select the floating rule" || exit 1

echo "empty title dimmed (green $before_green -> $after_green) and floated its windows; empty xdg tag floated its own"
