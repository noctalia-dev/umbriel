#!/usr/bin/env bash
# harness: outputs=2
# Scene containment across a shared output edge. This scrolls one output's strip until columns hang off the shared edge,
# physically on top of the neighbour, then asserts from real framebuffers that the neighbour is byte-identical to its
# empty baseline (nothing bled), that the home output did change (content is drawn), and that content still reaches the
# right side of the home strip (the per-output clip did not cut it short). Every assertion is reported rather than
# aborting on the first, because which of them fails localises the regression.
set -euo pipefail

rc=0

shot() { grim -o "$1" "$2"; }

# Animations (overview zoom, card motion) mean a single grab can catch a moving frame and make comparisons flaky. Grab
# until two consecutive frames 0.25s apart match, so every baseline and result below is a settled frame. That gap is
# wider than the 200ms default animation, so this loop is the barrier: callers need only a short primer to guarantee the
# animation has started, never a wait sized to the animation itself.
shot_settled() {
  local output=$1 dest=$2 previous=$UMBRIEL_RUNTIME_DIR/.settle.png
  shot "$output" "$previous"
  for _ in $(seq 24); do
    sleep 0.25
    shot "$output" "$dest"
    cmp -s "$previous" "$dest" && return 0
    mv "$dest" "$previous"
  done
  echo "  $output never settled"
  return 1
}

spawn() {
  foot --title="$1" sh -c 'sleep 300' > /dev/null 2>&1 &
}

wait_windows() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $1 ]] && return 0
    sleep 0.25
  done
  echo "expected $1 window(s), got $("$UMBRIEL" windows --json | jq 'length')"
  return 1
}

output_x() {
  "$UMBRIEL" outputs | awk -v name="$1" '$1 == name {found = 1; next} found && /Position:/ {split($2, p, ","); print p[1]; exit}'
}

# Mean of a crop, as a stable fingerprint of one screen region.
region_mean() {
  magick "$1" -crop "$2" -colorspace RGB -format '%[fx:mean]' info:
}

check() {
  if [[ $2 == "$3" ]]; then
    echo "  ok   $1"
  else
    echo "  FAIL $1 (got '$2', want '$3')"
    rc=1
  fi
}

spawn probe
wait_windows 1
home=$("$UMBRIEL" windows --json | jq -r '.[0].workspace' | cut -d: -f1)
if [[ $home == HEADLESS-1 ]]; then neighbour=HEADLESS-2; else neighbour=HEADLESS-1; fi
home_x=$(output_x "$home")
echo "windows land on $home (x=$home_x), watching $neighbour (x=$(output_x "$neighbour"))"

sleep 0.3
shot_settled "$neighbour" "$UMBRIEL_RUNTIME_DIR/neighbour-base.png"
shot_settled "$home" "$UMBRIEL_RUNTIME_DIR/home-base.png"
# The rightmost on-strip column ends just short of the shared edge, so this crop is window content when the strip is
# populated and backdrop when it is empty.
edge_crop=20x600+1246+60
home_base_edge=$(region_mean "$UMBRIEL_RUNTIME_DIR/home-base.png" "$edge_crop")

# Overview baseline while the home output holds a single card: its filmstrip cannot overhang the shared edge yet, so
# this is the neighbour showing nothing but its own filmstrip.
"$UMBRIEL" msg overview-open > /dev/null
sleep 0.3
shot_settled "$neighbour" "$UMBRIEL_RUNTIME_DIR/neighbour-ov-base.png"
"$UMBRIEL" msg overview-close > /dev/null

# 624-wide columns on a 1280-wide output fit two at a time. Focusing the leftmost column scrolls the surplus off the
# RIGHT edge, which in a side-by-side layout is exactly where the neighbouring output lives.
for i in 2 3 4 5; do spawn "bleed-$i"; done
wait_windows 5
for _ in $(seq 6); do "$UMBRIEL" msg window-focus-left > /dev/null; sleep 0.2; done
sleep 0.3

edge=$((home_x + 1280))
"$UMBRIEL" windows --json | jq -c '[.[] | {title, x, w}] | sort_by(.x)'
over=$("$UMBRIEL" windows --json | jq --argjson e "$edge" '[.[] | select(.x + .w > $e)] | length')
echo "columns reaching past the shared edge at x=$edge: $over"
[[ $over -gt 0 ]] || {
  echo "SETUP FAIL: nothing reaches past the shared edge"
  exit 1
}

shot_settled "$neighbour" "$UMBRIEL_RUNTIME_DIR/neighbour-full.png"
shot_settled "$home" "$UMBRIEL_RUNTIME_DIR/home-full.png"

neighbour_same=$(cmp -s "$UMBRIEL_RUNTIME_DIR/neighbour-base.png" "$UMBRIEL_RUNTIME_DIR/neighbour-full.png" && echo same || echo changed)
home_same=$(cmp -s "$UMBRIEL_RUNTIME_DIR/home-base.png" "$UMBRIEL_RUNTIME_DIR/home-full.png" && echo same || echo changed)
home_full_edge=$(region_mean "$UMBRIEL_RUNTIME_DIR/home-full.png" "$edge_crop")
edge_state=$([[ $home_base_edge == "$home_full_edge" ]] && echo same || echo changed)

check "no bleed onto $neighbour" "$neighbour_same" same
check "content drawn on $home" "$home_same" changed
check "content reaches the strip edge on the home output" "$edge_state" changed

# Transitions are where containment used to be re-derived per move: a workspace slide, a fullscreen enter/leave, and a
# focus scroll all move nodes while columns hang over the neighbour. Sample the neighbour after each step; it must never
# change, including mid-animation.
transition_drift=0
for action in window-toggle-fullscreen window-toggle-fullscreen workspace-next workspace-previous \
  window-focus-right window-focus-left column-move-right column-move-left; do
  "$UMBRIEL" msg "$action" > /dev/null
  for _ in 1 2 3; do
    sleep 0.08
    shot "$neighbour" "$UMBRIEL_RUNTIME_DIR/neighbour-step.png"
    if ! cmp -s "$UMBRIEL_RUNTIME_DIR/neighbour-base.png" "$UMBRIEL_RUNTIME_DIR/neighbour-step.png"; then
      echo "  drift after $action"
      transition_drift=$((transition_drift + 1))
      cp "$UMBRIEL_RUNTIME_DIR/neighbour-step.png" "$UMBRIEL_RUNTIME_DIR/drift-$action.png"
      break
    fi
  done
done
check "no bleed onto $neighbour across transitions" "$transition_drift" 0

# Same output, now with a populated and scrolled strip: cards for off-strip columns land past the shared edge. The
# neighbour must look exactly as it did with one card next door.
"$UMBRIEL" msg overview-open > /dev/null
sleep 0.3
shot_settled "$neighbour" "$UMBRIEL_RUNTIME_DIR/neighbour-ov-full.png"
overview_same=$(cmp -s "$UMBRIEL_RUNTIME_DIR/neighbour-ov-base.png" "$UMBRIEL_RUNTIME_DIR/neighbour-ov-full.png" && echo same || echo changed)
"$UMBRIEL" msg overview-close > /dev/null
check "no bleed onto $neighbour with the overview open" "$overview_same" same

# A failing check keeps its runtime directory, so the framebuffers above are
# already where the harness will point.
exit "$rc"
