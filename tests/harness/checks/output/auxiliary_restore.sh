#!/usr/bin/env bash
# harness: outputs=2
# Floating, pinned, and scratchpad windows keep their state and logical home
# when a physical output disappears. Run the same probes through scrolling,
# dwindle, and master workspaces so every layout participates in the recovery.
set -euo pipefail

readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}"
readonly UNMAP_CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly OUTPUT_LAYOUT_W=4480
readonly OUTPUT_LAYOUT_H=1440
readonly BASE_CONFIG="$(< "$UMBRIEL_CONFIG")"
readonly SCRATCHPAD=auxiliary

layouts=(scroll dwindle master)
titles=(scroll-anchor scroll-probe dwindle-anchor dwindle-probe master-anchor master-probe)
probes=(scroll-probe dwindle-probe master-probe)
anchors=(scroll-anchor dwindle-anchor master-anchor)

declare -A workspace_of=(
  [scroll-anchor]=1 [scroll-probe]=1
  [dwindle-anchor]=2 [dwindle-probe]=2
  [master-anchor]=3 [master-probe]=3
)

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_count() {
  local expected=$1 count=
  for _ in $(seq 80); do
    count=$("$UMBRIEL" windows --json | jq 'length')
    [[ $count == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected $expected windows, got $count"
  return 1
}

field_of() {
  "$UMBRIEL" windows --json \
    | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

home_of() {
  local workspace=
  workspace=$(field_of "$1" workspace)
  [[ -z $workspace ]] && return 0
  printf '%s/%s' "${workspace%%:*}" \
    "$("$WORKSPACE" --all | awk -F'\t' -v id="$workspace" '$1 == id { print $2 }')"
}

wait_for_home() {
  local title=$1 expected=$2 home=
  for _ in $(seq 80); do
    home=$(home_of "$title")
    [[ $home == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' on '$expected', got '$home'"
  return 1
}

wait_for_field() {
  local title=$1 field=$2 expected=$3 actual=
  for _ in $(seq 80); do
    actual=$(field_of "$title" "$field")
    [[ $actual == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' field '$field' to be '$expected', got '$actual'"
  return 1
}

output_origin() {
  "$UMBRIEL" outputs \
    | awk -v name="$1" '$1 == name { found = 1; next } found && /Position:/ { split($2, p, ","); print p[1], p[2]; exit }'
}

move_pointer_to_output() {
  local output=$1 output_x output_y
  read -r output_x output_y < <(output_origin "$output")
  if [[ -z ${output_x:-} || -z ${output_y:-} ]]; then
    echo "could not resolve origin for $output" >&2
    return 1
  fi
  "$POINTER" "$OUTPUT_LAYOUT_W" "$OUTPUT_LAYOUT_H" move "$((output_x + 640))" "$((output_y + 360))"
}

focus_window() {
  "$UMBRIEL" msg "window-focus-warp:$(field_of "$1" id)" > /dev/null
}

move_to_workspace() {
  local title=$1 workspace=$2
  focus_window "$title"
  "$UMBRIEL" msg "window-move-to-workspace:$workspace/HEADLESS-1" > /dev/null
  wait_for_home "$title" "HEADLESS-1/$workspace"
}

geometry_snapshot() {
  "$UMBRIEL" windows --json | jq -Sc '[.[] | {title, x, y, w, h, floating}] | sort_by(.title)'
}

anchor_snapshot() {
  "$UMBRIEL" windows --json \
    | jq -Sc '[.[] | select(.title | endswith("-anchor")) | {title, x, y, w, h, floating}] | sort_by(.title)'
}

wait_for_stable_snapshot() {
  local kind=$1 previous= current= stable=0
  for _ in $(seq 80); do
    if [[ $kind == all ]]; then
      current=$(geometry_snapshot)
    else
      current=$(anchor_snapshot)
    fi
    if [[ -n $current && $current == "$previous" ]]; then
      stable=$((stable + 1))
      if ((stable >= 3)); then
        printf '%s' "$current"
        return 0
      fi
    else
      stable=0
    fi
    previous=$current
    sleep 0.1
  done
  echo "$kind geometry did not settle: $current" >&2
  return 1
}

wait_for_snapshot() {
  local kind=$1 expected=$2 current=
  for _ in $(seq 80); do
    if [[ $kind == all ]]; then
      current=$(geometry_snapshot)
    else
      current=$(anchor_snapshot)
    fi
    [[ $current == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "$kind geometry did not return"
  echo "expected: $expected"
  echo "     got: $current"
  return 1
}

destroy_home_with_workspace_views() {
  "$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
  for title in "${titles[@]}"; do
    wait_for_home "$title" HEADLESS-2/1
  done
}

destroy_home_with_scratchpad_views() {
  "$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
  for title in "${anchors[@]}"; do
    wait_for_home "$title" HEADLESS-2/1
  done
  for title in "${probes[@]}"; do
    wait_for_home "$title" ''
  done
}

recreate_home() {
  local created=
  created=$("$UMBRIEL" output-create HEADLESS-1)
  if [[ $created != HEADLESS-1 ]]; then
    echo "expected recreated output HEADLESS-1, got '$created'"
    return 1
  fi
}

wait_for_workspace_homes() {
  local title
  for title in "$@"; do
    wait_for_home "$title" "HEADLESS-1/${workspace_of[$title]}"
  done
}

active_probe() {
  "$UMBRIEL" windows --json \
    | jq -r '[.[] | select(.active and (.title | endswith("-probe"))) | .title][0] // ""'
}

wait_for_active_probe() {
  local previous=${1:-} active=
  for _ in $(seq 80); do
    active=$(active_probe)
    if [[ -n $active && $active != "$previous" ]]; then
      printf '%s' "$active"
      return 0
    fi
    sleep 0.1
  done
  echo "scratchpad focus did not advance from '$previous'" >&2
  return 1
}

assert_visible_scratchpad_cycle() {
  local output=$1 active=
  declare -A seen=()
  for _ in 1 2 3; do
    if ! "$UMBRIEL" msg "scratchpad-focus-next:$SCRATCHPAD" > /dev/null; then
      echo "scratchpad on '$output' was not visible while cycling '$active'"
      return 1
    fi
    active=$(wait_for_active_probe "${active:-}")
    seen[$active]=1
  done
  if ((${#seen[@]} != 3)); then
    echo "expected all three probes in the visible '$output' scratchpad, saw ${!seen[*]}"
    return 1
  fi
}

write_matrix_config() {
  local first_enabled=$1 second_enabled=$2
  {
    printf '%s\n' "$BASE_CONFIG"
    cat <<EOF

[animation]
enabled = false

[output.HEADLESS-1]
enabled = $first_enabled
mode = "2560x1440"
position = [0, 0]
workspaces = 3

[output.HEADLESS-2]
enabled = $second_enabled
mode = "1920x1080"
position = [2560, 0]
workspaces = "dynamic"

[[scratchpad]]
name = "auxiliary"

[[workspace]]
output = "HEADLESS-1"
index = 1
layout.mode = "scrolling"

[[workspace]]
output = "HEADLESS-1"
index = 2
layout.mode = "dwindle"

[[workspace]]
output = "HEADLESS-1"
index = 3
layout.mode = "master"

[[window_rule]]
match.title = "^late-float$"
default_floating = true
EOF
  } > "$UMBRIEL_CONFIG"
}

write_matrix_config true true
"$UMBRIEL" msg config-reload > /dev/null

count=0
for layout in "${layouts[@]}"; do
  for role in anchor probe; do
    title=$layout-$role
    spawn_client "$title"
    count=$((count + 1))
    wait_for_count "$count"
    move_to_workspace "$title" "${workspace_of[$title]}"
  done
done
wait_for_workspace_homes "${titles[@]}"

# Ordinary floating views retain exact geometry and stay outside every layout.
for title in "${probes[@]}"; do
  focus_window "$title"
  "$UMBRIEL" msg window-toggle-floating > /dev/null
  wait_for_field "$title" floating true
  "$UMBRIEL" msg window-center > /dev/null
done
floating_before=$(wait_for_stable_snapshot all)
destroy_home_with_workspace_views
for title in "${probes[@]}"; do
  wait_for_field "$title" floating true
done
recreate_home
wait_for_workspace_homes "${titles[@]}"
wait_for_snapshot all "$floating_before"
for title in "${probes[@]}"; do
  focus_window "$title"
  "$UMBRIEL" msg window-toggle-floating > /dev/null
  wait_for_field "$title" floating false
done

# A pinned view remains pinned across recovery. One toggle after return must
# unpin and retile it, which distinguishes a retained pin from an ordinary
# floating view without adding a pin flag to the public windows response.
for title in "${probes[@]}"; do
  focus_window "$title"
  "$UMBRIEL" msg window-toggle-pinned > /dev/null
  wait_for_field "$title" floating true
  "$UMBRIEL" msg window-center > /dev/null
done
pinned_before=$(wait_for_stable_snapshot all)
destroy_home_with_workspace_views
for title in "${probes[@]}"; do
  wait_for_field "$title" floating true
done
recreate_home
wait_for_workspace_homes "${titles[@]}"
wait_for_snapshot all "$pinned_before"
for title in "${probes[@]}"; do
  focus_window "$title"
  "$UMBRIEL" msg window-toggle-pinned > /dev/null
  wait_for_field "$title" floating false
done

# Ordinary floating views stay workspace-owned while a separate scratchpad
# survives the loss of every output. Recreate the refuge first so the test
# observes the temporary home instead of skipping directly to the final one.
for title in "${probes[@]}"; do
  focus_window "$title"
  "$UMBRIEL" msg window-toggle-floating > /dev/null
  wait_for_field "$title" floating true
  "$UMBRIEL" msg window-center > /dev/null
done
spawn_client legit-scratch
count=$((count + 1))
wait_for_count "$count"
move_to_workspace legit-scratch 1
move_pointer_to_output HEADLESS-1
focus_window legit-scratch
"$UMBRIEL" msg "window-move-to-scratchpad:$SCRATCHPAD" > /dev/null
wait_for_home legit-scratch ''
"$UMBRIEL" msg "scratchpad-toggle:$SCRATCHPAD" > /dev/null

write_matrix_config false false
"$UMBRIEL" msg config-reload > /dev/null
for title in "${titles[@]}"; do
  wait_for_home "$title" ''
done
wait_for_home legit-scratch ''
"$UNMAP_CLIENT" late-float 800 500 > "$UMBRIEL_RUNTIME_DIR/late-float.log" 2>&1 &
count=$((count + 1))
wait_for_count "$count"
wait_for_home late-float ''
wait_for_field late-float floating true

write_matrix_config false true
"$UMBRIEL" msg config-reload > /dev/null
for title in "${titles[@]}"; do
  wait_for_home "$title" HEADLESS-2/1
done
for title in "${probes[@]}"; do
  wait_for_field "$title" floating true
done
wait_for_home late-float HEADLESS-2/1
wait_for_field late-float floating true
wait_for_home legit-scratch ''
"$UMBRIEL" msg "scratchpad-focus-next:$SCRATCHPAD" > /dev/null

write_matrix_config true true
"$UMBRIEL" msg config-reload > /dev/null
wait_for_workspace_homes "${titles[@]}"
for title in "${probes[@]}"; do
  wait_for_field "$title" floating true
done
wait_for_home legit-scratch ''
"$UMBRIEL" msg "scratchpad-focus-next:$SCRATCHPAD" > /dev/null
"$UMBRIEL" msg "window-close:$(field_of legit-scratch id)" > /dev/null
count=$((count - 1))
wait_for_count "$count"
"$UMBRIEL" msg "window-close:$(field_of late-float id)" > /dev/null
count=$((count - 1))
wait_for_count "$count"
for title in "${probes[@]}"; do
  focus_window "$title"
  "$UMBRIEL" msg window-toggle-floating > /dev/null
  wait_for_field "$title" floating false
done

scratchpad_phase() {
  local return_floating=$1 title scratch_anchor_before

  if [[ $return_floating == true ]]; then
    for title in "${probes[@]}"; do
      focus_window "$title"
      "$UMBRIEL" msg window-toggle-floating > /dev/null
      wait_for_field "$title" floating true
      "$UMBRIEL" msg window-center > /dev/null
    done
  fi

  move_pointer_to_output HEADLESS-1
  for title in "${probes[@]}"; do
    focus_window "$title"
    "$UMBRIEL" msg "window-move-to-scratchpad:$SCRATCHPAD" > /dev/null
    wait_for_home "$title" ''
    wait_for_field "$title" floating true
  done
  "$UMBRIEL" msg "scratchpad-toggle:$SCRATCHPAD" > /dev/null
  assert_visible_scratchpad_cycle HEADLESS-1
  scratch_anchor_before=$(wait_for_stable_snapshot anchors)

  destroy_home_with_scratchpad_views
  assert_visible_scratchpad_cycle HEADLESS-2

  recreate_home
  wait_for_workspace_homes "${anchors[@]}"
  for title in "${probes[@]}"; do
    wait_for_home "$title" ''
  done
  wait_for_snapshot anchors "$scratch_anchor_before"
  assert_visible_scratchpad_cycle HEADLESS-1

  for title in "${probes[@]}"; do
    focus_window "$title"
    "$UMBRIEL" msg "window-restore-from-scratchpad:$SCRATCHPAD" > /dev/null
    wait_for_home "$title" "HEADLESS-1/${workspace_of[$title]}"
    wait_for_field "$title" floating "$return_floating"
  done

  if [[ $return_floating == true ]]; then
    for title in "${probes[@]}"; do
      focus_window "$title"
      "$UMBRIEL" msg window-toggle-floating > /dev/null
      wait_for_field "$title" floating false
    done
  fi
}

# Scratchpad presentation is visible while it moves to the refuge and back.
# Restoring each entry must recover both its source workspace and its original
# tiled or floating state.
scratchpad_phase false
scratchpad_phase true

wait_for_workspace_homes "${titles[@]}"
echo "floating, pinned, and scratchpad state survived output loss in scrolling, dwindle, and master layouts"
