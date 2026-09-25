# Helpers shared by harness checks. A check sources it with: source "$UMBRIEL_HARNESS_LIB"

# Runs the pointer helper through a sequence and holds the resulting input state (a pressed button, a held modifier)
# until pointer_release: pointer_hold <width> <height> <command>... [-- <command>...]. It returns once every command
# before `--` has been processed; the commands after `--` run on release. One hold at a time.
pointer_hold() {
  local width=$1 height=$2
  shift 2
  local -a before=()
  while (($# > 0)) && [[ $1 != -- ]]; do
    before+=("$1")
    shift
  done
  (($# > 0)) && shift
  local fifo=$UMBRIEL_RUNTIME_DIR/pointer-hold.fifo
  POINTER_HOLD_LOG=$UMBRIEL_RUNTIME_DIR/pointer-hold.log
  rm -f "$fifo"
  mkfifo "$fifo"
  "$UMBRIEL_POINTER_CLIENT" "$width" "$height" "${before[@]}" mark held hold "$@" < "$fifo" > "$POINTER_HOLD_LOG" 2>&1 &
  POINTER_HOLD_PID=$!
  exec {POINTER_HOLD_FD}> "$fifo"
  for _ in $(seq 200); do
    grep -q '^held$' "$POINTER_HOLD_LOG" && return 0
    sleep 0.025
  done
  echo "the pointer helper never reached its hold: $(< "$POINTER_HOLD_LOG")"
  return 1
}

# Several holds in one run: put `mark <label> hold` among the commands after `--`, and pointer_step <label> runs the
# helper on to that hold.
pointer_step() {
  echo >&"$POINTER_HOLD_FD"
  for _ in $(seq 200); do
    grep -q "^$1\$" "$POINTER_HOLD_LOG" && return 0
    sleep 0.025
  done
  echo "the pointer helper never reached $1: $(< "$POINTER_HOLD_LOG")"
  return 1
}

# Runs the remaining commands after `--` and waits for the helper to exit.
pointer_release() {
  echo >&"$POINTER_HOLD_FD"
  exec {POINTER_HOLD_FD}>&-
  wait "$POINTER_HOLD_PID"
}

# The window observer suffixes its enter with the held-key count, so events are matched as line prefixes.
events() {
  local count
  count=$(grep -c "^$2" "$1" 2>/dev/null) || true
  echo "${count:-0}"
}

await_events() {
  local file=$1 event=$2 expected=$3 label=${4:-$1}
  for _ in $(seq 60); do
    (($(events "$file" "$event") >= expected)) && return 0
    sleep 0.1
  done
  echo "timed out waiting for $expected '$event' on $label: $(tr '\n' '|' < "$file")"
  return 1
}
