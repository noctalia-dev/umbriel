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

# Waits for at least $3 lines matching $2 in $1, then returns. Poll step is $4 (default 0.1s), matching
# await_events; a check whose waits are tight under parallel stress passes a shorter step.
await_lines() {
  local file=$1 pattern=$2 expected=$3 step=${4:-0.1}
  for _ in $(seq 60); do
    (($(events "$file" "$pattern") >= expected)) && return 0
    sleep "$step"
  done
  echo "timed out waiting for $expected '$pattern' in $file: $(tr '\n' '|' < "$file")"
  return 1
}

# Shared fixture for live workspace reveal checks.
workspace_reveal_config() {
  cp "$UMBRIEL_REPO/examples/effects/animation/workspace_wipe/shader.glsl" "$UMBRIEL_RUNTIME_DIR/wipe.glsl"
  cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = true
[animation.windows_in]
enabled = false
[animation.windows_out]
enabled = false
[animation.windows_move]
enabled = false
[animation.workspaces]
style = "reveal"
effect = "wipe"
duration_ms = 1000
curve = "linear"
[effects.preset.wipe]
kind = "animation"
shader = "wipe.glsl"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
CONFIG
}

# Query a mapped window by its exact title; wait for it before reading its state.
window_by_title() { "$UMBRIEL" windows --json | jq -ce --arg title "$1" '.[] | select(.title == $title)'; }
await_window() {
  for _ in $(seq 100); do
    window_by_title "$1" > /dev/null && return
    sleep 0.025
  done
  echo "missing window: $1"; return 1
}

# Plain colours make capture pixels independent of decorations and animation.
capture_config() {
  cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.title_bar]
mode = "off"
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = false
CONFIG
}

# Two repaintable windows under an opaque magenta window. FDs 8/9 control
# the lower clients; each check supplies its own layout and protection rules.
capture_clients() {
  mkfifo "$UMBRIEL_RUNTIME_DIR/left" "$UMBRIEL_RUNTIME_DIR/right"
  exec 8<> "$UMBRIEL_RUNTIME_DIR/left"
  exec 9<> "$UMBRIEL_RUNTIME_DIR/right"
  REPAINT_ON_STDIN=1 FOLLOW_CONFIGURES=1 FILL_COLOR=0xFF00FF00 \
    "$UMBRIEL_UNMAP_CLIENT" left "$1" "$1" <&8 > "$UMBRIEL_RUNTIME_DIR/left.log" 2>&1 &
  await_window left
  REPAINT_ON_STDIN=1 FOLLOW_CONFIGURES=1 FILL_COLOR=0xFF0000FF \
    "$UMBRIEL_UNMAP_CLIENT" right "$1" "$1" <&9 > "$UMBRIEL_RUNTIME_DIR/right.log" 2>&1 &
  await_window right
  OPAQUE_BUFFER=1 FOLLOW_CONFIGURES=1 FILL_COLOR=0xFFFF00FF \
    "$UMBRIEL_UNMAP_CLIENT" protected 1280 720 > "$UMBRIEL_RUNTIME_DIR/protected.log" 2>&1 &
  await_window protected
}

# Save the frame for subsequent colour/geometry assertions as well.
capture_assert_hidden() {
  grim "$1"
  local count
  count=$("$UMBRIEL_PIXEL_PROBE" "$1" count 'r>0.9&&b>0.9&&g<0.1')
  [[ $count == 0 ]] || { echo "protected pixels leaked: $count"; return 1; }
}

# The first protection field belongs to the magenta window. Later rules stay
# unchanged so feedback tests can keep another owner protected throughout.
capture_set_protection() {
  sed -i "0,/^block_capture = /s/^block_capture = .*/block_capture = $1/" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}
