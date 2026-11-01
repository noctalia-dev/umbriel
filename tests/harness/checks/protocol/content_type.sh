#!/usr/bin/env bash
# A client can mark its main rendering subsurface as game content, as current
# Proton does. Opening rules and IPC must see both pre-map and late-created
# rendering children. Later type changes refresh dynamic rules without replaying
# one-shot opening behavior.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/content-type-client.log"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/content-type-control"
readonly BEFORE_SHOT="$UMBRIEL_RUNTIME_DIR/content-type-before.png"
readonly AFTER_SHOT="$UMBRIEL_RUNTIME_DIR/content-type-after.png"
readonly LATE_LOG="$UMBRIEL_RUNTIME_DIR/content-type-late.log"
readonly LATE_FIFO="$UMBRIEL_RUNTIME_DIR/content-type-late-control"
readonly REPLAY_LOG="$UMBRIEL_RUNTIME_DIR/content-type-replay.log"
readonly REPLAY_FIFO="$UMBRIEL_RUNTIME_DIR/content-type-replay-control"
CLIENT_PID=

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

[output.HEADLESS-1]
workspaces = 2

[[window_rule]]
match.content_type = "game"
default_floating = true

[[window_rule]]
match.content_type = "video"
opacity = 0.5

[[window_rule]]
match.content_type = "video"
default_floating = false

[[window_rule]]
match.app_id = "^content-type-late$"
match.content_type = "game"
default_workspace = 2

[[window_rule]]
match.title = "^content-type-replay-opening$"
default_workspace = 2

[[window_rule]]
match.app_id = "^content-type-replay$"
match.content_type = "game"
default_workspace = 2
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
IFS=x read -r image_width image_height < <("$UMBRIEL_PIXEL_PROBE" "$BEFORE_SHOT" size)
sample_x=$((image_width / 2))
sample_y=$((image_height / 2))
read -r _ before_green _ \
  < <("$UMBRIEL_PIXEL_PROBE" "$BEFORE_SHOT" mean "20x20+$((sample_x - 10))+$((sample_y - 10))")

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
read -r _ after_green _ \
  < <("$UMBRIEL_PIXEL_PROBE" "$AFTER_SHOT" mean "20x20+$((sample_x - 10))+$((sample_y - 10))")
if ((after_green < before_green + 30)); then
  echo "post-map content type did not apply dynamic opacity: green $before_green -> $after_green"
  exit 1
fi

source_workspace=$(
  "$UMBRIEL" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .index == 1) | .id'
)
target_workspace=$(
  "$UMBRIEL" workspaces --json | jq -r '.[] | select(.output == "HEADLESS-1" and .index == 2) | .id'
)
if [[ -z $source_workspace || -z $target_workspace ]]; then
  echo "content type workspaces were not created: $("$UMBRIEL" workspaces --json)"
  exit 1
fi

mkfifo "$LATE_FIFO"
exec {late_fd}<>"$LATE_FIFO"
env \
  APP_ID=content-type-late \
  CONTENT_TYPE_AFTER_MAP=game \
  CONTENT_TYPE_ON_SUBSURFACE=1 \
  "$CLIENT" content-type-late <&"$late_fd" > "$LATE_LOG" 2>&1 &

for _ in $(seq 80); do
  windows=$("$UMBRIEL" windows --json)
  if grep -q '^mapped$' "$LATE_LOG" \
      && jq -e --arg workspace "$source_workspace" '
        any(.[];
          .app_id == "content-type-late"
          and .content_type == "none"
          and .floating == false
          and .workspace == $workspace
        )
      ' <<< "$windows" > /dev/null; then
    break
  fi
  sleep 0.05
done
if ! grep -q '^mapped$' "$LATE_LOG" \
    || ! jq -e --arg workspace "$source_workspace" '
      any(.[];
        .app_id == "content-type-late"
        and .content_type == "none"
        and .floating == false
        and .workspace == $workspace
      )
    ' <<< "$windows" > /dev/null; then
  echo "late content type client did not initially map on workspace 1: $windows; client log: $(< "$LATE_LOG")"
  exit 1
fi

printf 'u' >&"$late_fd"
for _ in $(seq 80); do
  windows=$("$UMBRIEL" windows --json)
  if grep -q '^content-type-updated$' "$LATE_LOG" \
      && jq -e --arg workspace "$target_workspace" '
        any(.[];
          .app_id == "content-type-late"
          and .content_type == "game"
          and .floating == true
          and .workspace == $workspace
        )
      ' <<< "$windows" > /dev/null; then
    break
  fi
  sleep 0.05
