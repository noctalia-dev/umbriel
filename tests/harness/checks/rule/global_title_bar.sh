#!/usr/bin/env bash
# Global off/on/forced are defaults; explicit rules override them in existing and opening windows.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
title_bar_config
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[[window_rule]]
match.title = "^mode-force-on$"
title_bar = "forced"
[[window_rule]]
match.title = "^mode-force-off$"
title_bar = "off"
[[window_rule]]
match.title = "^mode-on-"
title_bar = "on"
CONFIG
set_mode() {
  sed -i '/^\[appearance.title_bar\]$/ { n; s/mode = "[^"]*"/mode = "'"$1"'"/; }' "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}
spawn_mode_client() {
  local title=$1
  shift
  env LOG_CONFIGURES=1 "$@" "$UMBRIEL_FRACTIONAL_CLIENT" "$title" 480 300 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
}
initial_height() {
  local first
  first=$(rg -m 1 '^configure ' "$UMBRIEL_RUNTIME_DIR/$1.log")
  [[ ${first##*x} == "$2" ]] || { echo "$1: expected initial height $2, got $first"; return 1; }
}

set_mode off
spawn_mode_client mode-inherit-csd XDG_DECORATION=client
spawn_mode_client mode-inherit-ssd XDG_DECORATION=server
spawn_mode_client mode-no-preference
spawn_mode_client mode-force-on XDG_DECORATION=client
spawn_mode_client mode-force-off XDG_DECORATION=server
spawn_mode_client mode-on-csd XDG_DECORATION=client
spawn_mode_client mode-on-ssd XDG_DECORATION=server
for title in mode-inherit-csd mode-inherit-ssd mode-no-preference mode-force-off mode-on-csd; do
  title_bar_wait_field "$title" title_bar false
done
title_bar_wait_field mode-force-on title_bar true
title_bar_wait_field mode-on-ssd title_bar true
initial_height mode-force-on 676
initial_height mode-inherit-ssd 704
"$UMBRIEL" settle > /dev/null
before=$(title_bar_window mode-inherit-csd)

set_mode forced
for title in mode-inherit-csd mode-inherit-ssd mode-no-preference mode-force-on mode-on-ssd; do
  title_bar_wait_field "$title" title_bar true
done
title_bar_wait_field mode-force-off title_bar false
title_bar_wait_field mode-on-csd title_bar false
"$UMBRIEL" settle > /dev/null
after=$(title_bar_window mode-inherit-csd)
[[ $(jq -r .h <<< "$after") == $(($(jq -r .h <<< "$before") - 28)) ]]
[[ $(jq -r .y <<< "$after") == $(($(jq -r .y <<< "$before") + 28)) ]]
spawn_mode_client mode-late-csd XDG_DECORATION=client
title_bar_wait_field mode-late-csd title_bar true
initial_height mode-late-csd 676

set_mode on
for title in mode-inherit-csd mode-late-csd mode-on-csd mode-force-off; do
  title_bar_wait_field "$title" title_bar false
done
for title in mode-inherit-ssd mode-no-preference mode-force-on mode-on-ssd; do
  title_bar_wait_field "$title" title_bar true
done

# Inheritance also updates maximized floating content when only the global default changes.
set_mode forced
spawn_mode_client ssd-float-mode XDG_DECORATION=client
title_bar_wait_field ssd-float-mode title_bar true
id=$(title_bar_window ssd-float-mode | jq -r .id)
"$UMBRIEL" msg window-focus "$id" > /dev/null
"$UMBRIEL" msg window-toggle-maximize > /dev/null
title_bar_wait_box ssd-float-mode 1264x676+8+36
set_mode off
title_bar_wait_field ssd-float-mode title_bar false
title_bar_wait_box ssd-float-mode 1264x704+8+8
title_bar_wait_field mode-force-on title_bar true
title_bar_wait_field mode-on-ssd title_bar true
set_mode forced
title_bar_wait_field ssd-float-mode title_bar true
title_bar_wait_box ssd-float-mode 1264x676+8+36
set_mode on
title_bar_wait_field ssd-float-mode title_bar false
title_bar_wait_box ssd-float-mode 1264x704+8+8
echo 'global modes reload, opening sizes inherit correctly, and explicit rules override every default'
