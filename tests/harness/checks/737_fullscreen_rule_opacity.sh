#!/usr/bin/env bash
# A compositor window-rule opacity applies while windowed, is bypassed while fullscreen, and resumes after leaving
# fullscreen. Client-provided alpha remains active in every state.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/tests/subsurface-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/fullscreen-rule-opacity-client.log"
readonly RULE_APP_ID=fullscreen-rule-opacity

if [[ ! -x $CLIENT ]]; then
  echo "subsurface client not built at $CLIENT"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1

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

env INITIAL_FULLSCREEN=1 TRANSLUCENT_CONTENT=1 "$CLIENT" "$RULE_APP_ID" > "$CLIENT_LOG" 2>&1 &
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

echo "fullscreen bypassed rule opacity across map and toggles while preserving client alpha"
