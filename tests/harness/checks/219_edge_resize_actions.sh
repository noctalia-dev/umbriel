#!/usr/bin/env bash
# harness: outputs=1
# The edge-anchored resize actions move only the named edge and leave the opposite
# one where it is, so a positive delta always grows the window from that edge. That
# is what separates them from window-modify-width/height, which resize around the
# layout's own anchor.
#
# The height coverage uses the middle window of a three-window column: with two
# windows each sitting at one end of a fixed-span pair the two behaviours are
# geometrically identical, so only a middle window tells them apart (the unanchored
# action drags both neighbours along, the anchored one moves exactly one of them).
# Dwindle's splits cover the width directions, where the edge facing the screen is
# the one that has to stay put, and a floating window covers the guards the
# fraction resize path applies: the fullscreen refusal, asserted on the client's
# own presentation log because the compositor corrects the geometry either way;
# the 0.1 size floor and the usable-extent ceiling; a dropped maximize; the height
# directions; the presentation animating with the opposite edge holding for the
# whole resize rather than being placed for a size the client has not committed
# yet; and a client that answers the configure with its own size (HOLD_SIZE), where
# that edge has to follow the committed geometry and not the requested one.
set -euo pipefail

readonly CLIENT="${UMBRIEL_FRACTIONAL_CLIENT:-./build-debug/tests/fractional-client}"
readonly OUTPUT_W=1280

pids=()

spawn_client() {
  "$CLIENT" "$1" "${2:-700}" "${3:-700}" > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  pids+=("$!")
}

count() { "$UMBRIEL" windows --json | jq 'length'; }

box() {
  "$UMBRIEL" windows --json | jq -r --arg t "$1" '.[] | select(.title == $t) | "\(.x) \(.y) \(.w) \(.h)"'
}

focus() {
  "$UMBRIEL" msg "window-focus:$("$UMBRIEL" windows --json | jq -r --arg t "$1" '.[] | select(.title == $t) | .id')" > /dev/null
  sleep 0.2
}

wait_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $(count) == "$want" ]] && return 0
    sleep 0.1
  done
  echo "expected $want window(s), got $(count)"
  return 1
}

# The fractional client ignores xdg_toplevel.close, the way 178 works around it:
# signal the processes instead of asking the compositor to close them.
close_all() {
  local pid
  for pid in "${pids[@]}"; do
    kill "$pid" 2>/dev/null || true
  done
  pids=()
  for _ in $(seq 60); do
    [[ $(count) == 0 ]] && return 0
    sleep 0.1
  done
  echo "could not close every window ($(count) left)"
  return 1
}

# `[general]` only: a phase that declares `[animation]` itself would otherwise
# append a second table of that name, which makes the reload drop the whole file.
reset_config() {
  cat > "$UMBRIEL_CONFIG" <<'EOF'
[general]
xwayland = false
show_cheatsheet = false
autostart = []
EOF
  cat >> "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  sleep 0.2
}

until_true() {
  local desc=$1 predicate=$2
  for _ in $(seq 60); do
    if "$predicate"; then
      return 0
    fi
    sleep 0.1
  done
  echo "timed out waiting for $desc"
  echo "  windows: $("$UMBRIEL" windows --json)"
  return 1
}

# --- height: the middle window of a three-window column ----------------------

close_all
reset_config <<'EOF'

[animation]
enabled = false
EOF

spawn_client col-a
wait_count 1
spawn_client col-b
wait_count 2
"$UMBRIEL" msg window-consume-left > /dev/null
sleep 0.3
spawn_client col-c
wait_count 3
"$UMBRIEL" msg window-consume-left > /dev/null
sleep 0.3

# The column has split its extent once the stacked heights differ.
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq -r '[.[].h] | unique | length') -gt 1 ]] && break
  sleep 0.1
done

read -r _ upper_y _ upper_h < <(box col-a)
read -r _ mid_y _ mid_h < <(box col-b)
read -r _ lower_y _ lower_h < <(box col-c)
if ! (( upper_y < mid_y && mid_y < lower_y )); then
  echo "expected three windows stacked top to bottom, got y=$upper_y/$mid_y/$lower_y"
  exit 1
fi
echo "column of three: upper=$(box col-a) middle=$(box col-b) lower=$(box col-c)"

focus col-b
before_upper=$(box col-a)
before_lower=$(box col-c)

