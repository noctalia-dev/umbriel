#!/usr/bin/env bash
# The renderer's input color transform support enables the standard version 2 color-management global and the Wine
# compatibility global. Builds with wayland-protocols 1.49 expose version 3 to Wine clients for Windows BT.2100.
set -euo pipefail

readonly GLOBAL_CLIENT="${UMBRIEL_GLOBAL_CLIENT:-./build-debug/tests/global-client}"
readonly UNMAP_CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly POINTER_CLIENT="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
CLIENT_PID=

if [[ ! -x $GLOBAL_CLIENT || ! -x $POINTER_CLIENT ]]; then
  echo "required harness clients are not built"
  exit 1
fi

COLOR_MANAGER_VERSION=$("$UNMAP_CLIENT" --color-manager-version)
if [[ $COLOR_MANAGER_VERSION != 2 && $COLOR_MANAGER_VERSION != 3 ]]; then
  echo "unexpected compiled color-management version: $COLOR_MANAGER_VERSION"
  exit 1
fi

"$GLOBAL_CLIENT" wp_color_manager_v1 present 2
bash -c 'exec -a wine "$@"' _ \
  "$GLOBAL_CLIENT" wp_color_manager_v1 present "$COLOR_MANAGER_VERSION"

echo "wp_color_manager_v1 version 2 advertised normally; Wine compatibility version $COLOR_MANAGER_VERSION advertised"

# TOML cannot redefine [output.HEADLESS-1], so each phase below replaces the
# whole config rather than appending to the previous one. Holding the pristine
# config in a variable keeps that reset from needing a file on disk.
BASELINE=$(< "$UMBRIEL_CONFIG")

printf '\n[output.HEADLESS-1]\nhdr = "on"\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

if ! grep -F "output 'HEADLESS-1': HDR unavailable: display does not advertise PQ" "$UMBRIEL_LOG" > /dev/null; then
  echo "missing expected headless HDR fallback: display does not advertise PQ"
  exit 1
fi

foot --title=color-diagnostics sh -c 'sleep 120' > /dev/null 2>&1 &
CLIENT_PID=$!
for _ in $(seq 40); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.1
done

color=$("$UMBRIEL" color --json)
if ! jq -e '
  .color_manager == true
  and .renderer.input_color_transform == true
  and .renderer.output_color_transform == true
  and (.renderer.timeline | type) == "boolean"
  and (.outputs | length) == 1
  and .outputs[0].name == "HEADLESS-1"
  and .outputs[0].hdr_mode == "on"
  and .outputs[0].hdr_requested == true
  and .outputs[0].hdr_active == false
  and .outputs[0].fallback_reason == "display does not advertise PQ"
  and .outputs[0].render_format == "XR24"
  and .outputs[0].transfer_function == "none"
  and .outputs[0].primaries == "none"
  and .outputs[0].sdr_white == 203
  and (.outputs[0].supported_transfer_functions | type) == "array"
  and (.outputs[0].supported_primaries | type) == "array"
  and (.surfaces | length) == 1
  and .surfaces[0].title == "color-diagnostics"
  and .surfaces[0].transfer_function == "none"
  and .surfaces[0].primaries == "none"
  and .surfaces[0].mastering_display_primaries == null
  and .surfaces[0].mastering_luminance == null
  and .surfaces[0].max_cll == 0
  and .surfaces[0].max_fall == 0
' <<< "$color" > /dev/null; then
  echo "unexpected color diagnostics: $color"
  exit 1
fi

color_human=$("$UMBRIEL" color)
if ! grep -F "fallback: display does not advertise PQ" <<< "$color_human" > /dev/null \
    || ! grep -F "surface " <<< "$color_human" > /dev/null \
    || ! grep -F "mastering luminance: unset; MaxCLL: 0; MaxFALL: 0" <<< "$color_human" > /dev/null; then
  echo "unexpected human-readable color diagnostics: $color_human"
  exit 1
fi

kill -TERM "$CLIENT_PID"
wait "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=

CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/wine-scrgb-client.log"
env COLOR_WINDOWS_SCRGB=1 REDRAW_ON_CLOSE=once APP_ID=wine-scrgb \
  bash -c 'exec -a wine "$@"' _ \
  "$UNMAP_CLIENT" wine-scrgb > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "Wine scRGB client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi

