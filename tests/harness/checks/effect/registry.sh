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

# registry-runtime: IPC-only selectors prepare synchronously, and pool roots
# reach every member and border overlay even while their owner is suppressed.
cp "$BASE" "$UMBRIEL_CONFIG"
cat > "$UMBRIEL_RUNTIME_DIR/runtime-border.glsl" <<'GLSL'
vec4 border(vec2 uv) { return vec4(0.0, 1.0, 0.0, 1.0); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/runtime-overlay.glsl" <<'GLSL'
vec4 window(vec2 uv) { return vec4(1.0, 0.0, 0.0, 1.0); }
GLSL
cat > "$UMBRIEL_RUNTIME_DIR/runtime-broken.glsl" <<'GLSL'
not valid GLSL
GLSL
# Nonalphabetical definitions traverse nested includes before the root file.
cat > "$UMBRIEL_RUNTIME_DIR/registry-nested.toml" <<'EOF_NESTED'
[effects.preset.runtime_border_z]
kind = "border"
shader = "runtime-border.glsl"
overlay = "runtime_overlay"
[effects.preset.runtime_border_a]
kind = "border"
shader = "runtime-border.glsl"
EOF_NESTED
cat > "$UMBRIEL_RUNTIME_DIR/registry-include.toml" <<'EOF_INCLUDE'
[include]
files = ["registry-nested.toml"]
[effects.pool.runtime_pool_z]
kind = "border"
choose = ["runtime_border_z", "runtime_border_a"]
EOF_INCLUDE
cat >> "$UMBRIEL_CONFIG" <<'EOF_CONFIG'
[include]
files = ["registry-include.toml"]
[animation]
enabled = false
[appearance]
border_width = 6
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[effects]
in_capture = true
[effects.preset.runtime_overlay]
kind = "window"
shader = "runtime-overlay.glsl"
[effects.preset.runtime_inert]
kind = "window"
[effects.preset.runtime_failed]
kind = "window"
shader = "runtime-broken.glsl"
[effects.pool.runtime_pool_a]
kind = "border"
choose = []
[[window_rule]]
match.title = "^runtime-root$"
default_floating = true
EOF_CONFIG
"$UMBRIEL" msg config-reload > /dev/null
FILL_COLOR=0xFF0000FF open_window runtime-root
"$UMBRIEL" settle > /dev/null
runtime_window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "runtime-root")')
runtime_id=$(jq -r .id <<< "$runtime_window")
read -r rx ry rw rh < <(jq -r '"\(.x) \(.y) \(.w) \(.h)"' <<< "$runtime_window")
runtime_state() { "$UMBRIEL" effects --json | jq -er --arg name "$1" '.presets[] | select(.name == $name) | .state'; }
runtime_pixel() {
  grim "$UMBRIEL_RUNTIME_DIR/runtime-registry.png"
  "$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/runtime-registry.png" pixel "$((rx + rw / 2))" "$((ry + rh / 2))"
}
expect "registry-unreferenced" "$(runtime_state runtime_border_z)" unreferenced
"$UMBRIEL" effects --json | jq -e '
  [.presets[].name] == ["runtime_border_z", "runtime_border_a", "runtime_overlay", "runtime_inert", "runtime_failed"]
  and [.pools[].name] == ["runtime_pool_z", "runtime_pool_a"]' > /dev/null || { echo "registry-declaration-order"; exit 1; }
runtime_text=$("$UMBRIEL" effects)
runtime_preset_order=$(awk -F '\t' '$1 ~ /^runtime_(border_z|border_a|overlay|inert|failed)$/ {print $1}' <<< "$runtime_text" | paste -sd ',')
runtime_pool_order=$(awk -F '\t' '$1 ~ /^runtime_pool_[za]$/ {print $1}' <<< "$runtime_text" | paste -sd ',')
expect "registry-text-preset-order" "$runtime_preset_order" "runtime_border_z,runtime_border_a,runtime_overlay,runtime_inert,runtime_failed"
expect "registry-text-pool-order" "$runtime_pool_order" "runtime_pool_z,runtime_pool_a"
"$UMBRIEL" msg "effect-border-set:runtime_pool_z/$runtime_id" > /dev/null
"$UMBRIEL" settle > /dev/null
for name in runtime_border_z runtime_border_a runtime_overlay; do
  expect "registry-pool-root $name" "$(runtime_state "$name")" compiled
