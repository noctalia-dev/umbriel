#!/usr/bin/env bash
# harness: outputs=2
# harness: xwayland=true
# An override-redirect menu is copied only through its real X11 transient parent.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
cat >> "$UMBRIEL_CONFIG" <<'TOML'
[animation]
enabled = false
[output.HEADLESS-1]
position = [0, 0]
workspaces = ["source", "away"]
[output.HEADLESS-2]
position = [1280, 0]
workspaces = ["own", "source"]
[[workspace]]
output = "HEADLESS-2"
name = "source"
mirror = { output = "HEADLESS-1", name = "source" }
[[window_rule]]
match.title = "^xwayland-popup-parent$"
default_output = "HEADLESS-1"
default_workspace = "source"
default_floating = true
TOML
"$UMBRIEL" msg config-reload > /dev/null
mkfifo "$UMBRIEL_RUNTIME_DIR/control"
exec {control_fd}<> "$UMBRIEL_RUNTIME_DIR/control"
"$UMBRIEL_XWAYLAND_POPUP_CLIENT" mirror-menu <&"$control_fd" > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/client.log" ready 1
printf '%s\n' parent-map transient 'override true' map >&"$control_fd"
for _ in $(seq 80); do
  count=$("$UMBRIEL" windows --json | jq '[.[] | select(.title == "xwayland-popup-parent")] | length')
  [[ $count == 1 ]] && break
  sleep 0.025
done
[[ $count == 1 ]]
"$UMBRIEL" msg workspace-switch source --output HEADLESS-2 > /dev/null
"$UMBRIEL" msg workspace-switch away --output HEADLESS-1 > /dev/null
"$UMBRIEL" settle > /dev/null
menu_box() {
  grim -o HEADLESS-2 "$UMBRIEL_RUNTIME_DIR/menu.png"
  "$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/menu.png" bbox 'b > 0.5 && r < 0.3 && g > 0.3 && g < 0.5'
}
for _ in $(seq 60); do
  read -r x y w h < <(menu_box)
  ((w > 0 && h > 0)) && break
  sleep 0.025
done
if ((w == 0 || h == 0)); then
  echo 'override-redirect menu missing from copy of its hidden parent workspace'
  exit 1
fi
printf '%s\n' unmap >&"$control_fd"
for _ in $(seq 60); do
  read -r _ _ w h < <(menu_box)
  ((w == 0 && h == 0)) && break
  sleep 0.025
done
if ((w != 0 || h != 0)); then
  echo 'unmapped override-redirect menu left stale copied pixels'
  exit 1
fi
echo 'X11 transient override-redirect menu follows the copied parent and disappears on unmap.'
