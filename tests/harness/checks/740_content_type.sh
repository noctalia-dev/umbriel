#!/usr/bin/env bash
# A client can mark its main rendering subsurface as game content, as current
# Proton does. Opening rules and IPC must see it, then a committed type change
# must refresh dynamic rules without replaying one-shot opening behavior.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/content-type-client.log"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/content-type-control"
readonly BEFORE_SHOT="$UMBRIEL_RUNTIME_DIR/content-type-before.png"
readonly AFTER_SHOT="$UMBRIEL_RUNTIME_DIR/content-type-after.png"
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

[[window_rule]]
match.content_type = "game"
default_floating = true

[[window_rule]]
match.content_type = "video"
opacity = 0.5

[[window_rule]]
match.content_type = "video"
default_floating = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
env \
  APP_ID=content-type-client \
  CONTENT_TYPE=game \
  CONTENT_TYPE_ON_SUBSURFACE=1 \
  CONTENT_TYPE_AFTER_MAP=video \
  "$CLIENT" content-type-client <&"$control_fd" > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!

for _ in $(seq 80); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
    wait "$CLIENT_PID" 2>/dev/null || true
    echo "content type client exited before mapping: $(< "$CLIENT_LOG")"
    exit 1
  fi
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "content type client did not map: $(< "$CLIENT_LOG")"
  exit 1
fi

windows=
for _ in $(seq 80); do
  windows=$("$UMBRIEL" windows --json)
  jq -e '
    length == 1
    and .[0].app_id == "content-type-client"
    and .[0].floating == true
    and .[0].content_type == "game"
  ' <<< "$windows" > /dev/null && break
  sleep 0.05
done
if ! jq -e '
  length == 1
  and .[0].app_id == "content-type-client"
  and .[0].floating == true
  and .[0].content_type == "game"
' <<< "$windows" > /dev/null; then
  echo "initial subsurface content type or opening rule was not applied: $windows"
  exit 1
fi
if ! "$UMBRIEL" windows | grep -Fq '[content_type=game]'; then
  echo "human-readable window output did not expose the content type: $("$UMBRIEL" windows)"
  exit 1
fi

sleep 0.1
grim "$BEFORE_SHOT"
read -r image_width image_height < <(magick identify -format '%w %h\n' "$BEFORE_SHOT")
sample_x=$((image_width / 2))
sample_y=$((image_height / 2))
before_green=$(
  magick "$BEFORE_SHOT" -crop "20x20+$((sample_x - 10))+$((sample_y - 10))" \
    -format '%[fx:round(255*mean.g)]' info:
)

printf 'u' >&"$control_fd"
for _ in $(seq 80); do
  windows=$("$UMBRIEL" windows --json)
  if grep -q '^content-type-updated$' "$CLIENT_LOG" \
      && jq -e '
        length == 1
        and .[0].content_type == "video"
        and .[0].floating == true
      ' <<< "$windows" > /dev/null; then
    break
  fi
  sleep 0.05
done
if ! grep -q '^content-type-updated$' "$CLIENT_LOG" \
    || ! jq -e '
      length == 1
      and .[0].content_type == "video"
      and .[0].floating == true
    ' <<< "$windows" > /dev/null; then
  echo "post-map content type did not reach IPC or replayed an opening rule: $windows; client log: $(< "$CLIENT_LOG")"
  exit 1
fi

sleep 0.1
grim "$AFTER_SHOT"
after_green=$(
  magick "$AFTER_SHOT" -crop "20x20+$((sample_x - 10))+$((sample_y - 10))" \
    -format '%[fx:round(255*mean.g)]' info:
)
if ((after_green < before_green + 30)); then
  echo "post-map content type did not apply dynamic opacity: green $before_green -> $after_green"
  exit 1
fi

echo "subsurface content type opening rule and IPC passed; later type refreshed opacity: green $before_green -> $after_green"
