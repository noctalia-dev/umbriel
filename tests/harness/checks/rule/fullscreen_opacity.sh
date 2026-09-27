#!/usr/bin/env bash
# A compositor window-rule opacity applies while windowed, is bypassed while fullscreen, and resumes after leaving
# fullscreen. Client-provided alpha remains active in every state. The fullscreen backdrop leaves with an unmapped
# window, and without opaque_fullscreen a client-translucent fullscreen window shows the desktop instead of it.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/tests/subsurface-client}"
readonly UNMAP_CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly LAYER_CLIENT="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/fullscreen-rule-opacity-client.log"
readonly UNMAP_LOG="$UMBRIEL_RUNTIME_DIR/fullscreen-backdrop-unmap.log"
readonly WALLPAPER_LOG="$UMBRIEL_RUNTIME_DIR/fullscreen-backdrop-wallpaper.log"
readonly ALPHA_LOG="$UMBRIEL_RUNTIME_DIR/fullscreen-client-alpha.log"
readonly RULE_APP_ID=fullscreen-rule-opacity
readonly UNMAP_TITLE=fullscreen-backdrop-unmap
readonly ALPHA_APP_ID=fullscreen-client-alpha

if [[ ! -x $CLIENT ]]; then
  echo "subsurface client not built at $CLIENT"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1
curve = "linear"

[colors]
backdrop = "#00FF00FF"

[appearance]
border_width = 0
corner_radius = 0

[[window_rule]]
match.app_id = "^fullscreen-rule-opacity$"
opacity = 0.5
EOF
"$UMBRIEL" msg config-reload > /dev/null

wait_for_fullscreen() {
  local title=$1 expected=$2
  local state=
  for _ in $(seq 60); do
    state=$("$UMBRIEL" tearing --json)
    if jq -e ".surfaces[] | select(.title == \"$title\") | .fullscreen == $expected" \
        <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.05
  done
  echo "$title fullscreen state did not become $expected: $state"
  return 1
}

sample_center() {
  local title=$1 screenshot=$2
  local windows win_x win_y win_w win_h
  windows=$("$UMBRIEL" windows --json)
  read -r win_x win_y win_w win_h <<< "$(
    jq -r ".[] | select(.title == \"$title\") | \"\(.x) \(.y) \(.w) \(.h)\"" <<< "$windows"
  )"
  grim "$screenshot"
  magick "$screenshot" \
    -crop "40x40+$((win_x + win_w / 2 - 20))+$((win_y + win_h / 2 - 20))" \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

assert_fullscreen_client_alpha() {
  local label=$1 red=$2 green=$3 blue=$4
  # Fullscreen removes only the rule multiplier. The client's 0.5 alpha blends magenta and green equally into gray.
  if (( red < 100 || red > 155 || green < 100 || green > 155 || blue < 100 || blue > 155 )); then
    echo "$label did not bypass rule opacity while preserving client alpha: red=$red green=$green blue=$blue"
    exit 1
  fi
}

assert_windowed_rule() {
  local red=$1 green=$2 blue=$3
  # Client alpha 0.5 and rule opacity 0.5 leave one quarter magenta over the green backdrop.
  if (( red < 35 || red > 95 || green < 165 || green > 215 || blue < 35 || blue > 95 )); then
    echo "windowed rule opacity was not restored: red=$red green=$green blue=$blue"
    exit 1
  fi
}

wait_for_line() {
  local log=$1 line=$2
  for _ in $(seq 80); do
    grep -q "^$line\$" "$log" && return 0
    sleep 0.05
  done
  echo "$log never printed $line: $(< "$log")"
  return 1
}

sample_corner() {
  grim "$1"
  magick "$1" -crop "20x20+5+5" \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]' info:
}

is_backdrop() {
  (( $1 < 30 && $2 > 225 && $3 < 30 ))
}

# The background layer client fills the output with 0x5577AA.
is_wallpaper() {
  (( $1 > 65 && $1 < 105 && $2 > 99 && $2 < 139 && $3 > 150 && $3 < 190 ))
}