# Down: the middle window keeps its top edge and takes the room from below, so the
# upper neighbour must not move at all (the unanchored action would shrink it).
"$UMBRIEL" msg window-modify-height-down:0.1 > /dev/null
height_down_ok() {
  read -r _ y _ h < <(box col-b)
  read -r _ ly _ lh < <(box col-c)
  [[ $(box col-a) == "$before_upper" ]] \
    && (( y == mid_y && h > mid_h && ly > lower_y && lh < lower_h ))
}
until_true "height-down to pin the middle window's top edge and move only the lower neighbour" height_down_ok

# Up: the bottom edge (y + h) stays put and this time only the upper neighbour moves.
# The lower neighbour is compared against its post-down geometry, not the value read
# before the down press: that press already moved it.
read -r _ mid_down_y _ mid_down_h < <(box col-b)
read -r _ _ _ upper_down_h < <(box col-a)
after_down_lower=$(box col-c)
"$UMBRIEL" msg window-modify-height-up:0.1 > /dev/null
height_up_ok() {
  read -r _ y _ h < <(box col-b)
  read -r _ _ _ uh < <(box col-a)
  [[ $(box col-c) == "$after_down_lower" ]] \
    && (( y < mid_down_y && h > mid_down_h && y + h == mid_down_y + mid_down_h && uh < upper_down_h ))
}
until_true "height-up to pin the middle window's bottom edge and move only the upper neighbour" height_up_ok

# --- dwindle: screen-facing edges are inert, owned splits still resize -------

close_all
reset_config <<'EOF'

[animation]
enabled = false

[layout]
mode = "dwindle"
EOF

spawn_client dwl-a
wait_count 1
spawn_client dwl-b
wait_count 2
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq -r '[.[].x] | unique | length') -eq 2 ]] && break
  sleep 0.1
done

read -r a_x _ _ _ < <(box dwl-a)
read -r b_x _ _ _ < <(box dwl-b)
if (( a_x < b_x )); then
  left=dwl-a
  right=dwl-b
else
  left=dwl-b
  right=dwl-a
fi

# A screen-facing edge has no boundary, so nothing may change -- including the
# window's maximize-to-edges state, since unmaximizing here would move it.
focus "$left"
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
sleep 0.4
before=$(box "$left")
"$UMBRIEL" msg window-modify-width-left:0.1 > /dev/null
sleep 0.5
if [[ $(box "$left") != "$before" ]]; then
  echo "width-left changed the geometry of a screen-facing edge: $before -> $(box "$left")"
  exit 1
fi
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
sleep 0.4

# The split it owns still resizes, pinning that screen edge.
read -r before_x _ before_w _ < <(box "$left")
"$UMBRIEL" msg window-modify-width-right:0.1 > /dev/null
left_split_ok() {
  read -r x _ w _ < <(box "$left")
  (( x == before_x && w > before_w ))
}
until_true "width-right to resize the dwindle split with the left screen edge pinned" left_split_ok

# The right window's own screen-facing edge is inert too, and its split edge pins
# the right screen edge (x + w unchanged).
focus "$right"
before=$(box "$right")
"$UMBRIEL" msg window-modify-width-right:0.1 > /dev/null
sleep 0.5
if [[ $(box "$right") != "$before" ]]; then
  echo "width-right changed the geometry of a screen-facing edge: $before -> $(box "$right")"
  exit 1
fi

read -r before_x _ before_w _ < <(box "$right")
"$UMBRIEL" msg window-modify-width-left:0.1 > /dev/null
right_split_ok() {
  read -r x _ w _ < <(box "$right")
  (( x < before_x && w > before_w && x + w == before_x + before_w ))
}
until_true "width-left to pin the right screen edge" right_split_ok

# --- floating: the guards the fraction path applies --------------------------

close_all
# The guards first, with animation off so every box below has settled; the
# animation sample at the end reloads the config for a duration it can observe.
reset_config <<'EOF'

[animation]
enabled = false
EOF

spawn_client float-a 700 700
wait_count 1
"$UMBRIEL" msg window-toggle-floating > /dev/null
sleep 0.4

# A fullscreen view owns its size: refuse rather than send a resize configure. The
# harness client reports every presentation it makes, so a configure that the
# compositor then corrects still shows up as another `mapped` line, which the
# geometry comparison alone would miss.
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
sleep 0.5
before=$(box float-a)
presents=$(grep -c '^mapped' "$UMBRIEL_RUNTIME_DIR/float-a.log" || true)
"$UMBRIEL" msg window-modify-width-right:0.3 > /dev/null
sleep 0.5
if [[ $(box float-a) != "$before" ]]; then
  echo "a fullscreen float was resized: $before -> $(box float-a)"
  exit 1