for _ in $(seq 40); do
  color=$("$UMBRIEL" color --json)
  jq -e '
    .outputs[0].hdr_mode == "on"
    and .outputs[0].hdr_requested == true
    and (.surfaces[] | select(.title == "wine-scrgb")
      | .app_id == "wine-scrgb" and .transfer_function == "extended linear" and .primaries == "sRGB")
  ' <<< "$color" > /dev/null && break
  sleep 0.1
done
if ! jq -e '
  .outputs[0].hdr_requested == true
  and (.surfaces[] | select(.title == "wine-scrgb")
    | .app_id == "wine-scrgb" and .transfer_function == "extended linear" and .primaries == "sRGB")
' <<< "$color" > /dev/null; then
  echo "Wine scRGB metadata did not reach surface diagnostics: $color"
  exit 1
fi

# The overview mirrors client buffers into raw scene buffers. Their color must match the normal scene surface even
# after Wine's compatibility metadata is repaired at the render boundary.
normal_shot="$UMBRIEL_RUNTIME_DIR/wine-scrgb-normal.png"
overview_shot="$UMBRIEL_RUNTIME_DIR/wine-scrgb-overview.png"
window_json=$("$UMBRIEL" windows --json | jq '.[] | select(.title == "wine-scrgb")')
window_x=$(jq -r '.x' <<< "$window_json")
window_y=$(jq -r '.y' <<< "$window_json")
window_w=$(jq -r '.w' <<< "$window_json")
window_h=$(jq -r '.h' <<< "$window_json")
normal_x=$((window_x + window_w / 2))
normal_y=$((window_y + window_h / 2))
overview_x=$((320 + normal_x / 2))
overview_y=$((180 + normal_y / 2))

sample_rgb() {
  magick "$1" -alpha off -crop "20x20+$(($2 - 10))+$(($3 - 10))" +repage \
    -format '%[fx:round(255*mean.r)] %[fx:round(255*mean.g)] %[fx:round(255*mean.b)]\n' info:
}

# Both samples must come from settled windows: let the opening animation finish first.
"$UMBRIEL" settle
grim "$normal_shot"
"$UMBRIEL" msg overview-open > /dev/null
# The sample point assumes the settled half-scale card, so wait out the overview and card motion.
"$UMBRIEL" settle
wine_scrgb_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "wine-scrgb") | .id')
"$UMBRIEL" msg "window-close:$wine_scrgb_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^redrawn$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^redrawn$' "$CLIENT_LOG"; then
  echo "Wine scRGB client did not redraw inside overview: $(cat "$CLIENT_LOG")"
  exit 1
fi
"$UMBRIEL" settle
grim "$overview_shot"
read -r normal_r normal_g normal_b < <(sample_rgb "$normal_shot" "$normal_x" "$normal_y")
read -r overview_r overview_g overview_b < <(sample_rgb "$overview_shot" "$overview_x" "$overview_y")
for channel in r g b; do
  normal_var="normal_$channel"
  overview_var="overview_$channel"
  difference=$(( ${!normal_var} - ${!overview_var} ))
  (( difference < 0 )) && difference=$((-difference))
  if (( difference > 3 )); then
    echo "Wine scRGB overview color changed: normal=$normal_r,$normal_g,$normal_b overview=$overview_r,$overview_g,$overview_b"
    exit 1
  fi
done
"$UMBRIEL" msg overview-close > /dev/null

wine_scrgb_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "wine-scrgb") | .id')
"$UMBRIEL" msg "window-close:$wine_scrgb_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^unmapped$' "$CLIENT_LOG"; then
  echo "Wine scRGB client did not unmap"
  exit 1
fi

kill -TERM "$CLIENT_PID"
wait "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=

printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
printf '\n[output.HEADLESS-1]\nhdr = "auto"\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

color=$("$UMBRIEL" color --json)
if ! jq -e '
  .outputs[0].hdr_mode == "auto"
  and .outputs[0].hdr_requested == false
  and .outputs[0].hdr_active == false
  and .outputs[0].fallback_reason == ""
' <<< "$color" > /dev/null; then
  echo "automatic HDR did not remain idle without HDR content: $color"
  exit 1