done
read -r r g b < <(runtime_pixel)
if (( r < 240 || g > 15 || b > 15 )); then
  echo "registry-overlay-pixel: IPC-only border overlay did not render: $r $g $b"
  exit 1
fi
"$UMBRIEL" msg "effect-border-toggle:$runtime_id" > /dev/null
"$UMBRIEL" msg "effect-window-set:runtime_inert/$runtime_id" > /dev/null
expect "registry-inert" "$(runtime_state runtime_inert)" inert
"$UMBRIEL" settle > /dev/null
read -r r g b < <(runtime_pixel)
if (( b < 240 || r > 15 || g > 15 )); then
  echo "registry-inert-plain: inert selection did not render plainly: $r $g $b"
  exit 1
fi
"$UMBRIEL" msg "effect-window-set:runtime_failed/$runtime_id" > /dev/null
"$UMBRIEL" settle > /dev/null
expect "registry-failed" "$(runtime_state runtime_failed)" failed
for name in runtime_border_z runtime_border_a runtime_overlay; do
  expect "registry-suppressed-root $name" "$(runtime_state "$name")" compiled
done
read -r r g b < <(runtime_pixel)
if (( b < 240 || r > 15 || g > 15 )); then
  echo "registry-failed-plain: failed selection did not render plainly: $r $g $b"
  exit 1
fi
"$UMBRIEL" msg "effect-border-reset:$runtime_id" > /dev/null
for name in runtime_border_z runtime_border_a runtime_overlay; do
  expect "registry-pruned-root $name" "$(runtime_state "$name")" unreferenced
done
"$UMBRIEL" msg "effect-window-reset:$runtime_id" > /dev/null
expect "registry-pruned-failure" "$(runtime_state runtime_failed)" unreferenced

# Configured effect actions are preparation roots before any key or corner fires.
# Only action/enablement changes below: selectors and preset definitions stay fixed.
readonly ACTION_ROOT_BASE="$UMBRIEL_RUNTIME_DIR/action-root-base.toml"
cp "$BASE" "$ACTION_ROOT_BASE"
cat >> "$ACTION_ROOT_BASE" <<'EOF_ROOTS'
[animation]
enabled = false
[effects.preset.action_border]
kind = "border"
shader = "runtime-border.glsl"
overlay = "action_overlay"
[effects.preset.action_overlay]
kind = "window"
shader = "runtime-overlay.glsl"
[effects.preset.action_spare]
kind = "window"
shader = "runtime-overlay.glsl"
[effects.pool.action_pool]
kind = "border"
choose = ["action_border"]
EOF_ROOTS
write_action_roots() {
  cp "$ACTION_ROOT_BASE" "$UMBRIEL_CONFIG"
  printf '%s\n' "$@" >> "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" settle > /dev/null
}
expect_action_roots() {
  local label=$1 expected=$2 name
  for name in action_border action_overlay; do
    expect "$label $name" "$(runtime_state "$name")" "$expected"
  done
}
write_action_roots '[keybinds]' '"Mod+F9" = "effect-border-cycle:action_pool"'
expect_action_roots registry-keybind-prepared compiled
write_action_roots '[keybinds]' '"Mod+F9" = "effect-window-set:action_spare"'
expect_action_roots registry-keybind-replaced unreferenced
expect "registry-keybind-replacement-prepared" "$(runtime_state action_spare)" compiled
write_action_roots '[hot_corners.top_left]' 'enabled = false' 'action = "effect-border-set:action_pool"'
expect_action_roots registry-disabled-corner-unreferenced unreferenced
expect "registry-keybind-removed" "$(runtime_state action_spare)" unreferenced
write_action_roots '[hot_corners.top_left]' 'enabled = true' 'action = "effect-border-set:action_pool"'
expect_action_roots registry-corner-pool-prepared compiled
write_action_roots '[hot_corners.top_left]' 'enabled = true' 'action = "effect-border-set:action_border"'
expect_action_roots registry-corner-preset-prepared compiled
write_action_roots '[hot_corners.top_left]' 'enabled = true' 'action = "effect-window-set:action_spare"'
expect_action_roots registry-corner-replaced unreferenced
expect "registry-corner-replacement-prepared" "$(runtime_state action_spare)" compiled
write_action_roots '[hot_corners.top_left]' 'enabled = false' 'action = "effect-window-set:action_spare"'
expect "registry-corner-disabled" "$(runtime_state action_spare)" unreferenced
