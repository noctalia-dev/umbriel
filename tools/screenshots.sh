#!/usr/bin/env bash
# Regenerate the user-guide screenshots in docs/user/assets/.
#
# Usage: tools/screenshots.sh [umbriel-binary]
#
# Runs the binary as a nested session with a throwaway home, config and D-Bus
# session, so nothing of the user's desktop or settings is touched. Shots use
# dark mode: a dim wallpaper made from one of the fixture paintings, and the
# Ayu dark palette (tools/screenshots/ayu-dark.json, from Noctalia's built-in
# palettes) for Umbriel, foot and luciole. It differs from the Noctalia palette
# of the docs site so the shots stand out from the page. Scenes are
# driven through `umbriel msg`. Needs: noctalia, mpvpaper, grim, foot, magick,
# cwebp, and a running Wayland session to nest in; luciole adds file manager
# windows when installed. ICON_THEME overrides luciole's Rose-Pine-Plus-Dark.
# Timing based: raise PAUSE if a shot comes out half drawn.
set -euo pipefail

bin=${1:-build-debug/umbriel}
PAUSE=${PAUSE:-2}

if [[ ${SHOTS_INNER:-} ]]; then
  # Inside the nested compositor: WAYLAND_DISPLAY and UMBRIEL_SOCKET point at it.
  u() { "$SHOTS_BIN" "$@"; }
  count() { u windows --json | jq length; }
  clear_windows() {
    pkill -x foot 2>/dev/null || true
    pkill -x luciole 2>/dev/null || true
    for _ in $(seq 50); do
      [[ $(count) -eq 0 ]] && break
      sleep 0.2
    done
  }
  term() { # title text: a terminal showing a fixture file
    u msg spawn "foot -T '$1' sh -c 'cat \"$HOME/$2\"; exec cat'" >/dev/null
  }
  files() {
    if command -v luciole >/dev/null; then
      u msg spawn "luciole '$HOME/Pictures'" >/dev/null
    else
      term Notes notes.txt
    fi
  }
  open_windows() { # n
    local want=$(($(count) + $1)) i
    for ((i = 0; i < $1; i++)); do
      case $((i % 3)) in
        0) files ;;
        1) term Shell session.txt ;;
        2) term Notes notes.txt ;;
      esac
      sleep 0.5
    done
    for _ in $(seq 50); do
      [[ $(count) -ge $want ]] && break
      sleep 0.2
    done
  }
  shot() { # name
    sleep "$PAUSE"
    grim -o shots "$SHOTS_TMP/$1.png"
    # Native 1280x800 and lossless: resampling and lossy encoding soften text and 1 px borders.
    cwebp -quiet -lossless -z 9 "$SHOTS_TMP/$1.png" -o "$SHOTS_OUT/$1.webp"
    echo "wrote $SHOTS_OUT/$1.webp"
  }
  scene() { # layout window-count
    clear_windows
    u msg workspace-set-layout "$1" >/dev/null
    open_windows "$2"
  }

  # A fixed-size virtual output makes every shot the same size; disabling the nested window's own output
  # leaves it the only place for windows, the cheatsheet, and the pointer.
  u output-create shots 1280x800@60
  u msg output-disable WL-1 >/dev/null
  trap 'clear_windows; pkill -x mpvpaper; kill "$PPID"' EXIT # always end the nested compositor, even on failure
  mpvpaper -o "no-audio image-display-duration=inf" shots "$SHOTS_TMP/wallpaper.png" >/dev/null 2>&1 &
  u config set hot_corners.top_left.enabled false >/dev/null # the resting pointer would open the overview

  u msg cheatsheet-open >/dev/null
  shot getting-started-cheatsheet
  u msg cheatsheet-close >/dev/null

  scene scrolling 3
  u msg column-focus-first >/dev/null
  u msg window-focus-right >/dev/null
  u msg column-center >/dev/null # neighbours cut off at both edges show the strip continues
  shot index-hero
  shot layout-scrolling

  scene dwindle 4
  shot layout-dwindle

  scene master 4
  shot layout-master

  u config set appearance.title_bar.mode '"forced"' >/dev/null
  scene stacking 3
  shot layout-stacking
  u config unset appearance.title_bar.mode >/dev/null

  scene scrolling 3
  u msg window-consume-from-left >/dev/null # stack the last window under the one before it
  u msg column-toggle-tabbed >/dev/null
  shot tab-groups

  u msg overview-open >/dev/null
  shot workspaces-overview
  u msg overview-close >/dev/null

  exit 0
