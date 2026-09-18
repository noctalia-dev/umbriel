#!/usr/bin/env bash
# harness: outputs=2
# Exact tiled layout state must survive physical output loss for every layout
# mode, even while the windows temporarily share a populated dynamic output.
set -euo pipefail

readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}"
readonly UNMAP_CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

spawn_client() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
}

spawn_remap_client() {
  remap_log="$UMBRIEL_RUNTIME_DIR/remap-client.log"
  remap_control="$UMBRIEL_RUNTIME_DIR/remap-client-control"
  mkfifo "$remap_control"
  exec {remap_fd}<>"$remap_control"
  REMAP_ON_STDIN=1 "$UNMAP_CLIENT" "$1" <&"$remap_fd" > "$remap_log" 2>&1 &
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
  "$UMBRIEL" windows --json | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
}

home_of() {
  local workspace=
  workspace=$(field_of "$1" workspace)
  [[ -z $workspace ]] && return 0
  printf '%s/%s' "${workspace%%:*}" "$("$WORKSPACE" --all | awk -F'\t' -v id="$workspace" '$1 == id { print $2 }')"
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

focus_window() {
  "$UMBRIEL" msg "window-focus:$(field_of "$1" id)" > /dev/null
}

move_to_workspace() {
  local title=$1 workspace=$2 output=$3
  focus_window "$title"
  "$UMBRIEL" msg "window-move-to-workspace:$workspace/$output" > /dev/null
  wait_for_home "$title" "$output/$workspace"
}

wait_for_windows_query() {
  local query=$1 message=$2 windows=
  for _ in $(seq 80); do
    windows=$("$UMBRIEL" windows --json)
    if jq -e "$query" > /dev/null <<< "$windows"; then
      return 0
    fi
    sleep 0.1
  done
  echo "$message: $windows"
  return 1
}

wait_for_width_change() {
  local title=$1 previous=$2 width=
  for _ in $(seq 80); do
    width=$(field_of "$title" w)
    [[ -n $width && $width != "$previous" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' width to change from '$previous', got '$width'"
  return 1
}

wait_for_width() {
  local title=$1 expected=$2 width=
  for _ in $(seq 80); do
    width=$(field_of "$title" w)
    [[ $width == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected '$title' width '$expected', got '$width'"
  return 1
}

geometry_snapshot() {
  "$UMBRIEL" windows --json | jq -Sc '[.[] | {title, x, y, w, h}] | sort_by(.title)'
}

wait_for_stable_geometry() {
  local previous= current= stable=0
  for _ in $(seq 80); do
    current=$(geometry_snapshot)
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
  echo "window geometry did not settle: $current" >&2
  return 1
}

wait_for_geometry_snapshot() {
  local expected=$1 current= stable=0
  for _ in $(seq 80); do
    current=$(geometry_snapshot)
    if [[ $current == "$expected" ]]; then
      stable=$((stable + 1))
      ((stable >= 3)) && return 0
    else
      stable=0
    fi
    sleep 0.1
  done
  echo "layout geometry did not return to its stable snapshot"
  echo "expected: $expected"
  echo "     got: $current"
  return 1
}

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1

[output.HEADLESS-1]
mode = "2560x1440"
position = [0, 0]
workspaces = 3

[output.HEADLESS-2]
mode = "1920x1080"
position = [2560, 0]
workspaces = "dynamic"

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

[[workspace]]
output = "HEADLESS-2"
index = 1
layout.mode = "scrolling"
EOF
"$UMBRIEL" msg config-reload > /dev/null

h1_titles=(scroll-a scroll-b scroll-c dwindle-a dwindle-b dwindle-c master-a master-b master-c)
mapped_h1_titles=(scroll-b scroll-c dwindle-a dwindle-b dwindle-c master-a master-b master-c)
h2_layout_titles=(refuge-scroll-a refuge-scroll-b refuge-scroll-c)
h2_survivor_titles=(refuge-ws2 refuge-ws3)
titles=("${h1_titles[@]}" "${h2_layout_titles[@]}" "${h2_survivor_titles[@]}")

declare -A expected_home=()
for title in scroll-a scroll-b scroll-c; do
  expected_home[$title]=HEADLESS-1/1
done
for title in dwindle-a dwindle-b dwindle-c; do
  expected_home[$title]=HEADLESS-1/2
done
for title in master-a master-b master-c; do
  expected_home[$title]=HEADLESS-1/3
done
for title in "${h2_layout_titles[@]}"; do
  expected_home[$title]=HEADLESS-2/1
done
expected_home[refuge-ws2]=HEADLESS-2/2
expected_home[refuge-ws3]=HEADLESS-2/3

count=0
for title in "${h1_titles[@]}"; do
  if [[ $title == scroll-a ]]; then
    spawn_remap_client "$title"
  else
    spawn_client "$title"
  fi
  count=$((count + 1))
  wait_for_count "$count"
  case $title in
    scroll-*) move_to_workspace "$title" 1 HEADLESS-1 ;;
    dwindle-*) move_to_workspace "$title" 2 HEADLESS-1 ;;
    master-*) move_to_workspace "$title" 3 HEADLESS-1 ;;
  esac
done

# Scrolling: reorder columns, build a reversed stack, and keep a maximized
# column with a nondefault restore width.
"$UMBRIEL" msg workspace-switch:1/HEADLESS-1 > /dev/null
focus_window scroll-c
"$UMBRIEL" msg column-move-to-first > /dev/null
focus_window scroll-b
"$UMBRIEL" msg window-consume-left > /dev/null
wait_for_windows_query '
  first(.[] | select(.title == "scroll-a")) as $a |
  first(.[] | select(.title == "scroll-b")) as $b |
  first(.[] | select(.title == "scroll-c")) as $c |
  $c.x < $a.x and $a.x == $b.x and $a.y < $b.y
' "scrolling consume did not build c,[a,b]"
"$UMBRIEL" msg window-move-up > /dev/null
wait_for_windows_query '
  first(.[] | select(.title == "scroll-a")) as $a |
  first(.[] | select(.title == "scroll-b")) as $b |
  first(.[] | select(.title == "scroll-c")) as $c |
  $c.x < $a.x and $a.x == $b.x and $b.y < $a.y
' "scrolling move-up did not build c,[b,a]"
focus_window scroll-c
wait_for_stable_geometry > /dev/null
scroll_default_width=$(field_of scroll-c w)
"$UMBRIEL" msg window-set-primary-extent:0.67 > /dev/null
wait_for_width_change scroll-c "$scroll_default_width"
wait_for_windows_query '
  first(.[] | select(.title == "scroll-a")) as $a |
  first(.[] | select(.title == "scroll-b")) as $b |
  first(.[] | select(.title == "scroll-c")) as $c |
  $c.x < $a.x and $a.x == $b.x and $b.y < $a.y and $c.w > $a.w
' "scrolling width or topology did not reach the requested state"
wait_for_stable_geometry > /dev/null
scroll_unmax_width=$(field_of scroll-c w)
"$UMBRIEL" msg window-toggle-maximize > /dev/null
wait_for_width_change scroll-c "$scroll_unmax_width"

# Dwindle: change leaf assignment and a split ratio, so flat insertion order
# cannot reproduce the same tree geometry.
"$UMBRIEL" msg workspace-switch:2/HEADLESS-1 > /dev/null
focus_window dwindle-c
wait_for_windows_query '
  first(.[] | select(.title == "dwindle-a")) as $a |
  first(.[] | select(.title == "dwindle-b")) as $b |
  first(.[] | select(.title == "dwindle-c")) as $c |
  $a.x < $c.x and $b.x == $c.x and $b.y < $c.y
' "dwindle setup did not form one left tile and two right tiles"
"$UMBRIEL" msg column-move-left > /dev/null
wait_for_windows_query '
  first(.[] | select(.title == "dwindle-a")) as $a |
  first(.[] | select(.title == "dwindle-b")) as $b |
  first(.[] | select(.title == "dwindle-c")) as $c |
  $c.x < $a.x and $a.x == $b.x and $b.y < $a.y
' "dwindle column move did not put c in the left leaf"
wait_for_stable_geometry > /dev/null
dwindle_default_width=$(field_of dwindle-c w)
"$UMBRIEL" msg window-set-primary-extent:0.67 > /dev/null
wait_for_width_change dwindle-c "$dwindle_default_width"
wait_for_windows_query '
  first(.[] | select(.title == "dwindle-a")) as $a |
  first(.[] | select(.title == "dwindle-b")) as $b |
  first(.[] | select(.title == "dwindle-c")) as $c |
  $c.x < $a.x and $a.x == $b.x and $b.y < $a.y and $c.w > $a.w
' "dwindle split ratio or topology did not reach the requested state"

# Master: put two windows in the master area, wait for that arrangement before
# the target-based vertical action, then reverse their rows and retain a
# nondefault width beneath maximized state.
"$UMBRIEL" msg workspace-switch:3/HEADLESS-1 > /dev/null
focus_window master-c
wait_for_windows_query '
  first(.[] | select(.title == "master-a")) as $a |
  first(.[] | select(.title == "master-b")) as $b |
  first(.[] | select(.title == "master-c")) as $c |
  $a.x < $c.x and $b.x == $c.x and $c.y < $b.y
' "master setup did not form one master and two stack rows"
"$UMBRIEL" msg window-consume-left > /dev/null
wait_for_windows_query '
  first(.[] | select(.title == "master-a")) as $a |
  first(.[] | select(.title == "master-b")) as $b |
  first(.[] | select(.title == "master-c")) as $c |
  $a.x == $c.x and $a.y < $c.y and $c.x < $b.x
' "master consume did not arrange [a,c] before move-up"
"$UMBRIEL" msg window-move-up > /dev/null
wait_for_windows_query '
  first(.[] | select(.title == "master-a")) as $a |
  first(.[] | select(.title == "master-b")) as $b |
  first(.[] | select(.title == "master-c")) as $c |
  $a.x == $c.x and $c.y < $a.y and $c.x < $b.x
' "master move-up did not arrange [c,a]"
wait_for_stable_geometry > /dev/null
master_default_width=$(field_of master-c w)
"$UMBRIEL" msg window-set-primary-extent:0.70 > /dev/null
wait_for_width_change master-c "$master_default_width"
wait_for_windows_query '
  first(.[] | select(.title == "master-a")) as $a |
  first(.[] | select(.title == "master-b")) as $b |
  first(.[] | select(.title == "master-c")) as $c |
  $a.x == $c.x and $c.y < $a.y and $c.x < $b.x and $c.w > $b.w
' "master width or topology did not reach the requested state"
wait_for_stable_geometry > /dev/null
master_unmax_width=$(field_of master-c w)
"$UMBRIEL" msg window-toggle-maximize > /dev/null
wait_for_width_change master-c "$master_unmax_width"

# HEADLESS-2 is a populated dynamic refuge, not an empty fixed workspace. Its
# first workspace owns a nontrivial scrolling layout, while workspaces 2 and 3
# stay alive through their own survivor windows.
for title in "${h2_layout_titles[@]}"; do
  spawn_client "$title"
  count=$((count + 1))
  wait_for_count "$count"
  move_to_workspace "$title" 1 HEADLESS-2
done
for workspace in 2 3; do
  title=refuge-ws$workspace
  spawn_client "$title"
  count=$((count + 1))
  wait_for_count "$count"
  move_to_workspace "$title" "$workspace" HEADLESS-2
done

"$UMBRIEL" msg workspace-switch:1/HEADLESS-2 > /dev/null
focus_window refuge-scroll-c
"$UMBRIEL" msg column-move-to-first > /dev/null
focus_window refuge-scroll-b
"$UMBRIEL" msg window-consume-left > /dev/null
wait_for_windows_query '
  first(.[] | select(.title == "refuge-scroll-a")) as $a |
  first(.[] | select(.title == "refuge-scroll-b")) as $b |
  first(.[] | select(.title == "refuge-scroll-c")) as $c |
  $c.x < $a.x and $a.x == $b.x and $a.y < $b.y
' "refuge scrolling consume did not build c,[a,b]"
"$UMBRIEL" msg window-move-up > /dev/null
wait_for_windows_query '
  first(.[] | select(.title == "refuge-scroll-a")) as $a |
  first(.[] | select(.title == "refuge-scroll-b")) as $b |
  first(.[] | select(.title == "refuge-scroll-c")) as $c |
  $c.x < $a.x and $a.x == $b.x and $b.y < $a.y
' "refuge scrolling move-up did not build c,[b,a]"
focus_window refuge-scroll-c
wait_for_stable_geometry > /dev/null
refuge_default_width=$(field_of refuge-scroll-c w)
"$UMBRIEL" msg window-set-primary-extent:0.63 > /dev/null
wait_for_width_change refuge-scroll-c "$refuge_default_width"
wait_for_windows_query '
  first(.[] | select(.title == "refuge-scroll-a")) as $a |
  first(.[] | select(.title == "refuge-scroll-b")) as $b |
  first(.[] | select(.title == "refuge-scroll-c")) as $c |
  $c.x < $a.x and $a.x == $b.x and $b.y < $a.y and $c.w > $a.w
' "refuge scrolling width or topology did not reach the requested state"

# Make focus history disagree with the layout orders. Finish on HEADLESS-2
# workspace 1 so every HEADLESS-1 view is evacuated into that populated refuge.
for title in scroll-c scroll-b scroll-a dwindle-c dwindle-b dwindle-a master-c master-b master-a; do
  focus_window "$title"
done
for title in refuge-scroll-c refuge-scroll-b refuge-scroll-a; do
  focus_window "$title"
done

for title in "${titles[@]}"; do
  wait_for_home "$title" "${expected_home[$title]}"
done
before=$(wait_for_stable_geometry)
remap_id=$(field_of scroll-a id)

# Remove both physical outputs. HEADLESS-1 first shares the populated dynamic
# workspace 1 on HEADLESS-2. Recreate the refuge first, then the original home,
# matching the order seen during VT and suspend recovery.
"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
for title in "${h1_titles[@]}"; do
  wait_for_home "$title" HEADLESS-2/1
done
"$UMBRIEL" msg "window-close:$remap_id" > /dev/null
wait_for_count "$((count - 1))"
if ! grep -q '^unmapped$' "$remap_log"; then
  echo "scroll-a did not stay alive and unmap while displaced"
  exit 1
fi
for title in "${h2_layout_titles[@]}" "${h2_survivor_titles[@]}"; do
  wait_for_home "$title" "${expected_home[$title]}"
done

"$UMBRIEL" output-destroy HEADLESS-2 > /dev/null
for title in "${titles[@]}"; do
  wait_for_home "$title" ''
done

created=$("$UMBRIEL" output-create HEADLESS-2)
if [[ $created != HEADLESS-2 ]]; then
  echo "expected refuge output HEADLESS-2, got '$created'"
  exit 1
fi
for title in "${mapped_h1_titles[@]}"; do
  wait_for_home "$title" HEADLESS-2/1
done
for title in "${h2_layout_titles[@]}" "${h2_survivor_titles[@]}"; do
  wait_for_home "$title" "${expected_home[$title]}"
done

created=$("$UMBRIEL" output-create HEADLESS-1)
if [[ $created != HEADLESS-1 ]]; then
  echo "expected home output HEADLESS-1, got '$created'"
  exit 1
fi
for title in "${mapped_h1_titles[@]}" "${h2_layout_titles[@]}" "${h2_survivor_titles[@]}"; do
  wait_for_home "$title" "${expected_home[$title]}"
done
printf 'r' >&"$remap_fd"
wait_for_count "$count"
wait_for_home scroll-a "${expected_home[scroll-a]}"
wait_for_geometry_snapshot "$before"

# The visible full width is not enough: the hidden restore width must survive
# as part of the layout snapshot too.
"$UMBRIEL" msg workspace-switch:1/HEADLESS-1 > /dev/null
focus_window scroll-c
"$UMBRIEL" msg window-toggle-maximize > /dev/null
wait_for_width scroll-c "$scroll_unmax_width"
"$UMBRIEL" msg workspace-switch:3/HEADLESS-1 > /dev/null
focus_window master-c
"$UMBRIEL" msg window-toggle-maximize > /dev/null
wait_for_width master-c "$master_unmax_width"

echo "scrolling, dwindle, master, and populated refuge layouts survived physical output loss"
