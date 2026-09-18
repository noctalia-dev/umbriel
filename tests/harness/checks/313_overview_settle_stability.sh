#!/usr/bin/env bash
# A critically damped overview row must reach its final rendered position and remain there at the fractional-scale,
# high-refresh geometry from issue 236. The animation unit check observes every solver tick; concurrent screencopy
# requests can coalesce onto a subset of output frames, so this check covers the physical wheel and overview projection.
set -euo pipefail

readonly OUTPUT_W=2560
readonly OUTPUT_H=1600
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/tests/workspace-client}"

if [[ ! -x $POINTER ]]; then
  echo "pointer client not built at $POINTER"
  exit 1
fi
if [[ ! -x $WORKSPACE ]]; then
  echo "workspace client not built at $WORKSPACE"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation.overview]
workspace_curve = "spring:1,1000"

[overview]
zoom = 0.5

[output."HEADLESS-1"]
mode = "2560x1600@165"
scale = 1.5
workspaces = 5
EOF
"$UMBRIEL" msg config-reload > /dev/null

output=$("$UMBRIEL" outputs --json)
if ! jq -e '
  length == 1
  and .[0].name == "HEADLESS-1"
  and .[0].scale == 1.5
  and any(.[0].modes[];
    .current and .width == 2560 and .height == 1600 and .refresh_mhz == 165000)
' <<< "$output" > /dev/null; then
  echo "reported output geometry was not applied: $output"
  exit 1
fi

"$UMBRIEL_UNMAP_CLIENT" overview-settle 1200 700 > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]] && break
  sleep 0.05
done
[[ $("$UMBRIEL" windows --json | jq 'length') -eq 1 ]]
"$UMBRIEL" msg window-move-to-workspace:5 > /dev/null

# Record the exact settled destination with workspace 5 already active.
"$UMBRIEL" msg workspace-switch:5 > /dev/null
sleep 0.4
"$UMBRIEL" msg overview-open > /dev/null
sleep 0.6
grim "$UMBRIEL_RUNTIME_DIR/settle-target.png"
"$UMBRIEL" msg overview-close > /dev/null
sleep 0.4

# Return to workspace 1, reopen the overview, and cross four rows with the same physical wheel path as the report.
# Queue captures around the spring tail; each grim waits independently for a frame instead of delaying the next sample.
"$UMBRIEL" msg workspace-switch:1 > /dev/null
sleep 0.4
"$UMBRIEL" msg overview-open > /dev/null
sleep 0.6
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move $((OUTPUT_W / 2)) $((OUTPUT_H / 2)) \
  notch 1 notch 1 notch 1 notch 1
if [[ $("$WORKSPACE") != 5 ]]; then
  echo "four wheel notches did not activate workspace 5"
  exit 1
fi

capture_pids=()
for sample in $(seq 0 35); do
  delay=$(awk -v sample="$sample" 'BEGIN { printf "%.3f", 0.18 + sample * 0.007 }')
  (sleep "$delay"; grim "$UMBRIEL_RUNTIME_DIR/settle-$sample.png") &
  capture_pids+=("$!")
done
for pid in "${capture_pids[@]}"; do
  wait "$pid"
done

card_y() {
  magick "$1" -alpha on -fuzz 1% -fill none +opaque '#5577aa' -trim miff:- |
    identify -format '%Y\n' -
}

target_y=$(card_y "$UMBRIEL_RUNTIME_DIR/settle-target.png")
previous_y=$(card_y "$UMBRIEL_RUNTIME_DIR/settle-0.png")
[[ $previous_y -ne $target_y ]]
arrived=0
transitions=0
for sample in $(seq 1 35); do
  y=$(card_y "$UMBRIEL_RUNTIME_DIR/settle-$sample.png")
  if [[ $y -eq $target_y ]]; then
    arrived=1
  elif ((arrived)); then
    echo "overview preview left its settled position again (frame $sample y $y, target $target_y)"
    exit 1
  fi
  [[ $y -ne $previous_y ]] && transitions=$((transitions + 1))
  previous_y=$y
done
((arrived))
((transitions >= 3))

echo 'physical wheel spring reaches and holds its settled overview row'
