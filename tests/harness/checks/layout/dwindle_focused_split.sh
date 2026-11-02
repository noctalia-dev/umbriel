#!/usr/bin/env bash
# A new Dwindle window splits the focused leaf, not the next leaf in flat order. With new_toward_cursor, it takes the
# half of that leaf on the pointer's side instead of the right or bottom half.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"

spawn_client() {
  local title=$1
  "$CLIENT" "$title" 1200 700 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

wait_for_focus() {
  local want=$1
  for _ in $(seq 40); do
    if [[ $("$UMBRIEL" windows --json | jq -r --arg id "$want" '.[] | select(.id == $id) | .focused') == true ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $want to be focused: $("$UMBRIEL" windows --json)"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
mode = "dwindle"

[animation]
duration_ms = 1
curve = "linear"
EOF
"$UMBRIEL" msg config-reload > /dev/null

spawn_client dwindle-focused-a
wait_for_count 1
spawn_client dwindle-focused-b
wait_for_count 2
spawn_client dwindle-focused-c
wait_for_count 3
sleep 0.1

windows=$("$UMBRIEL" windows --json)
a_id=$(jq -r '.[] | select(.title == "dwindle-focused-a") | .id' <<< "$windows")
a_y=$(jq -r '.[] | select(.title == "dwindle-focused-a") | .y' <<< "$windows")
b_x=$(jq -r '.[] | select(.title == "dwindle-focused-b") | .x' <<< "$windows")
if [[ -z $a_id || -z $a_y || -z $b_x ]]; then
  echo "could not resolve initial Dwindle geometry: $windows"
  exit 1
fi

"$UMBRIEL" msg window-focus "$a_id" > /dev/null
wait_for_focus "$a_id"
spawn_client dwindle-focused-d
wait_for_count 4

for _ in $(seq 40); do
  windows=$("$UMBRIEL" windows --json)
  d_x=$(jq -r '.[] | select(.title == "dwindle-focused-d") | .x' <<< "$windows")
  d_y=$(jq -r '.[] | select(.title == "dwindle-focused-d") | .y' <<< "$windows")
  if [[ $d_x != null && $d_x -lt $b_x && $d_y -gt $a_y ]]; then
    break
  fi
  sleep 0.1
done

if [[ $d_x == null || $d_x -ge $b_x || $d_y -le $a_y ]]; then
  echo "new window did not split the focused leaf: $windows"
  exit 1
fi

echo "Dwindle splits the focused window's leaf"

# A over D beside B over C on the 1280x720 output: B is a landscape tile, so its split is side by side and the default
# puts the new window right of it.
b_id=$(jq -r '.[] | select(.title == "dwindle-focused-b") | .id' <<< "$windows")
b_y=$(jq -r '.[] | select(.title == "dwindle-focused-b") | .y' <<< "$windows")
if [[ -z $b_id || -z $b_y ]]; then
  echo "could not resolve the upper-right tile: $windows"
  exit 1
fi
cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout.dwindle]
new_toward_cursor = true
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$POINTER" 1280 720 move "$((b_x + 20))" "$((b_y + 20))" > "$UMBRIEL_RUNTIME_DIR/pointer.log" 2>&1
"$UMBRIEL" msg window-focus "$b_id" > /dev/null
wait_for_focus "$b_id"
spawn_client dwindle-focused-e
wait_for_count 5

for _ in $(seq 40); do
  windows=$("$UMBRIEL" windows --json)
  e_x=$(jq -r '.[] | select(.title == "dwindle-focused-e") | .x' <<< "$windows")
  e_y=$(jq -r '.[] | select(.title == "dwindle-focused-e") | .y' <<< "$windows")
  new_b_x=$(jq -r '.[] | select(.title == "dwindle-focused-b") | .x' <<< "$windows")
  if [[ $e_x != null && $e_x -lt $new_b_x && $e_y -eq $b_y ]]; then
    break
  fi
  sleep 0.1
done

if [[ $e_x == null || $e_x -ge $new_b_x || $e_y -ne $b_y ]]; then
  echo "new window did not open on the pointer's side of the focused leaf: $windows"
  exit 1
fi

echo "Dwindle opens a new window toward the pointer with new_toward_cursor"
