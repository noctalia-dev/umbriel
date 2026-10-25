# Focus-model fixtures use real X11 clients and a separate XTEST sender for native Wayland typing observations.
source "$UMBRIEL_REPO/tests/harness/eis_lib.sh"

readonly X_FOCUS_CLIENT="${UMBRIEL_XWAYLAND_FOCUS_CLIENT:-./build-debug/tests/xwayland-focus-client}"
declare -A X_FOCUS_FDS=() X_FOCUS_PIDS=() X_FOCUS_LOGS=()

x_focus_start() {
  local id=$1 title=$2 model=$3 fd
  local fifo="$UMBRIEL_RUNTIME_DIR/focus-$id.fifo"
  X_FOCUS_LOGS[$id]="$UMBRIEL_RUNTIME_DIR/focus-$id.log"
  mkfifo "$fifo"
  exec {fd}<> "$fifo"
  X_FOCUS_FDS[$id]=$fd
  "$X_FOCUS_CLIENT" "$title" "$model" <&"$fd" > "${X_FOCUS_LOGS[$id]}" 2>&1 &
  X_FOCUS_PIDS[$id]=$!
  await_events "${X_FOCUS_LOGS[$id]}" ready 1 "$title X11 connection"
  eis_wait_window "$title"
  "$UMBRIEL" settle > /dev/null
}

x_focus_send() {
  local id=$1
  shift
  printf '%s\n' "$@" >&"${X_FOCUS_FDS[$id]}"
}

x_focus_stop() {
  local id=$1
  x_focus_send "$id" quit
  wait "${X_FOCUS_PIDS[$id]}"
  local fd=${X_FOCUS_FDS[$id]}
  exec {fd}>&-
}

x_focus_native_key() {
  local title=$1 log=$2 key=$3
  local before
  before=$(events "$log" "keyboard-key code=$key state=pressed")
  eis_send sender "tap $key"
  await_events "$log" "keyboard-key code=$key state=pressed" "$((before + 1))" "$title typing after focus negotiation"
  eis_wait_active "$title"
}

x_focus_id() {
  eis_window "$1" | jq -r .id
}

x_focus_wait_unmapped() {
  local title=$1
  for _ in $(seq 60); do
    [[ -z $(eis_window "$title") ]] && return 0
    sleep 0.05
  done
  echo "X11 fixture $title did not unmap: $("$UMBRIEL" windows --json)"
  return 1
}

x_focus_fixture_config() {
  cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[animation]
enabled = false

[layout.scrolling]
default_extent_fraction = 1.0

[input.focus]
follows_mouse = true

[[window_rule]]
match.title = "^focus-model-"
default_floating = true
default_floating_size_px = { width = 480, height = 360 }
default_position = { x = 780, y = 100, anchor = "top_left" }
CONFIG
  "$UMBRIEL" msg config-reload > /dev/null
}
