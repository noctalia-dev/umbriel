#!/usr/bin/env bash
# A real toplevel follows an active data drag without intercepting the target
# underneath it. It can unmap and reattach during the same drag, then return to
# ordinary layout placement when dropped.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

cat >> "$UMBRIEL_CONFIG" <<'EOF'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
drag_opacity = 1.0
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = false
[[window_rule]]
match.title = "^toplevel-drag-source$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
[[window_rule]]
match.title = "^toplevel-drag-target$"
default_floating = true
default_position = { x = 800, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL_GLOBAL_CLIENT" xdg_toplevel_drag_manager_v1 present 1

window() {
  "$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)'
}

for variant in detached before-start after-map existing dwindle master held-buttons; do
  layout=scrolling
  [[ $variant == dwindle || $variant == master ]] && layout=$variant
  "$UMBRIEL" msg workspace-set-layout "$layout" > /dev/null
  source_log="$UMBRIEL_RUNTIME_DIR/toplevel-$variant-source.log"
  target_log="$UMBRIEL_RUNTIME_DIR/toplevel-$variant-target.log"
  fifo="$UMBRIEL_RUNTIME_DIR/toplevel-$variant.fifo"
  mkfifo "$fifo"
  "$UMBRIEL_DRAG_CLIENT" target toplevel-drag-target > "$target_log" 2>&1 &
  target_pid=$!
  await_lines "$target_log" ready 1
  "$UMBRIEL_DRAG_CLIENT" toplevel "$variant" < "$fifo" > "$source_log" 2>&1 &
  source_pid=$!
  exec {control_fd}> "$fifo"
  await_lines "$source_log" ready 1
  "$UMBRIEL" settle > /dev/null
  if [[ $variant == held-buttons ]]; then
    pointer_hold 1280 720 move 200 200 press 272 -- move 400 300 mark moved hold move 1000 200 mark target hold press 273 release 272 mark dropped hold release 273
  else
    pointer_hold 1280 720 move 200 200 press 272 -- move 400 300 mark moved hold move 1000 200 mark target hold release 272
  fi
  await_lines "$source_log" drag-started 1
  title=toplevel-drag-window
  if [[ $variant == existing ]]; then
    title=toplevel-drag-source
  else
    await_lines "$source_log" tear-off-mapped 1
  fi
  pointer_step moved
  "$UMBRIEL" settle > /dev/null
  box=$(window "$title")
  [[ $(jq -r '"\(.x) \(.y)"' <<< "$box") == '380 270' ]] || {
    echo "$variant: attached window did not follow pointer with its offsets: $box"; exit 1;
  }
  [[ $(jq -r '.column' <<< "$box") == -1 ]] || { echo 'carried window stayed in tiling'; exit 1; }
  if [[ $variant == detached ]]; then
    printf 'u\n' >&"$control_fd"
    await_lines "$source_log" 'command u' 1
    "$UMBRIEL" settle > /dev/null
    [[ -z $(window "$title") ]] || { echo 'unmapped carried window stayed visible'; exit 1; }
    printf 'r\n' >&"$control_fd"
    await_lines "$source_log" 'tear-off-mapped' 2
    printf 'g\n' >&"$control_fd"
    await_lines "$source_log" 'command g' 1
    printf 'o\n' >&"$control_fd"
    await_lines "$source_log" 'command o' 1
  fi
  pointer_step target
  "$UMBRIEL" settle > /dev/null
  box=$(window "$title")
  [[ $(jq -r '"\(.x) \(.y)"' <<< "$box") == '980 170' ]] || {
    echo "$variant: attached window stopped following pointer: $box"; exit 1;
  }
  await_lines "$target_log" drag-enter 1
  image="$UMBRIEL_RUNTIME_DIR/toplevel-$variant.png"
  grim -o HEADLESS-1 "$image"
  read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$image" pixel 1040 240)
  if [[ $variant != existing ]]; then
    ((red > green + 40 && blue > green + 15)) || { echo "carried window is not rendered: $red,$green,$blue"; exit 1; }
  fi
  if [[ $variant == held-buttons ]]; then
    pointer_step dropped
    await_lines "$source_log" drag-finished 1
    printf 'd\n' >&"$control_fd"
    await_lines "$source_log" 'command d' 1
  fi
  pointer_release
  await_lines "$target_log" drop-received 1
  await_lines "$source_log" drag-finished 1
  wait "$target_pid"
  "$UMBRIEL" settle > /dev/null
  box=$(window "$title")
  if [[ $variant != existing ]]; then
    [[ $(jq -r '.floating' <<< "$box") == false ]] || { echo "$variant: dropped window did not join tiling: $box"; exit 1; }
  fi
  if [[ $variant == held-buttons ]]; then
    printf 'q\n' >&"$control_fd"
  else
    printf 'dq\n' >&"$control_fd"
  fi
  wait "$source_pid"
  exec {control_fd}>&-
  "$UMBRIEL" settle > /dev/null
  echo "$variant: motion, underlying drop target, and cleanup verified"
done
