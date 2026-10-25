# Real XTEST connections and native input observations shared by the EIS integration checks.
source "$UMBRIEL_HARNESS_LIB"

readonly XTEST_CLIENT="${UMBRIEL_XWAYLAND_INPUT_CLIENT:-./build-debug/tests/xwayland-input-client}"
readonly EI_PEER_CLIENT="${UMBRIEL_EI_PEER_CLIENT:-./build-debug/tests/ei-peer-client}"
readonly NATIVE_CLIENT="${UMBRIEL_SEAT_LOG_CLIENT:-./build-debug/tests/seat-log-client}"
declare -A EIS_INPUT_FDS=() EIS_INPUT_PIDS=() EIS_INPUT_LOGS=()

eis_start() {
  local id=$1 name=$2 fd
  local fifo="$UMBRIEL_RUNTIME_DIR/xtest-$id.fifo"
  EIS_INPUT_LOGS[$id]="$UMBRIEL_RUNTIME_DIR/xtest-$id.log"
  mkfifo "$fifo"
  exec {fd}<> "$fifo"
  EIS_INPUT_FDS[$id]=$fd
  bash -c 'exec -a "$1" "$2"' _ "$name" "$XTEST_CLIENT" <&"$fd" > "${EIS_INPUT_LOGS[$id]}" 2>&1 &
  EIS_INPUT_PIDS[$id]=$!
  await_events "${EIS_INPUT_LOGS[$id]}" ready 1 "XTEST client $name"
}

eis_send() {
  local id=$1
  shift
  printf '%s\n' "$@" >&"${EIS_INPUT_FDS[$id]}"
}

eis_sync() {
  local id=$1 label=$2
  eis_send "$id" "sync $label"
  await_events "${EIS_INPUT_LOGS[$id]}" "sync $label" 1 "XTEST client $id"
}

eis_stop() {
  local id=$1
  eis_send "$id" quit
  wait "${EIS_INPUT_PIDS[$id]}"
  local fd=${EIS_INPUT_FDS[$id]}
  exec {fd}>&-
}

eis_permission() {
  local decision=$1 name=$2 count=${3:-1}
  await_events "$UMBRIEL_LOG" ".*input permission $decision .*app='$name'" "$count" "$name permission $decision"
}

eis_approve() {
  local id=$1 name=$2
  eis_send "$id" 'tap 30'
  eis_permission requested "$name"
  "$UMBRIEL_POINTER_CLIENT" 1280 720 keyboard-only tap 28 > /dev/null
  eis_permission granted "$name"
}

eis_window() {
  "$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)'
}

eis_wait_window() {
  local title=$1
  for _ in $(seq 60); do
    [[ -n $(eis_window "$title") ]] && return 0
    sleep 0.05
  done
  echo "native window $title never mapped: $("$UMBRIEL" windows --json)"
  return 1
}

eis_wait_active() {
  local title=$1
  for _ in $(seq 60); do
    [[ $(eis_window "$title" | jq -r .active) == true ]] && return 0
    sleep 0.05
  done
  echo "native window $title never became active: $("$UMBRIEL" windows --json)"
  return 1
}

# Find only Xwayland's inherited endpoint in this check's process group. The compositor may double-fork Xwayland,
# so direct child enumeration is insufficient; no process or socket from the user's session is considered.
eis_socket() {
  python3 - "$UMBRIEL_RUNTIME_DIR/instance.pgid" <<'PY'
import pathlib
import sys

group = int(pathlib.Path(sys.argv[1]).read_text())
for process in pathlib.Path('/proc').iterdir():
    if not process.name.isdigit():
        continue
    try:
        stat = (process / 'stat').read_text()
        fields = stat[stat.rfind(')') + 2:].split()
        if int(fields[2]) != group:
            continue
        for entry in (process / 'environ').read_bytes().split(b'\0'):
            if entry.startswith(b'LIBEI_SOCKET='):
                print(entry.split(b'=', 1)[1].decode())
                sys.exit(0)
    except (OSError, ValueError):
        continue
sys.exit('private Xwayland LIBEI_SOCKET was not advertised')
PY
}