fi
if (( $(grep -c '^mapped' "$UMBRIEL_RUNTIME_DIR/float-a.log" || true) != presents )); then
  echo "a fullscreen float was sent a configure: $(tail -n 2 "$UMBRIEL_RUNTIME_DIR/float-a.log" | tr '\n' ' ')"
  exit 1
fi
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
sleep 0.4

# The 0.1 fraction floor, not the one-pixel minimum clampXdgWidth alone leaves a
# hint-less client with: the delta lands on the fraction, so from the 700px box
# this is 0.547 - 0.9 -> 0.1, where pixel arithmetic would land on 1px.
floor=$(box float-a)
read -r floor_x _ floor_w _ <<< "$floor"
"$UMBRIEL" msg window-modify-width-left:-0.9 > /dev/null
sleep 0.5
shrunk=$(box float-a)
read -r shrunk_x _ shrunk_w _ <<< "$shrunk"
if (( shrunk_w != OUTPUT_W / 10 )); then
  echo "width-left:-0.9 shrank the float to $shrunk_w, expected the 0.1 floor of $((OUTPUT_W / 10))"
  exit 1
fi
if (( shrunk_x + shrunk_w != floor_x + floor_w )); then
  echo "the shrink moved the pinned right edge: $((floor_x + floor_w)) -> $((shrunk_x + shrunk_w))"
  exit 1
fi

# The ceiling end of the same clamp, from the floor state: one more 0.9 lands on
# the whole usable extent (the pixel cap in floatingFractionSize would hide a
# missing 1.0 clamp here, so this asserts the outcome and the opposite edge, not
# the arithmetic behind it).
"$UMBRIEL" msg window-modify-width-left:0.9 > /dev/null
sleep 0.5
grown=$(box float-a)
read -r grown_x _ grown_w _ <<< "$grown"
if (( grown_w != OUTPUT_W )); then
  echo "width-left:0.9 grew the float to $grown_w, expected the clamped $OUTPUT_W"
  exit 1
fi
if (( grown_x + grown_w != shrunk_x + shrunk_w )); then
  echo "width-left:0.9 moved the pinned right edge: $((shrunk_x + shrunk_w)) -> $((grown_x + grown_w))"
  exit 1
fi

# The fraction is saturated now, so a second identical press has nothing left to
# do: it must not walk the float any further off the output.
"$UMBRIEL" msg window-modify-width-left:0.9 > /dev/null
sleep 0.5
if [[ $(box float-a) != "$grown" ]]; then
  echo "a second width-left:0.9 moved a saturated float: $grown -> $(box float-a)"
  exit 1
fi

# A maximize is dropped before the resize, and for maximize-to-edges that also
# brings the decorations back. The proof is the next toggle: if the resize had
# left the flag set, toggling would restore the pre-maximize box instead of
# maximizing again.
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
sleep 0.5
maximized=$(box float-a)
"$UMBRIEL" msg window-modify-width-left:-0.2 > /dev/null
sleep 0.5
if [[ $(box float-a) == "$maximized" ]]; then
  echo "width-left:-0.2 left a maximized float at $maximized"
  exit 1
fi
"$UMBRIEL" msg window-toggle-maximize-to-edges > /dev/null
sleep 0.5
if [[ $(box float-a) != "$maximized" ]]; then
  echo "the resize kept the maximize state: toggling landed on $(box float-a), not $maximized"
  exit 1
fi

# --- floating: the height directions, and a client that refuses the size ------

# The height half of the same anchor arithmetic: the top edge travels and the
# bottom one holds, then the reverse. Resizing in both directions is deliberate,
# since a single press would leave the no-op that a size at the usable limit
# produces indistinguishable from a pinned edge.
close_all
reset_config <<'EOF'

[animation]
enabled = false
EOF

spawn_client vert-a 700 700
wait_count 1
"$UMBRIEL" msg window-toggle-floating > /dev/null
sleep 0.4
read -r _ v_y _ v_h < <(box vert-a)

"$UMBRIEL" msg window-modify-height-down:-0.2 > /dev/null
sleep 0.5
read -r _ d_y _ d_h < <(box vert-a)
if (( d_y != v_y || d_h >= v_h )); then
  echo "height-down:-0.2 did not hold the top edge: y=$v_y h=$v_h -> y=$d_y h=$d_h"
  exit 1