fi

CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/wine-scrgb-auto-child-client.log"
env REQUEST_FULLSCREEN=1 COLOR_WINDOWS_SCRGB=1 COLOR_ON_SUBSURFACE=1 COLOR_CHILD_LIFECYCLE=1 \
  APP_ID=wine-scrgb-auto-child \
  bash -c 'exec -a wine "$@"' _ \
  "$UNMAP_CLIENT" wine-scrgb-auto-child > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "Wine scRGB child-surface client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi

color=$("$UMBRIEL" color --json)
if ! jq -e '
  .outputs[0].hdr_mode == "auto"
  and .outputs[0].hdr_requested == false
  and .outputs[0].hdr_active == false
  and .outputs[0].fallback_reason == ""
' <<< "$color" > /dev/null; then
  echo "unmapped Wine color child unexpectedly requested automatic HDR: $color"
  exit 1
fi

wine_scrgb_auto_child_id=$("$UMBRIEL" windows --json \
  | jq -r '.[] | select(.title == "wine-scrgb-auto-child") | .id')
"$UMBRIEL" msg "window-close:$wine_scrgb_auto_child_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^color-child-mapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
color=$("$UMBRIEL" color --json)
if ! grep -q '^color-child-mapped$' "$CLIENT_LOG" \
    || ! jq -e '
      .outputs[0].hdr_requested == false
      and .outputs[0].fallback_reason == ""
    ' <<< "$color" > /dev/null; then
  echo "Wine child without HDR metadata unexpectedly requested automatic HDR: $color"
  exit 1
fi

"$UMBRIEL" msg "window-close:$wine_scrgb_auto_child_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^color-child-hdr$' "$CLIENT_LOG" && break
  sleep 0.1
done
for _ in $(seq 40); do
  color=$("$UMBRIEL" color --json)
  jq -e '
    .outputs[0].hdr_mode == "auto"
    and .outputs[0].hdr_requested == true
    and .outputs[0].hdr_active == false
    and .outputs[0].fallback_reason == "display does not advertise PQ"
    and (.surfaces[] | select(.title == "wine-scrgb-auto-child")
      | .app_id == "wine-scrgb-auto-child"
        and .transfer_function == "extended linear"
        and .primaries == "sRGB")
  ' <<< "$color" > /dev/null && break
  sleep 0.1
done
if ! jq -e '
  .outputs[0].hdr_requested == true
  and .outputs[0].fallback_reason == "display does not advertise PQ"
  and (.surfaces[] | select(.title == "wine-scrgb-auto-child")
    | .app_id == "wine-scrgb-auto-child"
      and .transfer_function == "extended linear"
      and .primaries == "sRGB")
' <<< "$color" > /dev/null; then
  echo "fullscreen Wine scRGB child metadata did not request automatic HDR: $color"
  exit 1
fi

"$UMBRIEL" msg "window-close:$wine_scrgb_auto_child_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^color-child-unmapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
for _ in $(seq 40); do
  color=$("$UMBRIEL" color --json)
  jq -e '
    .outputs[0].hdr_mode == "auto"
    and .outputs[0].hdr_requested == false
    and .outputs[0].hdr_active == false
    and .outputs[0].fallback_reason == ""
  ' <<< "$color" > /dev/null && break
  sleep 0.1
done
if ! grep -q '^color-child-unmapped$' "$CLIENT_LOG" \
    || ! jq -e '
      .outputs[0].hdr_mode == "auto"
      and .outputs[0].hdr_requested == false
      and .outputs[0].hdr_active == false
      and .outputs[0].fallback_reason == ""
    ' <<< "$color" > /dev/null; then
  echo "automatic HDR did not release after the Wine scRGB child surface unmapped: $color"
  exit 1
fi

"$UMBRIEL" msg "window-close:$wine_scrgb_auto_child_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^unmapped$' "$CLIENT_LOG"; then
  echo "Wine scRGB child-surface client did not unmap"
  exit 1
fi

kill -TERM "$CLIENT_PID"
wait "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=

