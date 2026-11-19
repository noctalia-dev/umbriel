#!/usr/bin/env bash
# Clear-colored title bars must be redrawn above blur after overview and focus changes.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

settle_frame() {
  "$UMBRIEL" clock-advance 2000
  "$UMBRIEL" settle >/dev/null
}

cat >>"$UMBRIEL_CONFIG" <<'EOF'
[animation]
enabled = true
[appearance]
border_width = 2
corner_radius = 0
[appearance.title_bar]
mode = "forced"
[appearance.blur]
enabled = true
optimized = true
[colors]
backdrop = "#000000FF"
[colors.title_bar]
background = "#000000FF"
background_unfocused = "#FF00FF80"
[[window_rule]]
match.title = "^blur-refocus-"
blur = true
EOF
"$UMBRIEL" config validate -c "$UMBRIEL_CONFIG" >/dev/null
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL" clock-freeze
"$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 >"$UMBRIEL_RUNTIME_DIR/wallpaper.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/wallpaper.log" ready 1
for name in one two; do
  FOLLOW_CONFIGURES=1 "$UMBRIEL_UNMAP_CLIENT" "blur-refocus-$name" 480 300 >"$UMBRIEL_RUNTIME_DIR/$name.log" 2>&1 &
  await_lines "$UMBRIEL_RUNTIME_DIR/$name.log" mapped 1
done

expect_background() {
  local phase=$1 window x y color
  settle_frame
  window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.active)')
  [[ $(jq -r '.title_bar' <<<"$window") == true ]]
  x=$(jq -r '.x + 180' <<<"$window")
  y=$(jq -r '.y - 14' <<<"$window")
  grim -o HEADLESS-1 "$UMBRIEL_RUNTIME_DIR/title-bar.png"
  color=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/title-bar.png" pixel "$x" "$y")
  [[ $color == "0 0 0" ]] || {
    echo "$phase: focused title bar lost its opaque background: $color"
    exit 1
  }
}

ids=$("$UMBRIEL" windows --json | jq -r '.[].id')
for id in $ids; do
  "$UMBRIEL" msg window-focus "$id" >/dev/null
  expect_background "before overview"
  "$UMBRIEL" msg overview-open >/dev/null
  settle_frame
  "$UMBRIEL" msg overview-close >/dev/null
  expect_background "after overview"
done
for id in $ids; do
  "$UMBRIEL" msg window-focus "$id" >/dev/null
  expect_background "after IPC refocus"
done
echo "opaque title bars retain their background across overview and IPC refocus with blur"

# A light unfocused frame stays opaque where the title bar meets the border's inner arc.
sed -i 's/^corner_radius = 0$/corner_radius = 12/; s/#FF00FF80/#F0EFF7FF/; /^\[animation\]$/ { n; s/enabled = true/enabled = false/; }' "$UMBRIEL_CONFIG"
cat >>"$UMBRIEL_CONFIG" <<'EOF'
[colors.border]
unfocused = "#F0EFF7FF"
EOF
for scale in 1 1.25 2; do
  cat >>"$UMBRIEL_CONFIG" <<EOF
[output.HEADLESS-1]
scale = $scale
EOF
  "$UMBRIEL" msg config-reload >/dev/null
  for id in $ids; do
    "$UMBRIEL" msg window-focus "$id" >/dev/null
    settle_frame
    window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.active == false)')
    read -r x y width < <(jq -r '[.x, .y - 28, .w] | @tsv' <<<"$window")
    grim -o HEADLESS-1 "$UMBRIEL_RUNTIME_DIR/title-bar.png"
    for side in left right; do
      for point in '3 2' '2 3' '6 0' '0 6'; do
        read -r dx dy <<<"$point"
        [[ $side == right ]] && dx=$((width - 1 - dx))
        read -r px py < <(awk -v x="$((x + dx))" -v y="$((y + dy))" -v s="$scale" \
          'BEGIN { printf "%.0f %.0f\n", x*s, y*s }')
        read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/title-bar.png" pixel "$px" "$py")
        if ((red < 238 || green < 237 || blue < 245)); then
          echo "scale=$scale $side unfocused title bar leaked its backdrop at $px,$py: $red $green $blue"
          exit 1
        fi
      done
    done
  done
  sed -i '/^\[output.HEADLESS-1\]$/ { N; d; }' "$UMBRIEL_CONFIG"
done
echo "light unfocused title bars meet their borders without curved backdrop gaps"

# A transparent border exposes the title fill's inner arc, including its blur mask.
sed -i 's/^corner_radius = 12$/corner_radius = 15/; s/^border_width = 2$/border_width = 4/; s/^unfocused = .*/unfocused = "#00000000"/' "$UMBRIEL_CONFIG"
cat >>"$UMBRIEL_CONFIG" <<'EOF'
[animation.border]
duration_ms = 100
curve = "linear"
EOF
expect_rounded_corners() {
  local window=$1 x y width px py red green blue side dx
  read -r x y width < <(jq -r '[.x, .y - 28, .w] | @tsv' <<<"$window")
  grim -o HEADLESS-1 "$UMBRIEL_RUNTIME_DIR/title-bar.png"
  for side in left right; do
    dx=1
    [[ $side == right ]] && dx=$((width - 2))
    read -r px py < <(awk -v x="$((x + dx))" -v y="$((y + 1))" -v s="$scale" \
      'BEGIN { printf "%.0f %.0f\n", x*s, y*s }')
    read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/title-bar.png" pixel "$px" "$py")
    if ((red != 85 || green != 119 || blue != 170)); then
      echo "scale=$scale $side transparent-border corner covered the wallpaper at $px,$py: $red $green $blue"
      return 1
    fi
  done
}

for background in '#F0EFF7FF' '#FF00FF80'; do
  animated=false
  [[ $background == '#FF00FF80' ]] && animated=true
  sed -i "s/^background_unfocused = .*/background_unfocused = \"$background\"/; /^\[animation\]$/ { n; s/^enabled = .*/enabled = $animated/; }" "$UMBRIEL_CONFIG"
  for scale in 1 1.25 2 2.5; do
    cat >>"$UMBRIEL_CONFIG" <<EOF
[output.HEADLESS-1]
scale = $scale
EOF
    "$UMBRIEL" msg config-reload >/dev/null
    for id in $ids; do
      "$UMBRIEL" msg window-focus "$id" >/dev/null
      settle_frame
      window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.active == false)')
      expect_rounded_corners "$window"
    done
    sed -i '/^\[output.HEADLESS-1\]$/ { N; d; }' "$UMBRIEL_CONFIG"
  done
done

# The overlap follows resolved window colors rather than only the global border setting.
sed -i 's/^unfocused = "#00000000"$/unfocused = "#F0EFF7FF"/' "$UMBRIEL_CONFIG"
cat >>"$UMBRIEL_CONFIG" <<'EOF'
[[window_rule]]
match.title = "^blur-refocus-one$"
border_color_unfocused = "#00000000"
EOF
"$UMBRIEL" msg config-reload >/dev/null
id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "blur-refocus-two") | .id')
"$UMBRIEL" msg window-focus "$id" >/dev/null
settle_frame
window=$("$UMBRIEL" windows --json | jq -c '.[] | select(.title == "blur-refocus-one")')
scale=1
expect_rounded_corners "$window"

echo "transparent-border title bars retained rounded corners through focus, blur, scaling, and window overrides"