fi

"$UMBRIEL" msg window-modify-height-down:0.2 > /dev/null
sleep 0.5
read -r _ e_y _ e_h < <(box vert-a)
if (( e_y != d_y || e_h <= d_h )); then
  echo "height-down:0.2 did not grow downward from the held top edge: y=$d_y h=$d_h -> y=$e_y h=$e_h"
  exit 1
fi

"$UMBRIEL" msg window-modify-height-up:0.2 > /dev/null
sleep 0.5
read -r _ u_y _ u_h < <(box vert-a)
if (( u_y >= e_y || u_h <= e_h || u_y + u_h != e_y + e_h )); then
  echo "height-up:0.2 did not grow upward from the held bottom edge: y=$e_y h=$e_h -> y=$u_y h=$u_h"
  exit 1
fi

# A client that keeps its own size and ignores the configure is what a size-hinted
# or fixed-size window does when the request falls outside its hints. The pinned
# edge has to end up where the size that actually committed puts it, not where the
# requested size would have.
close_all
reset_config <<'EOF'

[animation]
enabled = false
EOF

HOLD_SIZE=1 "$CLIENT" hold-a 700 700 > "$UMBRIEL_RUNTIME_DIR/hold-a.log" 2>&1 &
pids+=("$!")
wait_count 1
"$UMBRIEL" msg window-toggle-floating > /dev/null
sleep 0.4
read -r h_x _ h_w _ < <(box hold-a)
"$UMBRIEL" msg window-modify-width-left:0.2 > /dev/null
sleep 0.6
read -r i_x _ i_w _ < <(box hold-a)
if (( i_w != h_w )); then
  echo "the holding client changed size after all: w=$h_w -> $i_w"
  exit 1
fi
if (( i_x + i_w != h_x + h_w )); then
  echo "a client that kept its own size moved the opposite edge: right $((h_x + h_w)) -> $((i_x + i_w))"
  exit 1
fi

# --- floating: the resize animates instead of snapping -----------------------

close_all
reset_config <<'EOF'

[animation]
duration_ms = 2000
curve = "linear"

[animation.windows_in]
enabled = false

[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0

[appearance.shadow]
enabled = false
EOF

# The presented box, so a snap is distinguishable from an animated resize.
capture_box() {
  local image="$UMBRIEL_RUNTIME_DIR/$1.png" width height x y
  grim "$image"
  read -r width height x y < <(
    magick "$image" -alpha off -colorspace gray -threshold 1% \
      -bordercolor black -border 1 -trim -format '%w %h %X %Y\n' info:
  )
  printf '%d %d %d %d\n' "$((x - 1))" "$((y - 1))" "$width" "$height"
}

spawn_client float-anim 700 700
wait_count 1
"$UMBRIEL" msg window-toggle-floating > /dev/null
sleep 0.5

read -r base_x _ base_w _ < <(capture_box anim-base)
# 700 + 0.2 * 1280 = 956, still pinned on the right edge.
"$UMBRIEL" msg window-modify-width-left:0.2 > /dev/null
sleep 0.15
read -r mid_x _ mid_w _ < <(capture_box anim-mid)
sleep 1.5
read -r end_x _ end_w _ < <(box float-anim)
sleep 0.8
read -r settled_x _ settled_w _ < <(box float-anim)

# The opposite edge has to hold for the whole resize: a resize that places the
# origin for the size the client has already committed and only animates the size
# moves that edge while the client catches up. Trim boxes measure a pixel or two
# loose against the IPC box, so the edge is compared within a small tolerance.
if (( end_w != base_w + OUTPUT_W / 5 )); then
  echo "width-left:0.2 settled at $end_w, expected $((base_w + OUTPUT_W / 5))"
  exit 1
fi
drift=$((mid_x + mid_w - base_x - base_w))
if (( mid_w <= base_w || mid_w >= end_w || drift < -8 || drift > 8 )); then
  echo "the float snapped instead of animating: base=${base_w}+${base_x} mid=${mid_w}+${mid_x} end=${end_w}+${end_x}, opposite edge drifted ${drift}px"
  exit 1
fi
if (( settled_w != end_w || settled_x != end_x )); then
  echo "the resize did not settle: $end_w+$end_x -> $settled_w+$settled_x"
  exit 1
fi

echo "edge-anchored resize actions pin the opposite edge, stay inert on edges a layout cannot resize, and hold a float inside the fraction path's guards"