if ((COLOR_MANAGER_VERSION >= 3)); then
  CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/wine-bt2100-client.log"
  env REQUEST_FULLSCREEN=1 COLOR_WINDOWS_BT2100=1 APP_ID=wine-bt2100 \
    bash -c 'exec -a wine "$@"' _ \
    "$UNMAP_CLIENT" wine-bt2100 > "$CLIENT_LOG" 2>&1 &
  CLIENT_PID=$!
  for _ in $(seq 60); do
    grep -q '^mapped$' "$CLIENT_LOG" && break
    sleep 0.1
  done
  if ! grep -q '^mapped$' "$CLIENT_LOG"; then
    echo "Wine BT.2100 client never mapped: $(cat "$CLIENT_LOG")"
    exit 1
  fi

  for _ in $(seq 40); do
    color=$("$UMBRIEL" color --json)
    jq -e '
      .outputs[0].hdr_mode == "auto"
      and .outputs[0].hdr_requested == true
      and .outputs[0].hdr_active == false
      and .outputs[0].fallback_reason == "display does not advertise PQ"
      and (.surfaces[] | select(.title == "wine-bt2100")
        | .app_id == "wine-bt2100"
          and .transfer_function == "PQ"
          and .primaries == "BT.2020"
          and .mastering_display_primaries == null
          and .mastering_luminance == null)
    ' <<< "$color" > /dev/null && break
    sleep 0.1
  done
  if ! jq -e '
    .outputs[0].hdr_requested == true
    and .outputs[0].fallback_reason == "display does not advertise PQ"
    and (.surfaces[] | select(.title == "wine-bt2100")
      | .app_id == "wine-bt2100"
        and .transfer_function == "PQ"
        and .primaries == "BT.2020"
        and .mastering_display_primaries == null
        and .mastering_luminance == null)
  ' <<< "$color" > /dev/null; then
    echo "Wine BT.2100 metadata did not request automatic HDR: $color"
    exit 1
  fi

  wine_bt2100_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "wine-bt2100") | .id')
  "$UMBRIEL" msg "window-close:$wine_bt2100_id" > /dev/null
  for _ in $(seq 40); do
    grep -q '^unmapped$' "$CLIENT_LOG" && break
    sleep 0.1
  done
  color=$("$UMBRIEL" color --json)
  if ! grep -q '^unmapped$' "$CLIENT_LOG" \
      || ! jq -e '
        .outputs[0].hdr_mode == "auto"
        and .outputs[0].hdr_requested == false
        and .outputs[0].hdr_active == false
        and .outputs[0].fallback_reason == ""
      ' <<< "$color" > /dev/null; then
    echo "automatic HDR did not release after Wine BT.2100 content unmapped: $color"
    exit 1
  fi

  kill -TERM "$CLIENT_PID"
  wait "$CLIENT_PID" 2>/dev/null || true
  CLIENT_PID=
fi

CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/auto-hdr-client.log"
env REQUEST_FULLSCREEN=1 COLOR_HDR=1 \
  "$UNMAP_CLIENT" auto-hdr > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "automatic HDR client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi

for _ in $(seq 40); do
  color=$("$UMBRIEL" color --json)
  jq -e '
    .outputs[0].hdr_mode == "auto"
    and .outputs[0].hdr_requested == true
    and .outputs[0].hdr_active == false
    and .outputs[0].fallback_reason == "display does not advertise PQ"
    and (.surfaces[] | select(.title == "auto-hdr")
      | .transfer_function == "PQ" and .primaries == "BT.2020")
  ' <<< "$color" > /dev/null && break
  sleep 0.1
done
if ! jq -e '
  .outputs[0].hdr_requested == true
  and .outputs[0].fallback_reason == "display does not advertise PQ"
  and (.surfaces[] | select(.title == "auto-hdr")
    | .transfer_function == "PQ" and .primaries == "BT.2020")
' <<< "$color" > /dev/null; then
  echo "fullscreen PQ content did not request automatic HDR: $color"
  exit 1
fi

auto_hdr_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "auto-hdr") | .id')
"$UMBRIEL" msg "window-close:$auto_hdr_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
color=$("$UMBRIEL" color --json)
if ! grep -q '^unmapped$' "$CLIENT_LOG" \
    || ! jq -e '
      .outputs[0].hdr_mode == "auto"
      and .outputs[0].hdr_requested == false
      and .outputs[0].hdr_active == false
      and .outputs[0].fallback_reason == ""
    ' <<< "$color" > /dev/null; then
  echo "automatic HDR did not release after its owner unmapped: $color"
  exit 1
