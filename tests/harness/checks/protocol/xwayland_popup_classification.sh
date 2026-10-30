#!/usr/bin/env bash
# harness: xwayland=true
# Popup metadata can arrive after association and while the Wayland surface
# stays mapped. Classification must change without a client hide/show cycle.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="$UMBRIEL_XWAYLAND_POPUP_CLIENT"
readonly APP_ID=UmbrielXwaylandPopup
readonly NATIVE_CLIENT="$UMBRIEL_SEAT_LOG_CLIENT"
readonly NATIVE_TITLE=xwayland-popup-native
readonly NATIVE_LOG="$UMBRIEL_RUNTIME_DIR/popup-native.log"
client_pid=
native_pid=
sequence=0

cleanup() {
  if [[ -n $client_pid ]]; then
    printf 'quit\n' >&"$control_fd" || true
    wait "$client_pid" || true
  fi
  if [[ -n $native_pid ]]; then
    kill "$native_pid" 2>/dev/null || true
    wait "$native_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[animation.dim_unfocused]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[colors]
backdrop = "#00FF00FF"
[colors.overview]
background_tint = "#00000000"
workspace_background = "#00FF00FF"
[overview]
zoom = 0.5
background_blur = false
workspace_wallpaper = false
shortcuts = false

[[window_rule]]
match.app_id = "^UmbrielXwaylandPopup$"
default_floating = false
default_maximize = true
opacity = 1.0
blur = false
CONFIG
"$UMBRIEL" msg config-reload > /dev/null

views() { "$UMBRIEL" windows --json | jq -c --arg app "$APP_ID" '[.[] | select(.app_id == $app)]'; }
send() { printf '%s\n' "$@" >&"$control_fd"; }

start_client() {
  local name=$1 fifo="$UMBRIEL_RUNTIME_DIR/popup-$1.control"
  client_log="$UMBRIEL_RUNTIME_DIR/popup-$name.log"
  mkfifo "$fifo"
  exec {control_fd}<> "$fifo"
  "$CLIENT" "xwayland-popup-$name" <&"$control_fd" > "$client_log" 2>&1 &
  client_pid=$!
  await_lines "$client_log" ready 1 0.025
}

stop_client() {
  send quit
  wait "$client_pid"
  client_pid=
  exec {control_fd}>&-
  expect_view_count 0
}

expect_view_count() {
  for _ in $(seq 100); do
    [[ $(views | jq length) == "$1" ]] && return 0
    sleep 0.025
  done
  echo "expected $1 managed fixture views: $(views)"
  cat "$client_log"
  return 1
}

expect_view_title() {
  for _ in $(seq 100); do
    [[ $(views | jq -r '.[0].title') == "$1" ]] && return 0
    sleep 0.025
  done
  echo "expected managed fixture title $1: $(views)"
  return 1
}

expect_managed() {
  expect_view_count 1
  for _ in $(seq 100); do
    if views | jq -e '.[0] | .visible and .active and .maximized and (.floating == false)' > /dev/null; then
      "$UMBRIEL" settle > /dev/null
      return 0
    fi
    sleep 0.025
  done
  echo "fixture did not become a focused maximized tile: $(views)"
  return 1
}

expect_state() {
  local label=$1 pattern=$2 token
  for _ in $(seq 100); do
    sequence=$((sequence + 1))
    token="${label}_$sequence"
    send "state $token"
    await_lines "$client_log" "state label=$token " 1 0.01
    if grep -qE "^state label=$token $pattern$" "$client_log"; then return 0; fi
    sleep 0.025
  done
  echo "$label did not reach X11 state: $pattern"
  cat "$client_log"
  return 1
}

expect_raw_focus_clear() {
  local label=$1 token
  for _ in $(seq 100); do
    sequence=$((sequence + 1))
    token="${label}_$sequence"
    send "state $token"
    await_lines "$client_log" "focus label=$token " 1 0.01
    if grep -q "^focus label=$token input=0 active=0$" "$client_log"; then return 0; fi
    sleep 0.025
  done
  echo "$label did not clear both raw X focus channels"
  cat "$client_log"
  return 1
}

expect_native_active() {
  for _ in $(seq 100); do
    if "$UMBRIEL" windows --json | jq -e --arg title "$NATIVE_TITLE" \
      '.[] | select(.title == $title) | .active' > /dev/null; then return 0; fi
    sleep 0.025
  done
  echo "native window did not retain active focus: $("$UMBRIEL" windows --json)"
  cat "$NATIVE_LOG"
  return 1
}

expect_managed_state() {
  local label=$1 fullscreen=${2:-false} type=${3:-normal} x y width height
  read -r x y width height < <(views | jq -r '.[0] | "\(.x) \(.y) \(.w) \(.h)"')
  if [[ $fullscreen == true ]]; then
    x=0 y=0 width=1280 height=720
  fi
  expect_state "$label" "map=2 override=false type=$type x=$x y=$y w=$width h=$height input=true active=true listed=true stacked=true max_horz=true max_vert=true fullscreen=$fullscreen"
}

expect_popup_state() {
  local label=$1 override=$2 type=${3:-popup} x=${4:-200} y=${5:-150} width=${6:-300} height=${7:-160}
  local active=true stacked=true
  if [[ $override == true ]]; then active='(true|false)'; stacked=false; fi
  expect_state "$label" "map=2 override=$override type=$type x=$x y=$y w=$width h=$height input=true active=$active listed=true stacked=$stacked max_horz=false max_vert=false fullscreen=false"
}

expect_output_pixel() {
  local label=$1 x=$2 y=$3 expected=$4 image="$UMBRIEL_RUNTIME_DIR/popup-$1.png" pixel
  "$UMBRIEL" settle > /dev/null
  grim -o HEADLESS-1 "$image"
  pixel=$("$UMBRIEL_PIXEL_PROBE" "$image" pixel "$x" "$y")
  [[ $pixel == "$expected" ]] || { echo "$label: pixel at $x,$y was $pixel, expected $expected"; return 1; }
}

expect_pixels() {
  local label=$1 x=${2:-200} y=${3:-150} width=${4:-300} height=${5:-160}
  local image="$UMBRIEL_RUNTIME_DIR/popup-$label.png" pixel
  "$UMBRIEL" settle > /dev/null
  grim -o HEADLESS-1 "$image"
  pixel=$("$UMBRIEL_PIXEL_PROBE" "$image" pixel "$((x + width / 2))" "$((y + height / 2))")
  [[ $pixel == '48 96 144' ]] || { echo "$label: popup content pixel was $pixel"; return 1; }
  for point in "$((x - 5)) $((y + 20))" "$((x + width + 5)) $((y + 20))"; do
    read -r px py <<< "$point"
    pixel=$("$UMBRIEL_PIXEL_PROBE" "$image" pixel "$px" "$py")
    [[ $pixel == '0 255 0' ]] || { echo "$label: outside pixel at $px,$py was $pixel"; return 1; }
  done
}

# The passive popup draws over its maximized parent instead of the backdrop.
expect_passive_pixels() {
  local label=$1 image="$UMBRIEL_RUNTIME_DIR/popup-$1.png" pixel
  "$UMBRIEL" settle > /dev/null
  grim -o HEADLESS-1 "$image"
  pixel=$("$UMBRIEL_PIXEL_PROBE" "$image" pixel 350 230)
  [[ $pixel == '48 96 144' ]] || { echo "$label: popup content pixel was $pixel"; return 1; }
  pixel=$("$UMBRIEL_PIXEL_PROBE" "$image" pixel 510 230)
  [[ $pixel == '192 128 64' ]] || { echo "$label: parent pixel was $pixel"; return 1; }
}

type_key() {
  local key=$1 before
  before=$(events "$client_log" "key code=$((key + 8)) state=pressed")
  "$UMBRIEL_POINTER_CLIENT" 1280 720 tap "$key" > /dev/null
  await_lines "$client_log" "key code=$((key + 8)) state=pressed" "$((before + 1))" 0.025
}

type_key_to() {
  local key=$1 window=$2 before
  before=$(events "$client_log" "key code=$((key + 8)) state=pressed window=$window")
  "$UMBRIEL_POINTER_CLIENT" 1280 720 tap "$key" > /dev/null
  await_lines "$client_log" "key code=$((key + 8)) state=pressed window=$window" "$((before + 1))" 0.025
}

# The same metadata before association is the classification control. Broad
# application tiling/maximize rules must never own a client-placed menu.
start_client before
send 'type popup' map
expect_popup_state before false
expect_view_count 0
expect_pixels before
type_key 30
stop_client

# EWMH MENU denotes a persistent torn-off menu. It stays managed unless OR
# says otherwise, including when its type arrives before its first map.
start_client managed-menu
send 'type menu' map
expect_managed
expect_managed_state menu-before false menu
menu_id=$(views | jq -r '.[0].id')
type_key 40

# A title event acknowledges each preceding type update on the XWM stream.
# NORMAL and MENU share a wrapper, so changing between them keeps its ID.
send 'type normal' 'title xwayland-menu-normal'
expect_view_title xwayland-menu-normal
expect_managed_state menu-normal
send 'type menu' 'title xwayland-menu-restored'
expect_view_title xwayland-menu-restored
expect_managed_state menu-same-wrapper false menu
if [[ $(views | jq -r '.[0].id') != "$menu_id" ]]; then
  echo 'a normal-to-menu metadata update replaced the managed View'
  exit 1
fi

# POPUP_MENU is client-placed. Returning to MENU must map a new managed View
# from the existing surface without needing an unmap, map or redraw command.
send 'type popup'
expect_view_count 0
expect_popup_state menu-to-popup false
expect_pixels menu-to-popup
send 'type menu'
expect_managed
expect_managed_state popup-to-menu false menu
type_key 41

# OR remains authoritative for a window typed MENU. Its saved client geometry
# returns while OR is set; clearing OR reinstates ordinary managed rules.
send 'override true'
expect_view_count 0
send focus
expect_popup_state menu-override true menu
expect_pixels menu-override
# wlroots does not request seat keyboard focus for an OR MENU window. Its X
# self-focus is observable above; managed input resumes after OR is cleared.
send 'override false' 'geometry 240 190 320 180'
expect_managed
expect_managed_state menu-override-restored false menu
type_key 43
stop_client

start_client late-type
send map
expect_managed
expect_managed_state normal
original_id=$(views | jq -r '.[0].id')

# A following title event acknowledges the preceding type event on XWM's
# ordered event stream. A same-classification property update keeps its View.
send 'type normal' 'title xwayland-popup-same-class'
expect_view_title xwayland-popup-same-class
if [[ $(views | jq -r '.[0].id') != "$original_id" ]]; then
  echo 'a normal-to-normal metadata update replaced the managed View'
  exit 1
fi

# Seed both maximize atoms and fullscreen so popup conversion must discard
# compositor-owned window state rather than merely keep absent flags absent.
"$UMBRIEL" msg window-toggle-fullscreen > /dev/null
for _ in $(seq 100); do
  [[ $(views | jq -r '.[0].fullscreen') == true ]] && break
  sleep 0.025
done
if [[ $(views | jq -r '.[0].fullscreen') != true ]]; then
  echo "fixture did not enter fullscreen before popup conversion: $(views)"
  exit 1
fi
"$UMBRIEL" settle > /dev/null
expect_managed_state fullscreen-seeded true

# Publish only the type, with no configure or redraw command to rescue it.
# The original client rectangle must replace the maximized tile immediately.
send 'type popup'
expect_view_count 0
expect_popup_state late-type false
expect_pixels late-type
type_key 31

# Other popup types keep client placement and input. Future geometry requests
# are still honored after classification, independently of rectangle recovery.
send 'type dropdown' 'geometry 240 190 320 180'
expect_popup_state dropdown false dropdown 240 190 320 180
expect_view_count 0
expect_pixels dropdown 240 190 320 180
type_key 32
send 'type normal'
expect_managed
expect_managed_state restored-normal
type_key 33

# Role changes while overview cards exist must drop the old View immediately.
# At zoom 0.5 the maximized card covers this sample; the actual 320x180 popup
# at 240,190 cannot reach it. No pointer hit-testing selects a card here.
"$UMBRIEL" msg overview-open > /dev/null
expect_output_pixel overview-managed 800 450 '48 96 144'
send 'type popup'
expect_view_count 0
expect_state overview-popup 'map=2 override=false type=popup x=240 y=190 w=320 h=180 input=(true|false) active=(true|false) listed=true stacked=true max_horz=false max_vert=false fullscreen=false'
expect_output_pixel overview-popup 800 450 '0 255 0'
send 'type normal'
expect_view_count 1
expect_output_pixel overview-restored 800 450 '48 96 144'
"$UMBRIEL" msg overview-close > /dev/null
expect_managed
expect_managed_state overview-closed
type_key 37

# Present a real frame before withdrawal, then remap the same X window. This
# synchronizes lifecycle coverage with rendering rather than an arbitrary wait.
image="$UMBRIEL_RUNTIME_DIR/popup-managed-before-remap.png"
grim -o HEADLESS-1 "$image"
send unmap
expect_state withdrawn 'map=0 override=false type=normal .* listed=false stacked=false max_horz=false max_vert=false fullscreen=false'
expect_view_count 0
send map
expect_managed
expect_managed_state remapped-normal
type_key 34
stop_client

start_client late-override
send map
expect_managed
expect_managed_state before-override

# A stack-only change produces real ConfigureNotify after the OR attribute
# changes. It cannot replace the saved 300x160 client rectangle with a new one.
send 'override true'
expect_view_count 0
send focus
expect_popup_state override true normal
expect_pixels override
type_key 35

# Turning OR off redirects requests again. A genuine geometry request makes
# the server report its new flag; the new managed View reapplies normal rules.
send 'override false' 'geometry 240 190 320 180'
expect_managed
expect_managed_state restored-override
type_key 36
stop_client

# A client may move and resize directly as it changes OR. These new axes
# replace its saved opening rectangle; restoring the old rectangle is wrong.
start_client direct-override
send map
expect_managed
expect_managed_state before-direct-override
send override-direct
expect_view_count 0
send focus
expect_popup_state direct-override true normal 240 190 320 180
expect_pixels direct-override 240 190 320 180
type_key 38
stop_client

# Retiring an XWM focus record after the seat moved to a native client must
# preserve the native handoff. PointerRoot is a different X input focus value
# from None, so observing only whether this popup owns focus would miss it.
start_client native-handoff
send map
expect_managed
send 'type popup'
expect_view_count 0
expect_popup_state before-native false
"$NATIVE_CLIENT" "$NATIVE_TITLE" > "$NATIVE_LOG" 2>&1 &
native_pid=$!
expect_native_active
await_lines "$NATIVE_LOG" keyboard-enter 1 0.025
expect_raw_focus_clear native-focused
native_leaves=$(events "$NATIVE_LOG" keyboard-leave)
x_keys_before=$(events "$client_log" 'key code=47 state=pressed')
send unmap
expect_state native-popup-withdrawn 'map=0 override=false type=popup .* input=false active=false listed=false stacked=false max_horz=false max_vert=false fullscreen=false'
expect_raw_focus_clear native-popup-withdrawn
expect_native_active
if [[ $(events "$NATIVE_LOG" keyboard-leave) != "$native_leaves" ]]; then
  echo "withdrawing the old popup removed native keyboard focus: $(cat "$NATIVE_LOG")"
  exit 1
fi
native_keys_before=$(events "$NATIVE_LOG" 'keyboard-key code=39 state=pressed')
"$UMBRIEL_POINTER_CLIENT" 1280 720 keyboard-only tap 39 > /dev/null
await_lines "$NATIVE_LOG" 'keyboard-key code=39 state=pressed' "$((native_keys_before + 1))" 0.025
if [[ $(events "$client_log" 'key code=47 state=pressed') != "$x_keys_before" ]]; then
  echo "native typing also reached the withdrawn X11 popup: $(cat "$client_log")"
  exit 1
fi
stop_client
kill "$native_pid"
wait "$native_pid" 2>/dev/null || true
native_pid=

# A utility window transient for another X11 window that refuses keyboard input is client-placed, like WeChat's search
# suggestions. It must not join the layout or take typing from the window it belongs to.
start_client passive
send parent-map
expect_managed
parent_window=$(grep -o 'parent=0x[0-9a-f]*' "$client_log" | head -1 | cut -d= -f2)
send 'transient' 'type utility' 'input false' 'take-focus' map
expect_state passive 'map=2 override=false type=utility x=200 y=150 w=300 h=160 input=false active=false listed=true stacked=true max_horz=false max_vert=false fullscreen=false'
expect_view_count 1
expect_passive_pixels passive
type_key_to 44 "$parent_window"

# Remapping the same X window re-reads its metadata before the wrapper exists.
send unmap
expect_state passive-withdrawn 'map=0 override=false type=utility .* listed=false stacked=false max_horz=false max_vert=false fullscreen=false'
send map
expect_state passive-remapped 'map=2 override=false type=utility x=200 y=150 w=300 h=160 input=false active=false listed=true stacked=true max_horz=false max_vert=false fullscreen=false'
expect_view_count 1
expect_passive_pixels passive-remapped
type_key_to 45 "$parent_window"
stop_client

echo 'mapped X11 windows reclassify by popup type and override-redirect state while preserving client geometry and input'
