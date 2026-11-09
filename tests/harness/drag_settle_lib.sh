# Shared by the checks that pin what a released, still-deforming sheet does while
# its workspace moves. A released sheet rests under the shared drag tree, outside
# every workspace layer, until its spring settles; the drag that carries a window
# also carries its transient children. Each check adds one scenario to this desk.
source "$UMBRIEL_HARNESS_LIB"

readonly SETTLE_BTN_LEFT=272
readonly SETTLE_OUTPUT_W=1280
readonly SETTLE_OUTPUT_H=720
readonly SETTLE_REGION="${SETTLE_OUTPUT_W}x${SETTLE_OUTPUT_H}+0+0"
readonly SETTLE_IMAGE="$UMBRIEL_RUNTIME_DIR/settle.png"
# A quarter of a 400x300 sheet: proof it is drawn, not a stray edge.
readonly SETTLE_FLOOR=20000
# A dialogs's 200x150 face is smaller than a full sheet.
readonly SETTLE_DIALOG_FLOOR=5000
# Every drag below travels this far: enough to engage the spring's displacement.
readonly SETTLE_TRAVEL=120

# Floating-only desktop, instant animations, a slow drag spring so the sheet stays
# displaced far past whatever the check orders next, and a linear 300 ms slide
# (the default workspace curve is a spring and ignores duration_ms).
settle_config() {
  cat >> "$UMBRIEL_CONFIG" <<'CONFIG'

[layout]
mode = "floating"

[input.focus]
follows_mouse = false

[colors]
backdrop = "#000000FF"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
drag_opacity = 1.0
[appearance.shadow]
enabled = false
[animation]
duration_ms = 1
[animation.windows_in]
enabled = false
[animation.windows_out]
enabled = false
[animation.windows_drag]
physics = true
stiffness = 1.0
damping = 1.0
max_displacement_px = 200
[animation.workspaces]
enabled = true
duration_ms = 300
curve = "linear"
CONFIG
}

# A sheet the checks drag: floating, a fixed size, sized for the pixel floors.
settle_sheet_rule() { # $1 title
  cat >> "$UMBRIEL_CONFIG" <<CONFIG

[[window_rule]]
match.title = "^$1\$"
default_floating = true
default_floating_size_px = { width = 400, height = 300 }
CONFIG
}

# Maps `title` filled with `color` and waits until the compositor lists it.
settle_map() { # $1 title, $2 width, $3 height, $4 fill colour
  FILL_COLOR=$4 "$UMBRIEL_UNMAP_CLIENT" "$1" "$2" "$3" > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  await_window "$1"
  "$UMBRIEL" settle > /dev/null
}

# A parent toplevel and a dialog parented to it through a foreign handle, left
# unattached so the dialog keeps its own scene node: a workspace that hides it is
# then visible on screen instead of riding the parent's node.
settle_map_dialog() { # $1 parent title, $2 dialog title
  EXPORT_TOPLEVEL=1 "$UMBRIEL_SEAT_LOG_CLIENT" "$1" > "$UMBRIEL_RUNTIME_DIR/$1.log" 2>&1 &
  await_window "$1"
  await_lines "$UMBRIEL_RUNTIME_DIR/$1.log" "exported handle=" 1
  local handle
  handle=$(sed -n 's/^exported handle=//p' "$UMBRIEL_RUNTIME_DIR/$1.log")
  TRANSIENT_FOREIGN_HANDLE=$handle FILL_COLOR=0xFFFF0000 "$UMBRIEL_UNMAP_CLIENT" "$2" 200 150 \
    > "$UMBRIEL_RUNTIME_DIR/$2.log" 2>&1 &
  await_window "$2"
  "$UMBRIEL" settle > /dev/null
}

settle_box() { window_by_title "$1" | jq -r '"\(.x) \(.y) \(.w) \(.h)"'; }
settle_id() { window_by_title "$1" | jq -r '.id'; }
settle_workspace() { window_by_title "$1" | jq -r '.workspace'; }
settle_workspace_name() { # $1 workspace id, as `windows` reports it
  "$UMBRIEL" workspaces --json | jq -r --arg id "$1" '.[] | select(.id == $id) | .name'
}
settle_active_workspace() { "$UMBRIEL" workspaces --json | jq -r '.[] | select(.active) | .name'; }