fi

kill -TERM "$CLIENT_PID"
wait "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=

printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
printf '\n[output.HEADLESS-1]\nhdr = "fullscreen"\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

color=$("$UMBRIEL" color --json)
if ! jq -e '
  .outputs[0].hdr_mode == "fullscreen"
  and .outputs[0].hdr_requested == false
  and .outputs[0].hdr_active == false
  and .outputs[0].fallback_reason == ""
' <<< "$color" > /dev/null; then
  echo "fullscreen HDR did not remain idle without fullscreen content: $color"
  exit 1
fi

CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/fullscreen-hdr-client.log"
env REQUEST_FULLSCREEN=1 \
  "$UNMAP_CLIENT" fullscreen-hdr > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "fullscreen HDR client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi

for _ in $(seq 40); do
  color=$("$UMBRIEL" color --json)
  jq -e '
    .outputs[0].hdr_mode == "fullscreen"
    and .outputs[0].hdr_requested == true
    and .outputs[0].hdr_active == false
    and .outputs[0].fallback_reason == "display does not advertise PQ"
    and (.surfaces[] | select(.title == "fullscreen-hdr")
      | .transfer_function == "none" and .primaries == "none")
  ' <<< "$color" > /dev/null && break
  sleep 0.1
done
if ! jq -e '
  .outputs[0].hdr_requested == true
  and .outputs[0].fallback_reason == "display does not advertise PQ"
  and (.surfaces[] | select(.title == "fullscreen-hdr")
    | .transfer_function == "none" and .primaries == "none")
' <<< "$color" > /dev/null; then
  echo "untagged fullscreen content did not request fullscreen HDR: $color"
  exit 1
fi

fullscreen_hdr_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "fullscreen-hdr") | .id')
"$UMBRIEL" msg "window-close:$fullscreen_hdr_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
color=$("$UMBRIEL" color --json)
if ! grep -q '^unmapped$' "$CLIENT_LOG" \
    || ! jq -e '
      .outputs[0].hdr_mode == "fullscreen"
      and .outputs[0].hdr_requested == false
      and .outputs[0].hdr_active == false
      and .outputs[0].fallback_reason == ""
    ' <<< "$color" > /dev/null; then
  echo "fullscreen HDR did not release after fullscreen content unmapped: $color"
  exit 1
fi

kill -TERM "$CLIENT_PID"
wait "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=

printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
printf '\n[output.HEADLESS-1]\nhdr = "off"\n\n[[window_rule]]\nmatch.app_id = "^hdr-rule-on$"\nhdr = "on"\n' \
  >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

"$POINTER_CLIENT" 1280 720 pause 30000 tap 30 > /dev/null 2>&1 &
sleep 0.1

CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/hdr-rule-on-client.log"
env APP_ID=hdr-rule-on \
  "$UNMAP_CLIENT" hdr-rule-on > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
hdr_rule_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "hdr-rule-on") | .id')
"$UMBRIEL" msg "window-focus:$hdr_rule_id" > /dev/null
for _ in $(seq 40); do
  color=$("$UMBRIEL" color --json)
  jq -e '
    .outputs[0].hdr_mode == "off"
    and .outputs[0].hdr_requested == true
    and .outputs[0].fallback_reason == "display does not advertise PQ"
  ' <<< "$color" > /dev/null && break
  sleep 0.1
done
if ! jq -e '
  .outputs[0].hdr_mode == "off"
  and .outputs[0].hdr_requested == true
  and .outputs[0].fallback_reason == "display does not advertise PQ"
' <<< "$color" > /dev/null; then
  echo "focused window HDR rule did not override the disabled output policy: $color"
  exit 1
fi

"$UMBRIEL" msg "window-close:$hdr_rule_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
for _ in $(seq 40); do
  color=$("$UMBRIEL" color --json)
  jq -e '
    .outputs[0].hdr_requested == false
    and .outputs[0].fallback_reason == ""
  ' <<< "$color" > /dev/null && break
  sleep 0.1
done
if ! jq -e '
  .outputs[0].hdr_requested == false
  and .outputs[0].fallback_reason == ""
