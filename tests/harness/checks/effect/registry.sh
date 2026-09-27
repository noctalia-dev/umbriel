#!/usr/bin/env bash
# The effect registry compiles exactly the presets something enabled references,
# keeps a compiled or failed program across reloads that leave its source alone,
# and reports a referenced preset that fails to compile. The bundled presets
# define without selecting: including them all compiles none, and selecting them
# all compiles each on the GPU without diagnostics.
set -euo pipefail
readonly BASE="$UMBRIEL_RUNTIME_DIR/registry-base.toml"
readonly USED="$UMBRIEL_RUNTIME_DIR/used.glsl"
readonly BROKEN="$UMBRIEL_RUNTIME_DIR/broken.glsl"
readonly IDLE="$UMBRIEL_RUNTIME_DIR/idle.glsl"
readonly SHOWN="$UMBRIEL_RUNTIME_DIR/shown.glsl"
cp "$UMBRIEL_CONFIG" "$BASE"
echo 'vec4 screen(vec2 uv) { return umbriel_sample(uv); }' > "$USED"
echo 'this is not GLSL' > "$BROKEN"
echo 'vec4 animation(vec2 uv) { return umbriel_sample(uv); }' > "$IDLE"
echo 'vec4 animation(vec2 uv) { return umbriel_sample(uv) * umbriel_clamped_progress; }' > "$SHOWN"

write_config() {
  cp "$BASE" "$UMBRIEL_CONFIG"
  cat >> "$UMBRIEL_CONFIG" <<EOF

[effects]
screen = "used"
[effects.preset.used]
kind = "screen"
shader = "$USED"
[effects.preset.broken]
kind = "screen"
shader = "$BROKEN"
[effects.preset.idle]
kind = "animation"
shader = "$IDLE"
[effects.preset.shown]
kind = "animation"
shader = "$SHOWN"
[animation.layers]
enabled = false
effect = "idle"
EOF
  printf '%s\n' "$@" >> "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}

compiled() { grep -c "Compiling $1 shader: $2\$" "$UMBRIEL_LOG" || true; }
diagnosed() { grep -c "effect preset '$1' ($2) failed to compile; rendering plainly" "$UMBRIEL_LOG" || true; }
expect() {
  if [[ $2 != "$3" ]]; then
    echo "$1: got $2, expected $3"
    exit 1
  fi
}

write_config
expect "referenced preset compilations" "$(compiled screen "$USED")" 1
expect "unreferenced preset compilations" "$(compiled screen "$BROKEN")" 0
expect "unreferenced preset diagnostics" "$(diagnosed broken screen)" 0

write_config '[output.HEADLESS-1]' 'screen_effect = "broken"'
expect "output-rule preset compilations" "$(compiled screen "$BROKEN")" 1
expect "failed preset diagnostics" "$(diagnosed broken screen)" 1
expect "retained preset compilations after reload" "$(compiled screen "$USED")" 1

write_config '[animation]' 'duration_ms = 300' '[output.HEADLESS-1]' 'screen_effect = "broken"'
expect "failed preset compilations after an unrelated reload" "$(compiled screen "$BROKEN")" 1
expect "failed preset diagnostics after an unrelated reload" "$(diagnosed broken screen)" 1

cp "$USED" "$BROKEN"
write_config '[animation]' 'duration_ms = 300' '[output.HEADLESS-1]' 'screen_effect = "broken"'
expect "repaired preset compilations" "$(compiled screen "$BROKEN")" 2
expect "repaired preset diagnostics" "$(diagnosed broken screen)" 1

expect "compilations of a preset only a disabled event names" "$(compiled animation "$IDLE")" 0
expect "compilations of a preset no event names" "$(compiled animation "$SHOWN")" 0

write_config '[animation.windows_move]' 'effect = "shown"'
expect "compilations of a preset an enabled event names" "$(compiled animation "$SHOWN")" 1

readonly EFFECTS="$(cd "$UMBRIEL_REPO/examples/effects" && pwd)"
readonly LOG_MARK=$(($(wc -l < "$UMBRIEL_LOG") + 1))
write_bundled() {
  cp "$BASE" "$UMBRIEL_CONFIG"
  cat >> "$UMBRIEL_CONFIG" <<EOF

[include]
files = [
  "$EFFECTS/animation/reveal/effect.toml",
  "$EFFECTS/animation/squash/effect.toml",
  "$EFFECTS/border/pulse/effect.toml",
  "$EFFECTS/window/scanlines/effect.toml",
  "$EFFECTS/screen/vignette/effect.toml",
  "$EFFECTS/cursor/glow/effect.toml",
]
EOF
  printf '%s\n' "$@" >> "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" settle > /dev/null
}
open_window() {
  "$UMBRIEL_UNMAP_CLIENT" "$1" 400 300 > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  for _ in $(seq 80); do
    "$UMBRIEL" windows --json | jq -e --arg t "$1" '.[] | select(.title == $t)' > /dev/null && return
    sleep 0.025
  done
  echo "the $1 client never mapped"
  exit 1
}

write_bundled
open_window plain
"$UMBRIEL" settle > /dev/null
expect "compilations of included but unselected bundled presets" \
  "$(tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | grep -c "Compiling .* shader: $EFFECTS/" || true)" 0
expect "effect instances of included but unselected bundled presets" \
  "$("$UMBRIEL" effect-frames --json | jq '[.outputs[].eligible] | add')" 0

write_bundled '[effects]' 'border = "pulse"' 'window = "scanlines"' 'screen = "vignette"' 'cursor = "glow"' \
  '[animation.windows_in]' 'effect = "reveal"' '[animation.windows_move]' 'effect = "squash"'
expect "compilations of every selected bundled preset" \
  "$(tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | grep -c "Compiling .* shader: $EFFECTS/" || true)" 6
if tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | grep -Eq "failed to compile|unknown key|ignoring effects"; then
  echo "a bundled preset failed to compile or produced configuration diagnostics:"
  tail -n +"$LOG_MARK" "$UMBRIEL_LOG" | grep -E "failed to compile|unknown key|ignoring effects"
  exit 1
fi
open_window bundled
"$UMBRIEL" settle > /dev/null
echo "only enabled references compiled, programs and failures retained across reload, repairs recompiled, bundled" \
  "presets compiled only once selected"
