#!/usr/bin/env bash
# A client can identify an xdg toplevel before its initial commit, then replace
# that tag after mapping. Opening rules must see the initial value, while a
# later replacement refreshes dynamic rules without replaying opening behavior.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/xdg-tag-client.log"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/xdg-tag-control"
readonly BEFORE_SHOT="$UMBRIEL_RUNTIME_DIR/xdg-tag-before.png"
readonly AFTER_SHOT="$UMBRIEL_RUNTIME_DIR/xdg-tag-after.png"
CLIENT_PID=

if [[ ! -x $CLIENT ]]; then
  echo "unmap-client is not built"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[appearance]
border_width = 0
corner_radius = 0
backdrop_color = "#00FF00FF"

[appearance.shadow]
enabled = false

[[window_rule]]
match.xdg_tag = "^game-running$"
opacity = 0.5

[[window_rule]]
match.xdg_tag = "^game-running$"
default_floating = false

[[window_rule]]
match.xdg_tag = "^game-launcher$"
default_floating = true
opacity = 0.9
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
env \
  APP_ID=xdg-tag-client \
  XDG_TAG=game-launcher \
  XDG_TAG_AFTER_MAP=game-running \
  "$CLIENT" xdg-tag-client <&"$control_fd" > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!

for _ in $(seq 80); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
    wait "$CLIENT_PID" 2>/dev/null || true
    echo "xdg tag client exited before mapping: $(< "$CLIENT_LOG")"
    exit 1
  fi
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "xdg tag client did not map: $(< "$CLIENT_LOG")"
  exit 1
fi

windows=
for _ in $(seq 80); do
  windows=$("$UMBRIEL" windows --json)
  jq -e '
    length == 1
    and .[0].app_id == "xdg-tag-client"
    and .[0].floating == true
    and .[0].xdg_tag == "game-launcher"
  ' <<< "$windows" > /dev/null && break
  sleep 0.05
done
if ! jq -e '
  length == 1
  and .[0].app_id == "xdg-tag-client"
  and .[0].floating == true
  and .[0].xdg_tag == "game-launcher"
' <<< "$windows" > /dev/null; then
  echo "initial xdg tag or opening rule was not applied: $windows"
  exit 1
fi
if ! "$UMBRIEL" windows | grep -Fq '[xdg_tag=game-launcher]'; then
  echo "human-readable window output did not expose the xdg tag: $("$UMBRIEL" windows)"
  exit 1
fi

sleep 0.1
grim "$BEFORE_SHOT"
read -r image_width image_height < <(magick identify -format '%w %h\n' "$BEFORE_SHOT")
read -r visual_width visual_height visual_x visual_y < <(
  magick "$BEFORE_SHOT" -alpha off -fuzz 1% -transparent '#00FF00' -trim -format '%w %h %X %Y\n' info:
)
sample_x=$((visual_x + visual_width / 2))
sample_y=$((visual_y + visual_height / 2))
if ((sample_x < 0 || sample_x >= image_width || sample_y < 0 || sample_y >= image_height)); then
  echo "visible window center is outside the captured output: geometry ${visual_width}x${visual_height}${visual_x}${visual_y}; image ${image_width}x${image_height}"
  exit 1
fi
before_green=$(
  magick "$BEFORE_SHOT" -crop "20x20+$((sample_x - 10))+$((sample_y - 10))" \
    -format '%[fx:round(255*mean.g)]' info:
)

printf 'u' >&"$control_fd"
for _ in $(seq 80); do
  windows=$("$UMBRIEL" windows --json)
  if grep -q '^xdg-tag-updated$' "$CLIENT_LOG" \
      && jq -e '
        length == 1
        and .[0].xdg_tag == "game-running"
        and .[0].floating == true
      ' <<< "$windows" > /dev/null; then
    break
  fi
  sleep 0.05
done
if ! grep -q '^xdg-tag-updated$' "$CLIENT_LOG" \
    || ! jq -e '
      length == 1
      and .[0].xdg_tag == "game-running"
      and .[0].floating == true
    ' <<< "$windows" > /dev/null; then
  echo "post-map xdg tag did not reach IPC or replayed an opening rule: $windows; client log: $(< "$CLIENT_LOG")"
  exit 1
fi
if ! "$UMBRIEL" windows | grep -Fq '[xdg_tag=game-running]'; then
  echo "human-readable window output did not refresh the xdg tag: $("$UMBRIEL" windows)"
  exit 1
fi

sleep 0.1
grim "$AFTER_SHOT"
after_green=$(
  magick "$AFTER_SHOT" -crop "20x20+$((sample_x - 10))+$((sample_y - 10))" \
    -format '%[fx:round(255*mean.g)]' info:
)
if ((after_green < before_green + 30)); then
  echo "post-map xdg tag did not replace the old tag or apply dynamic opacity: green $before_green -> $after_green"
  exit 1
fi

echo "initial xdg tag applied opening rules; replacement refreshed IPC and opacity: green $before_green -> $after_green"