' <<< "$color" > /dev/null; then
  echo "output HDR policy did not resume after the enabling window rule unmapped: $color"
  exit 1
fi

kill -TERM "$CLIENT_PID"
wait "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=

printf '%s\n' "$BASELINE" > "$UMBRIEL_CONFIG"
printf '\n[output.HEADLESS-1]\nhdr = "on"\n\n[[window_rule]]\nmatch.app_id = "^hdr-rule-off$"\nhdr = "off"\n' \
  >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/hdr-rule-off-client.log"
env APP_ID=hdr-rule-off \
  "$UNMAP_CLIENT" hdr-rule-off > "$CLIENT_LOG" 2>&1 &
CLIENT_PID=$!
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
hdr_rule_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "hdr-rule-off") | .id')
"$UMBRIEL" msg "window-focus:$hdr_rule_id" > /dev/null
for _ in $(seq 40); do
  color=$("$UMBRIEL" color --json)
  jq -e '
    .outputs[0].hdr_mode == "on"
    and .outputs[0].hdr_requested == false
    and .outputs[0].fallback_reason == ""
  ' <<< "$color" > /dev/null && break
  sleep 0.1
done
if ! jq -e '
  .outputs[0].hdr_mode == "on"
  and .outputs[0].hdr_requested == false
  and .outputs[0].fallback_reason == ""
' <<< "$color" > /dev/null; then
  echo "focused window HDR rule did not override the enabled output policy: $color"
  exit 1
fi

"$UMBRIEL" msg "window-close:$hdr_rule_id" > /dev/null
for _ in $(seq 40); do
  grep -q '^unmapped$' "$CLIENT_LOG" && break
  sleep 0.1
done
for _ in $(seq 40); do
  color=$("$UMBRIEL" color --json)
  jq -e '
    .outputs[0].hdr_requested == true
    and .outputs[0].fallback_reason == "display does not advertise PQ"
  ' <<< "$color" > /dev/null && break
  sleep 0.1
done
if ! jq -e '
  .outputs[0].hdr_requested == true
  and .outputs[0].fallback_reason == "display does not advertise PQ"
' <<< "$color" > /dev/null; then
  echo "enabled output HDR policy did not resume after the disabling window rule unmapped: $color"
  exit 1
fi

echo "HDR diagnostics, Wine scRGB, automatic and fullscreen transitions, and focused window overrides verified"

kill -TERM "$CLIENT_PID"
wait "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=

# Exercise SDR render-format transitions with a live window and effects. The
# 10-bit and returning 8-bit frames must match the initial 8-bit frame while
# the configured backdrop stays correctly encoded in every capture.
FX_CONFIG='
[colors]
backdrop = "#1e1e2eff"

[appearance]
corner_radius = 32

[appearance.blur]
enabled = true
optimized = true
passes = 2
radius = 8
noise = 0.0
brightness = 1.0
contrast = 1.0
saturation = 1.0

[[window_rule]]
blur = true'
SDR10_CONFIG='
[output.HEADLESS-1]
bit_depth = 10'
SDR10_ACTIVE='
  .outputs[0].bit_depth == 10
  and .outputs[0].bit_depth_fallback_reason == ""
  and .outputs[0].bit_depth_active == true
  and (.outputs[0].render_format == "XR30" or .outputs[0].render_format == "XB30")'