env INITIAL_FULLSCREEN=1 TRANSLUCENT_CONTENT=1 "$CLIENT" "$RULE_APP_ID" > "$CLIENT_LOG" 2>&1 &
rule_client_pid=$!
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "fullscreen opacity client never mapped: $(< "$CLIENT_LOG")"
  exit 1
fi

wait_for_fullscreen "$RULE_APP_ID" true
sleep 0.15
read -r red green blue <<< "$(sample_center "$RULE_APP_ID" "$UMBRIEL_RUNTIME_DIR/fullscreen-rule-opacity-initial.png")"
assert_fullscreen_client_alpha "initial fullscreen window" "$red" "$green" "$blue"

"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen "$RULE_APP_ID" false
sleep 0.15
read -r red green blue <<< "$(sample_center "$RULE_APP_ID" "$UMBRIEL_RUNTIME_DIR/fullscreen-rule-opacity-windowed.png")"
assert_windowed_rule "$red" "$green" "$blue"

"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen "$RULE_APP_ID" true
sleep 0.15
read -r red green blue <<< "$(sample_center "$RULE_APP_ID" "$UMBRIEL_RUNTIME_DIR/fullscreen-rule-opacity-restored.png")"
assert_fullscreen_client_alpha "restored fullscreen window" "$red" "$green" "$blue"

# The desktop background is the backdrop colour too, so a wallpaper tells the two apart.
kill "$rule_client_pid"
wait "$rule_client_pid" 2>/dev/null || true
"$LAYER_CLIENT" HEADLESS-1 0 > "$WALLPAPER_LOG" 2>&1 &
wait_for_line "$WALLPAPER_LOG" ready

# A 64x64 client leaves the rest of the output to the backdrop. Unmapping it without destroying the surface must take
# the backdrop along, even though the unmap commit follows handleUnmap.
"$UNMAP_CLIENT" "$UNMAP_TITLE" > "$UNMAP_LOG" 2>&1 &
wait_for_line "$UNMAP_LOG" mapped
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
wait_for_fullscreen "$UNMAP_TITLE" true
"$UMBRIEL" settle
read -r red green blue <<< "$(sample_corner "$UMBRIEL_RUNTIME_DIR/fullscreen-backdrop-mapped.png")"
if ! is_backdrop "$red" "$green" "$blue"; then
  echo "small fullscreen window drew no backdrop: red=$red green=$green blue=$blue"
  exit 1
fi
"$UMBRIEL" msg window-close > /dev/null
wait_for_line "$UNMAP_LOG" unmapped
"$UMBRIEL" settle
read -r red green blue <<< "$(sample_corner "$UMBRIEL_RUNTIME_DIR/fullscreen-backdrop-unmapped.png")"
if ! is_wallpaper "$red" "$green" "$blue"; then
  echo "fullscreen backdrop outlived its unmapped window: red=$red green=$green blue=$blue"
  exit 1
fi

# Without opaque_fullscreen, client alpha alone drops the backdrop: half-alpha magenta blends with the wallpaper.
sed -i '/^\[appearance\]$/a opaque_fullscreen = false' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
env INITIAL_FULLSCREEN=1 TRANSLUCENT_CONTENT=1 "$CLIENT" "$ALPHA_APP_ID" > "$ALPHA_LOG" 2>&1 &
wait_for_line "$ALPHA_LOG" mapped
wait_for_fullscreen "$ALPHA_APP_ID" true
"$UMBRIEL" settle
read -r red green blue <<< "$(sample_center "$ALPHA_APP_ID" "$UMBRIEL_RUNTIME_DIR/fullscreen-client-alpha.png")"
if (( red < 150 || red > 190 || green < 40 || green > 80 || blue < 193 || blue > 233 )); then
  echo "client-translucent fullscreen window did not show the desktop: red=$red green=$green blue=$blue"
  exit 1
fi

echo "fullscreen bypassed rule opacity while preserving client alpha, and the backdrop followed map state and client alpha"