# Presses super+left at (gx,gy) and travels SETTLE_TRAVEL in three steps. `hold`
# leaves the button down; `release` queues the release, which runs when
# pointer_release kicks the helper.
settle_press() { # $1 pointer span "WxH", $2 gx, $3 gy, $4 hold|release
  local -a commands=(
    move "$2" "$3" mod super press "$SETTLE_BTN_LEFT"
    move $(($2 + 40)) "$3" move $(($2 + 80)) "$3" move $(($2 + SETTLE_TRAVEL)) "$3"
  )
  [[ $4 == release ]] && commands+=(-- release "$SETTLE_BTN_LEFT" mod none)
  pointer_hold "${1%x*}" "${1#*x}" "${commands[@]}"
}

# Grabs `title`'s centre: the sheet follows the cursor from where it rests.
settle_drag() { # $1 title, $2 span "WxH", $3 hold|release
  local x y w h
  read -r x y w h <<< "$(settle_box "$1")"
  settle_press "$2" $((x + w / 2)) $((y + h / 2)) "$3"
}

# Grabs `title` a quarter in from a corner, clear of the dialog the compositor
# centres over a parent.
settle_drag_corner() { # $1 title, $2 span "WxH", $3 hold|release
  local x y w h
  read -r x y w h <<< "$(settle_box "$1")"
  settle_press "$2" $((x + w / 4)) $((y + h * 3 / 4)) "$3"
}

settle_drag_hold() { settle_drag "$1" "$2" hold; }
settle_drag_queue() { settle_drag "$1" "$2" release; }

settle_shot() { grim -o "${1:-HEADLESS-1}" "$SETTLE_IMAGE"; }
settle_green() { settle_shot "${1:-}"; "$UMBRIEL_PIXEL_PROBE" "$SETTLE_IMAGE" count 'g > 0.8 && r < 0.2 && b < 0.2'; }
settle_red() { settle_shot "${1:-}"; "$UMBRIEL_PIXEL_PROBE" "$SETTLE_IMAGE" count 'r > 0.8 && g < 0.2 && b < 0.2'; }
settle_bbox() { settle_shot "${1:-}"; "$UMBRIEL_PIXEL_PROBE" "$SETTLE_IMAGE" bbox 'g > 0.8 && r < 0.2 && b < 0.2'; }

# Every other animation in this config is 1 ms, so a sheet that does not change
# across an ordered tick means the deform never engaged and the deferral under
# test was never taken.
settle_require_deforming() { # $1 label, [$2 output]
  local before="$UMBRIEL_RUNTIME_DIR/deform-before.png" _ changed
  settle_shot "${2:-HEADLESS-1}"
  cp "$SETTLE_IMAGE" "$before"
  "$UMBRIEL" clock-advance 200 > /dev/null
  settle_shot "${2:-HEADLESS-1}"
  read -r _ changed < <("$UMBRIEL_PIXEL_PROBE" "$before" diff "$SETTLE_IMAGE" "$SETTLE_REGION")
  if ((changed == 0)); then
    echo "$1: the released sheet is not deforming (no pixel moved in 200 ms), the wait was never taken"
    exit 1
  fi
}

# The drawn sheet must have travelled with its workspace: still drawn, and moved
# along whichever axis this output arranges workspaces on by a large share of the
# slide, or clipped at the edge it left through. A view parked under the shared
# drag tree keeps drawing at its own coordinates.
settle_assert_travelled() { # $1 label, $2 box "x y w h", $3 drawn bbox, $4 minimum shift
  local label=$1 bxx bxy bxw bxh drx dry drw drh
  read -r bxx bxy bxw bxh <<< "$2"
  read -r drx dry drw drh <<< "$3"
  local dx=$((drx - bxx)) dy=$((dry - bxy))
  ((dx < 0)) && dx=$((-dx))
  ((dy < 0)) && dy=$((-dy))
  local moved=$((dx > dy ? dx : dy))
  ((moved >= $4)) && return 0
  # Clipped against the edge it travelled out through, and drawn smaller than its
  # own box: the sheet really moved out, it did not just change shape. Requiring
  # the box to have started at least the minimum away rejects a window that
  # already sat on the edge.
  if ((((drx <= 1 && bxx >= $4) || (dry <= 1 && bxy >= $4)) && drw * drh < bxw * bxh)); then
    return 0
  fi
  echo "$label: the deforming window did not travel with its workspace (drawn at ${drx},${dry}, box ${bxx},${bxy})"
  exit 1
}

# The drawn extent, after checking the sheet is drawn at all: a deform assertion
# against a window that never mapped would pass vacuously.
settle_drawn_extent() { # $1 sheet title
  local w h dw dh
  read -r _ _ w h <<< "$(settle_box "$1")"
  read -r _ _ dw dh < <(settle_bbox)
  if ((dw < w / 2 || dh < h / 2)); then
    echo "scenario never engaged: the window draws ${dw}x${dh} in a ${w}x${h} box"
    exit 1
  fi
  echo "$dw $dh"
}