write_color_config() {
  printf '%s\n%s\n' "$BASELINE" "${1:-}" > "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}

expect_color_state() {
  local label=$1 filter=$2
  local state
  state=$("$UMBRIEL" color --json)
  if ! jq -e "$filter" <<< "$state" > /dev/null; then
    echo "$label: unexpected color state: $state"
    exit 1
  fi
}

expect_rgb_near() {
  local label=$1 tolerance=$4
  local -a actual expected
  read -ra actual <<< "$2"
  read -ra expected <<< "$3"
  local i
  for i in 0 1 2; do
    if (( ${actual[i]} - ${expected[i]} > tolerance || ${expected[i]} - ${actual[i]} > tolerance )); then
      echo "$label: got rgb $2, expected $3 (tolerance $tolerance)"
      exit 1
    fi
  done
}

expect_rendered() {
  local label=$1
  local screenshot="$UMBRIEL_RUNTIME_DIR/sdr10-$label.png"
  local lit backdrop
  "$UMBRIEL" settle
  grim "$screenshot"
  lit=$("$UMBRIEL_PIXEL_PROBE" "$screenshot" count 'r > 0 || g > 0 || b > 0')
  if (( lit == 0 )); then
    echo "$label: screenshot is all-black"
    exit 1
  fi
  backdrop=$("$UMBRIEL_PIXEL_PROBE" "$screenshot" pixel 2 2)
  expect_rgb_near "$label backdrop at 2,2" "$backdrop" "30 30 46" 2
  FRAME_MEAN=$("$UMBRIEL_PIXEL_PROBE" "$screenshot" mean)
  echo "$label: frame rendered with blur/corner_radius active (backdrop=${backdrop}, mean=${FRAME_MEAN})"
}

write_color_config "$FX_CONFIG"
foot --config=/dev/null sh -c 'while :; do sleep 1; done' > /dev/null 2>&1 &
CLIENT_PID=$!
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -ge 1 ]] && break
  sleep 0.1
done
if [[ $("$UMBRIEL" windows --json | jq 'length') -lt 1 ]]; then
  echo "SDR10: foot window never mapped"
  exit 1
fi

expect_rendered sdr8
SDR8_MEAN=$FRAME_MEAN
write_color_config "$FX_CONFIG"$'\n'"$SDR10_CONFIG"
expect_rendered sdr10
expect_rgb_near "sdr10 frame mean against sdr8" "$FRAME_MEAN" "$SDR8_MEAN" 2
write_color_config "$FX_CONFIG"
expect_rendered sdr8-return
expect_rgb_near "sdr8-return frame mean against sdr8" "$FRAME_MEAN" "$SDR8_MEAN" 2

write_color_config "$SDR10_CONFIG"
expect_color_state sdr8-to-sdr10 "$SDR10_ACTIVE"
if ! grep -F '10-bit SDR: active' <<< "$("$UMBRIEL" color)" > /dev/null; then
  echo "sdr8-to-sdr10: missing active status in human color output"
  exit 1
fi

# Re-enabling the output must re-select the 10-bit format, not just retain the
# configured value while the output is powered off.
"$UMBRIEL" msg dpms-off > /dev/null
expect_color_state dpms-off '
  .outputs[0].bit_depth == 10
  and .outputs[0].bit_depth_active == false'
"$UMBRIEL" msg dpms-on > /dev/null
"$UMBRIEL" settle
expect_color_state dpms-on "$SDR10_ACTIVE"
echo "dpms-cycle: XR30/XB30 re-selected after DPMS off/on"

write_color_config $'\n[output.HEADLESS-1]\nhdr = "on"'
expect_color_state hdr-to-sdr10-setup '
  .outputs[0].hdr_requested == true
  and .outputs[0].hdr_active == false
  and (.outputs[0].fallback_reason | length) > 0
  and .outputs[0].render_format == "XR24"'

write_color_config "$SDR10_CONFIG"
expect_color_state hdr-to-sdr10 "$SDR10_ACTIVE"'
  and .outputs[0].hdr_requested == false
  and .outputs[0].hdr_active == false
  and .outputs[0].fallback_reason == ""
  and .outputs[0].transfer_function == "none"
  and .outputs[0].primaries == "none"'

write_color_config "$SDR10_CONFIG"$'\nhdr = "on"'
expect_color_state hdr-unavailable-with-sdr10 "$SDR10_ACTIVE"'
  and .outputs[0].hdr_requested == true
  and .outputs[0].hdr_active == false
  and (.outputs[0].fallback_reason | length) > 0'
if ! grep -F '10-bit SDR: active' <<< "$("$UMBRIEL" color)" > /dev/null; then
  echo "hdr-unavailable-with-sdr10: missing active status in human color output"
  exit 1
fi

write_color_config
expect_color_state sdr10-to-sdr8 '
  .outputs[0].bit_depth == 8
  and .outputs[0].bit_depth_fallback_reason == ""
  and .outputs[0].bit_depth_active == false
  and .outputs[0].render_format == "XR24"'
echo "10-bit SDR rendering, HDR fallback, DPMS re-selection and SDR8 return verified"