done
if ! grep -q '^content-type-updated$' "$LATE_LOG" \
    || ! jq -e --arg workspace "$target_workspace" '
      any(.[];
        .app_id == "content-type-late"
        and .content_type == "game"
        and .floating == true
        and .workspace == $workspace
      )
    ' <<< "$windows" > /dev/null; then
  echo "first post-map content type did not apply initial workspace placement: $windows; client log: $(< "$LATE_LOG")"
  exit 1
fi

mkfifo "$REPLAY_FIFO"
exec {replay_fd}<>"$REPLAY_FIFO"
env \
  APP_ID=content-type-replay \
  TITLE_AFTER_MAP=content-type-replay-settled \
  CONTENT_TYPE_AFTER_MAP=game \
  CONTENT_TYPE_ON_SUBSURFACE=1 \
  "$CLIENT" content-type-replay-opening <&"$replay_fd" > "$REPLAY_LOG" 2>&1 &

for _ in $(seq 80); do
  windows=$("$UMBRIEL" windows --json)
  if grep -q '^mapped$' "$REPLAY_LOG" \
      && jq -e --arg workspace "$target_workspace" '
        any(.[];
          .app_id == "content-type-replay"
          and .title == "content-type-replay-opening"
          and .content_type == "none"
          and .workspace == $workspace
        )
      ' <<< "$windows" > /dev/null; then
    break
  fi
  sleep 0.05
done
if ! grep -q '^mapped$' "$REPLAY_LOG" \
    || ! jq -e --arg workspace "$target_workspace" '
      any(.[];
        .app_id == "content-type-replay"
        and .title == "content-type-replay-opening"
        and .content_type == "none"
        and .workspace == $workspace
      )
    ' <<< "$windows" > /dev/null; then
  echo "replay client did not apply its opening title rule: $windows; client log: $(< "$REPLAY_LOG")"
  exit 1
fi

printf 't' >&"$replay_fd"
for _ in $(seq 80); do
  windows=$("$UMBRIEL" windows --json)
  if grep -q '^title-updated$' "$REPLAY_LOG" \
      && jq -e --arg workspace "$target_workspace" '
        any(.[];
          .app_id == "content-type-replay"
          and .title == "content-type-replay-settled"
          and .workspace == $workspace
        )
      ' <<< "$windows" > /dev/null; then
    break
  fi
  sleep 0.05
done
if ! grep -q '^title-updated$' "$REPLAY_LOG" \
    || ! jq -e --arg workspace "$target_workspace" '
      any(.[];
        .app_id == "content-type-replay"
        and .title == "content-type-replay-settled"
        and .workspace == $workspace
      )
    ' <<< "$windows" > /dev/null; then
  echo "replay client title did not settle on workspace 2: $windows; client log: $(< "$REPLAY_LOG")"
  exit 1
fi
replay_id=$(
  jq -r '.[] | select(.app_id == "content-type-replay" and .title == "content-type-replay-settled") | .id' \
    <<< "$windows"
)
"$UMBRIEL" msg window-focus "$replay_id" > /dev/null
"$UMBRIEL" msg window-move-to-workspace 1 > /dev/null

for _ in $(seq 80); do
  windows=$("$UMBRIEL" windows --json)
  jq -e --arg workspace "$source_workspace" '
    any(.[]; .app_id == "content-type-replay" and .workspace == $workspace)
  ' <<< "$windows" > /dev/null && break
  sleep 0.05
done
if ! jq -e --arg workspace "$source_workspace" '
  any(.[]; .app_id == "content-type-replay" and .workspace == $workspace)
' <<< "$windows" > /dev/null; then
  echo "replay client could not be moved to workspace 1: $windows"
  exit 1
fi
printf 'u' >&"$replay_fd"
for _ in $(seq 80); do
  windows=$("$UMBRIEL" windows --json)
  if grep -q '^content-type-updated$' "$REPLAY_LOG" \
      && jq -e --arg workspace "$source_workspace" '
        any(.[];
          .app_id == "content-type-replay"
          and .content_type == "game"
          and .floating == true
          and .workspace == $workspace
        )
      ' <<< "$windows" > /dev/null; then
    break
  fi
  sleep 0.05
done
if ! grep -q '^content-type-updated$' "$REPLAY_LOG" \
    || ! jq -e --arg workspace "$source_workspace" '
      any(.[];
        .app_id == "content-type-replay"
        and .content_type == "game"
        and .floating == true
        and .workspace == $workspace
      )
    ' <<< "$windows" > /dev/null; then
  echo "late content type replayed an opening placement after title settlement: $windows; client log: $(< "$REPLAY_LOG")"
  exit 1
fi

echo "initial and late subsurface content types applied opening rules without replay; later replacement refreshed opacity only: green $before_green -> $after_green"