fi

root=$(cd "$(dirname "$0")/.." && pwd)
out=$root/docs/user/assets
bin=$(realpath "$bin")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$out" "$tmp"/{home,config/luciole,cache,data,state,run}
chmod 700 "$tmp/run"
home=$tmp/home

# Fixture: public-domain paintings (see tools/screenshots/pictures/README.md) and two text files.
mkdir -p "$home/Pictures"
cp "$root"/tools/screenshots/pictures/*.jpg "$home/Pictures/"
cat >"$home/notes.txt" <<'EOF'
Umbriel

- four layouts
- workspaces per output
- tab groups
- scratchpads
- blur and shadows
EOF
# Layout-neutral commands, so the terminal reads right in every scene.
cat >"$home/session.txt" <<'EOF'
$ umbriel --version
umbriel 0.1.0
$ ls -1 ~/Pictures
landscape-at-sunset.jpg
rainbow-landscape.jpg
river-landscape.jpg
saint-ouen.jpg
EOF
# A soft, blurred take on one painting keeps the wallpaper quiet behind the windows.
magick "$root/tools/screenshots/pictures/ship-in-a-stormy-sea.jpg" -resize '1280x800^' -gravity center \
  -extent 1280x800 -blur 0x24 -modulate 45,40 "$tmp/wallpaper.png"

# The packaged starting configuration, so shots show what a new user sees, with the Noctalia palette included.
mkdir -p "$tmp/config/umbriel" "$tmp/config/foot"
perl -0pe 's/\[include\]\nfiles = \[\]/[include]\nfiles = ["noctalia.toml"]/' "$root/examples/config.toml" \
  >"$tmp/config/umbriel/config.toml"
# "auto" follows the generated colors.toml, which holds the dark palette.
printf '[browser]\nview_mode = "grid"\ncolor_scheme = "auto"\n' >"$tmp/config/luciole/config.toml"
printf 'include=%s/config/foot/themes/noctalia\nfont=monospace:size=11\npad=12x12\n' "$tmp" >"$tmp/config/foot/foot.ini"

# Render Noctalia's built-in templates (umbriel/noctalia.toml, foot/themes/noctalia, luciole/colors.toml and
# others) from the Ayu dark palette into the throwaway config. Hooks for absent programs warn harmlessly.
HOME=$home XDG_CONFIG_HOME="$tmp/config" XDG_DATA_HOME="$tmp/data" XDG_STATE_HOME="$tmp/state" \
  XDG_CACHE_HOME="$tmp/cache" noctalia theme --theme-json "$root/tools/screenshots/ayu-dark.json" \
  --builtin-config --default-mode dark >/dev/null 2>&1

# luciole's icon theme comes from the desktop settings (GSettings, here a plain key file).
real_data=${XDG_DATA_HOME:-$HOME/.local/share}
icons=${ICON_THEME:-Rose-Pine-Plus-Dark}
if [[ -d $real_data/icons/$icons || -d /usr/share/icons/$icons ]]; then
  mkdir -p "$tmp/config/glib-2.0/settings"
  printf "[org/gnome/desktop/interface]\nicon-theme='%s'\ncolor-scheme='prefer-dark'\n" "$icons" \
    >"$tmp/config/glib-2.0/settings/keyfile"
fi

parent=${WAYLAND_DISPLAY:?run inside a Wayland session}
[[ $parent == /* ]] || parent=${XDG_RUNTIME_DIR:?}/$parent

env -i PATH="$PATH" WAYLAND_DISPLAY="$parent" HOME="$home" LANG=C.UTF-8 \
  XDG_CONFIG_HOME="$tmp/config" XDG_CACHE_HOME="$tmp/cache" XDG_DATA_HOME="$tmp/data" \
  XDG_STATE_HOME="$tmp/state" XDG_RUNTIME_DIR="$tmp/run" \
  XDG_DATA_DIRS="$real_data:/usr/local/share:/usr/share" GSETTINGS_BACKEND=keyfile \
  SHOTS_INNER=1 SHOTS_TMP="$tmp" SHOTS_OUT="$out" SHOTS_BIN="$bin" \
  PAUSE="$PAUSE" \
  dbus-run-session -- "$bin" -c "$tmp/config/umbriel/config.toml" -s "$0"
