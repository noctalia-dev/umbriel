#!/usr/bin/env bash
# harness: outputs=2
# Committed root/subsurface buffers scale to the receiving output, including when the source workspace is hidden.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
cat >> "$UMBRIEL_CONFIG" <<'TOML'
[output.HEADLESS-1]
mode = "1280x720@60"
position = [0, 0]
workspaces = ["source", "away"]
[output.HEADLESS-2]
mode = "960x720@60"
position = [1280, 0]
workspaces = ["own", "source@HEADLESS-1"]
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[[window_rule]]
match.title = "mirror-render"
default_output = "HEADLESS-1"
default_workspace = "source"
default_floating = true
TOML
"$UMBRIEL" msg config-reload > /dev/null
mkfifo "$UMBRIEL_RUNTIME_DIR/control"
exec {control_fd}<> "$UMBRIEL_RUNTIME_DIR/control"
CHILD_ON_STDIN=1 WAYLAND_DEBUG=client "$UMBRIEL_SUBSURFACE_CLIENT" mirror-render 600 400 animate < "$UMBRIEL_RUNTIME_DIR/control" > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" mapped 1
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-1 "$UMBRIEL_RUNTIME_DIR/source.png"
grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/copy.png"
read -r sx sy sw sh < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/source.png" bbox 'b > 0.5 && r < 0.3 && g < 0.3')
read -r tx ty tw th < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/copy.png" bbox 'b > 0.5 && r < 0.3 && g < 0.3')
if ((sw == 0 || tw == 0 || tw < sw * 3 / 4 - 2 || tw > sw * 3 / 4 + 2 || th < sh * 3 / 4 - 2 || th > sh * 3 / 4 + 2)); then
  echo "copy should scale blue subsurface to 0.75: source ${sw}x${sh}, copy ${tw}x${th}"
  exit 1
fi
if ((tx < sx * 3 / 4 - 2 || tx > sx * 3 / 4 + 2 || ty < sy * 3 / 4 + 90 - 2 || ty > sy * 3 / 4 + 90 + 2)); then
  echo "copy should be centered: source $sx,$sy, copy $tx,$ty"
  exit 1
fi
"$UMBRIEL" msg workspace-switch away --output HEADLESS-1 > /dev/null
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/hidden-source.png"
read -r hx hy hw hh < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/hidden-source.png" bbox 'b > 0.5 && r < 0.3 && g < 0.3')
if [[ "$hx $hy $hw $hh" != "$tx $ty $tw $th" ]]; then
  echo "hiding source changed its copy: before $tx,$ty ${tw}x${th}; hidden $hx,$hy ${hw}x${hh}"
  exit 1
fi
# With the source hidden, callbacks must still arrive from the receiving output.
callbacks=$(grep -c 'wl_callback.*done' "$UMBRIEL_RUNTIME_DIR/client.log" || true)
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" '.*wl_callback.*done' "$((callbacks + 4))"
# Hiding the child reveals the copied parent buffer, not the last committed child.
printf h >&"$control_fd"
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" child-hidden 1
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/root.png"
read -r _ _ rw rh < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/root.png" bbox 'r > 0.5 && b < 0.3 && g < 0.3')
if ((rw != tw || rh != th)); then
  echo "copy should reveal the root surface after child hide: root ${rw}x${rh}, child ${tw}x${th}"
  exit 1
fi
printf p >&"$control_fd"
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" popup-mapped 1
"$UMBRIEL" settle > /dev/null
grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/popup.png"
read -r _ _ pw ph < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/popup.png" bbox 'g > 0.5 && r < 0.3 && b < 0.3')
if ((pw == 0 || ph == 0)); then
  echo "an xdg popup mapped on the hidden source is missing from the copy"
  exit 1
fi
# CPU measurement interval: the animated child is now hidden, leaving static buffers.
pid=$(< "$UMBRIEL_RUNTIME_DIR/instance.pgid")
read -ra start_stat < "/proc/$pid/stat"
ticks_per_second=$(getconf CLK_TCK)
sleep 3 # real time: measure process CPU time over a known wall-clock interval.
read -ra end_stat < "/proc/$pid/stat"
ticks=$((end_stat[13] + end_stat[14] - start_stat[13] - start_stat[14]))
if ((ticks * 100 >= ticks_per_second * 3 * 2)); then
  echo "static mirror exceeds 2 percent of one CPU: $ticks ticks over 3 seconds"
  exit 1
fi
echo "Static mirror CPU: $ticks ticks over 3 seconds (CLK_TCK=$ticks_per_second)."
echo 'A 0.75-scaled, centered subsurface remains visible when its source workspace is hidden.'
