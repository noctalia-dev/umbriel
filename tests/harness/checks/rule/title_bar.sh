#!/usr/bin/env bash
# On follows decoration preferences; explicit rules override them and reload on existing windows.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
title_bar_config
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[[window_rule]]
match.title = "^ssd-force-on$"
title_bar = "forced"
[[window_rule]]
match.title = "^ssd-force-off$"
title_bar = "off"
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

declare -A controls
start_controlled() {
  local title=$1 fd
  shift
  mkfifo "$UMBRIEL_RUNTIME_DIR/$title.fifo"
  exec {fd}<>"$UMBRIEL_RUNTIME_DIR/$title.fifo"
  controls[$title]=$fd
  env DECORATION_CONTROL=1 LOG_CONFIGURES=1 "$@" "$UMBRIEL_FRACTIONAL_CLIENT" "$title" 480 300 \
    <&"$fd" > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
}
command_to() {
  printf '%s\n' "$2" >&"${controls[$1]}"
}

start_controlled ssd-on-xdg XDG_DECORATION=server
title_bar_wait_field ssd-on-xdg title_bar true
"$UMBRIEL" settle > /dev/null
before=$(title_bar_window ssd-on-xdg)
command_to ssd-on-xdg 'xdg client'
title_bar_wait_field ssd-on-xdg title_bar false
"$UMBRIEL" settle > /dev/null
after=$(title_bar_window ssd-on-xdg)
[[ $(jq -r .h <<< "$after") == $(($(jq -r .h <<< "$before") + 28)) ]]
[[ $(jq -r .y <<< "$after") == $(($(jq -r .y <<< "$before") - 28)) ]]
command_to ssd-on-xdg 'xdg server'
title_bar_wait_field ssd-on-xdg title_bar true
command_to ssd-on-xdg 'xdg client'
title_bar_wait_field ssd-on-xdg title_bar false
command_to ssd-on-xdg 'xdg unset'
title_bar_wait_field ssd-on-xdg title_bar true
command_to ssd-on-xdg 'xdg client'
title_bar_wait_field ssd-on-xdg title_bar false
command_to ssd-on-xdg 'xdg destroy'
title_bar_wait_field ssd-on-xdg title_bar true

# An explicit legacy CSD request made before the xdg role affects the opening configure.
start_controlled ssd-on-legacy LEGACY_DECORATION=client
title_bar_wait_field ssd-on-legacy title_bar false
"$UMBRIEL" settle > /dev/null
first=$(rg -m 1 '^configure ' "$UMBRIEL_RUNTIME_DIR/ssd-on-legacy.log")
[[ ${first##*x} == 704 ]]
command_to ssd-on-legacy 'legacy server'
title_bar_wait_field ssd-on-legacy title_bar true
command_to ssd-on-legacy 'legacy none'
title_bar_wait_field ssd-on-legacy title_bar false
command_to ssd-on-legacy 'legacy destroy'
title_bar_wait_field ssd-on-legacy title_bar true

# XDG wins when both protocols exist, then falls back to the remaining legacy preference.
start_controlled ssd-on-both LEGACY_DECORATION=server XDG_DECORATION=client
title_bar_wait_field ssd-on-both title_bar false
command_to ssd-on-both 'legacy client'
command_to ssd-on-both 'xdg server'
title_bar_wait_field ssd-on-both title_bar true
command_to ssd-on-both 'xdg destroy'
title_bar_wait_field ssd-on-both title_bar false
command_to ssd-on-both 'legacy destroy'
title_bar_wait_field ssd-on-both title_bar true

start_controlled ssd-force-on XDG_DECORATION=client
title_bar_wait_field ssd-force-on title_bar true
first=$(rg -m 1 '^configure ' "$UMBRIEL_RUNTIME_DIR/ssd-force-on.log")
[[ ${first##*x} == 676 ]]
start_controlled ssd-force-off XDG_DECORATION=server
title_bar_wait_field ssd-force-off title_bar false
first=$(rg -m 1 '^configure ' "$UMBRIEL_RUNTIME_DIR/ssd-force-off.log")
[[ ${first##*x} == 704 ]]

# Last matching on resets an earlier force override, and a live title change also re-evaluates the rule.
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[[window_rule]]
match.title = "^ssd-force-on$"
title_bar = "on"
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
title_bar_wait_field ssd-force-on title_bar false
command_to ssd-force-on 'xdg server'
title_bar_wait_field ssd-force-on title_bar true
command_to ssd-force-on 'title ssd-force-off'
# There are now two windows with this title. Both must honor off.
for _ in $(seq 120); do
  states=$("$UMBRIEL" windows --json | jq '[.[] | select(.title == "ssd-force-off") | .title_bar]')
  [[ $(jq 'length' <<< "$states") == 2 && $(jq 'all(. == false)' <<< "$states") == true ]] && break
  sleep 0.025
done
[[ $(jq 'length' <<< "$states") == 2 && $(jq 'all(. == false)' <<< "$states") == true ]]

# No protocol inherits the global default. Explicit forced mode overrides a global off default.
start_controlled ssd-no-preference
title_bar_wait_field ssd-no-preference title_bar true
sed -i 's/title_bar = "on"/title_bar = "forced"/' "$UMBRIEL_CONFIG"
command_to ssd-force-on 'title ssd-force-on'
"$UMBRIEL" msg config-reload > /dev/null
title_bar_wait_field ssd-force-on title_bar true
sed -i '/^\[appearance.title_bar\]$/ { n; s/mode = "on"/mode = "off"/; }' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
title_bar_wait_field ssd-force-on title_bar true
title_bar_wait_field ssd-no-preference title_bar false
sed -i '/^\[appearance.title_bar\]$/ { n; s/mode = "off"/mode = "on"/; }' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
title_bar_wait_field ssd-force-on title_bar true
# Floating maximize refits content when decoration mode changes without losing maximize state.
start_controlled ssd-float-on XDG_DECORATION=server
title_bar_wait_field ssd-float-on title_bar true
id=$(title_bar_window ssd-float-on | jq -r .id)
"$UMBRIEL" msg window-focus "$id" > /dev/null
"$UMBRIEL" msg window-toggle-maximize > /dev/null
title_bar_wait_box ssd-float-on 1264x676+8+36
command_to ssd-float-on 'xdg client'
title_bar_wait_field ssd-float-on title_bar false
title_bar_wait_box ssd-float-on 1264x704+8+8
command_to ssd-float-on 'xdg server'
title_bar_wait_field ssd-float-on title_bar true
title_bar_wait_box ssd-float-on 1264x676+8+36
echo 'automatic title bars follow both protocols, preserve content geometry, and honor live rule overrides'
