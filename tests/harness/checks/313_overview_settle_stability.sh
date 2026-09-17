#!/usr/bin/env bash
# A critically damped overview row must reach its final rendered position and remain there at the fractional-scale,
# high-refresh geometry from issue 236. The animation unit check observes every quantized tail tick; concurrent
# screencopy requests can coalesce onto a subset of output frames, so this check covers the real overview projection.
set -euo pipefail

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation.overview]
workspace_curve = "spring:1,1000"

[output."HEADLESS-1"]
mode = "2560x1600@165"
scale = 1.5
workspaces = 5
EOF
"$UMBRIEL" msg config-reload > /dev/null

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

# Return to workspace 1, reopen the overview, and switch across several rows. Queue captures around the spring tail;
# each grim waits independently for a frame instead of delaying the next sample.
"$UMBRIEL" msg workspace-switch:1 > /dev/null
sleep 0.4
"$UMBRIEL" msg overview-open > /dev/null
sleep 0.6
"$UMBRIEL" msg workspace-switch:5 > /dev/null

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

echo 'overview spring reaches its settled row one logical pixel at a time'
