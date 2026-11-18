# Shared scene and pixel assertions for preset and continuous overview zoom checks.
source "$UMBRIEL_HARNESS_LIB"

# Append each check's bindings after this scene configuration, then call zoom_start.
zoom_config() {
  cat >> "$UMBRIEL_CONFIG" <<'TOML'

[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = false
[overview]
zoom = 0.4
zoom_presets = [0.25, 0.5, 0.1]
shortcuts = false
background_blur = false
[colors.overview]
background_tint = "#000000FF"
workspace_background = "#000000FF"
[[window_rule]]
match.title = "^overview-zoom$"
default_floating = true
default_position = { x = 440, y = 210, anchor = "top_left" }
TOML
}

# Keep each check's blue threshold: smooth zoom includes partially covered edge pixels.
zoom_start() {
  readonly ZOOM_IMAGE="$UMBRIEL_RUNTIME_DIR/zoom.png"
  readonly ZOOM_PIXEL_TEST="b > $1 && r < 0.1 && g < 0.1"
  "$UMBRIEL" msg config-reload > /dev/null
  FILL_COLOR=0xFF0000FF "$UMBRIEL_UNMAP_CLIENT" overview-zoom 400 300 > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
  for _ in $(seq 100); do
    [[ $("$UMBRIEL" windows --json | jq length) == 1 ]] && break
    sleep 0.05
  done
  [[ $("$UMBRIEL" windows --json | jq length) == 1 ]]
  "$UMBRIEL" settle
  ZOOM_SELECTION=$("$UMBRIEL" windows --json | jq -r '.[] | select(.focused) | .id')
}

zoom_measure() {
  local label=$1 expected=$2 x y w h
  grim "$ZOOM_IMAGE"
  read -r x y w h < <("$UMBRIEL_PIXEL_PROBE" "$ZOOM_IMAGE" bbox "$ZOOM_PIXEL_TEST")
  if (( w < expected - 2 || w > expected + 2 )); then
    echo "$label: card width $w, expected $expected"
    exit 1
  fi
  [[ $("$UMBRIEL" windows --json | jq -r '.[] | select(.focused) | .id') == "$ZOOM_SELECTION" ]]
  echo "$label: width=$w"
}
zoom_action() {
  "$UMBRIEL" msg "$1" > /dev/null
  "$UMBRIEL" settle
  zoom_measure "$1" "$2"
}
