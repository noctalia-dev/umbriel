#!/usr/bin/env bash
# harness: outputs=1
# Floating windows honor default_width/default_height as usable-area fractions,
# per axis, and default_size (pixels) outranks them. The default 1280x720 output
# keeps the expected pixels exact; a post-boot mode reload races the first
# client commit's usable-area snapshot, so no custom mode here.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/fractional-client}"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[[window_rule]]
match.title = "^frac-both$"
default_floating = true
default_width = 0.5
default_height = 0.5

[[window_rule]]
match.title = "^frac-width-only$"
default_floating = true
default_width = 0.25

[[window_rule]]
match.title = "^frac-vs-pixels$"
default_floating = true
default_size = [300, 200]
default_width = 0.9
default_height = 0.9
EOF
"$UMBRIEL" msg config-reload > /dev/null

field_of() {
  "$UMBRIEL" windows --json \
    | jq -r --arg title "$1" --arg field "$2" '.[] | select(.title == $title) | .[$field]'
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

# The client adopts every configured size, so IPC geometry reports what the
# compositor asked for, overriding its own 800x600 preference.
for title in frac-both frac-width-only frac-vs-pixels; do
  "$CLIENT" "$title" > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
done

# Both axes: 0.5 of 1280x720.
wait_for_field frac-both floating true
wait_for_field frac-both w 640
wait_for_field frac-both h 360

# One axis only: the unset axis keeps the client's own 600 preference.
wait_for_field frac-width-only floating true
wait_for_field frac-width-only w 320
wait_for_field frac-width-only h 600

# Pixels outrank fractions on both axes.
wait_for_field frac-vs-pixels floating true
wait_for_field frac-vs-pixels w 300
wait_for_field frac-vs-pixels h 200

echo "floating windows size from usable-area fractions per axis, and default_size still wins"
