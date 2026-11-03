#!/usr/bin/env bash
# A background_render rule keeps a hidden window unsuspended and paces its frame callbacks at the output refresh rate,
# while a window it does not match is suspended without callbacks. Reloading the rule away suspends the window too.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[[window_rule]]
match.title = "^rendered$"
background_render = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$UMBRIEL_RUNTIME_DIR/rendered.fifo"
# LOG_SUSPENDED binds xdg-shell version 6, so both clients can be told they are suspended.
LOG_SUSPENDED=1 REPAINT_ON_STDIN=1 "$CLIENT" rendered 400 300 \
  < "$UMBRIEL_RUNTIME_DIR/rendered.fifo" > "$UMBRIEL_RUNTIME_DIR/rendered.log" 2>&1 &
exec 3> "$UMBRIEL_RUNTIME_DIR/rendered.fifo"
LOG_SUSPENDED=1 "$CLIENT" control 400 300 > "$UMBRIEL_RUNTIME_DIR/control.log" 2>&1 &

for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') == 2 ]] && break
  sleep 0.1
done
"$UMBRIEL" settle

last_state() {
  local state
  state=$(grep -E '^(suspended|resumed)$' "$UMBRIEL_RUNTIME_DIR/$1.log" | tail -n 1) || true
  echo "${state:-resumed}"
}

wait_for_state() {
  for _ in $(seq 50); do
    [[ $(last_state "$1") == "$2" ]] && return 0
    sleep 0.1
  done
  echo "expected $1 to be $2 after $3, got $(last_state "$1"): $(< "$UMBRIEL_RUNTIME_DIR/$1.log")"
  exit 1
}

repaints() {
  grep -c '^repainted ' "$UMBRIEL_RUNTIME_DIR/rendered.log" || true
}

"$UMBRIEL" msg workspace-switch 2 > /dev/null
"$UMBRIEL" settle
# The control proves the suspension pass ran; the ruled window must have stayed out of it.
wait_for_state control suspended "leaving their workspace"
if grep -qx suspended "$UMBRIEL_RUNTIME_DIR/rendered.log"; then
  echo "the background_render window was suspended while hidden: $(< "$UMBRIEL_RUNTIME_DIR/rendered.log")"
  exit 1
fi

# Ten frame callbacks one after another take 1 s at the 10 Hz keepalive and a sixth of that at 60 Hz.
start=${EPOCHREALTIME/[.,]/}
for _ in $(seq 10); do
  before=$(repaints)
  printf R >&3
  until (($(repaints) > before)); do
    if (((${EPOCHREALTIME/[.,]/} - start) > 3000000)); then
      echo "the hidden background_render window stopped receiving frame callbacks: $(repaints) repaints"
      exit 1
    fi
    sleep 0.005
  done
done
elapsed_ms=$(((${EPOCHREALTIME/[.,]/} - start) / 1000))
if ((elapsed_ms >= 800)); then
  echo "ten hidden frame callbacks took ${elapsed_ms} ms, no faster than the keepalive"
  exit 1
fi

sed -i 's/^background_render = true$/background_render = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
wait_for_state rendered suspended "the rule was reloaded away"

echo "the background_render window stayed unsuspended with ten hidden callbacks in ${elapsed_ms} ms," \
  "the control was suspended, and reloading the rule away suspended it"
