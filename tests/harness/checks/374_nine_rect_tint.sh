#!/usr/bin/env bash
# harness: outputs=1
# Texture albedo multiplies live theme colors, including alpha, focus and reloads.
set -euo pipefail
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
python3 - "$UMBRIEL_RUNTIME_DIR/frame.png" <<'PY'
import struct,zlib,sys
from pathlib import Path
def chunk(t,d):return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d))
colors=[(255,255,255,255),(128,128,128,255),(0,0,0,255),(255,255,255,128)]
def pixel(x,y):return bytes(colors[(x-2)%4])
data=b''.join(b'\0'+b''.join(pixel(x,y) for x in range(8)) for y in range(8))
Path(sys.argv[1]).write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',8,8,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(data))+chunk(b'IEND',b''))
PY
cat >> "$UMBRIEL_CONFIG" <<EOF_CONFIG
[animation]
enabled = false
[appearance]
use_nine_rect = true
[appearance.nine_rect]
texture = "$UMBRIEL_RUNTIME_DIR/frame.png"
slice_px = {top=2,bottom=2,left=2,right=2}
horizontal_stretch_mode = "tile"
tint_with_border_color = true
[appearance.shadow]
enabled = false
[colors.border]
focused = "#00FF00FF"
unfocused = "#FF000080"
[[window_rule]]
match.title = "^tint-first$"
default_floating = true
default_floating_size_px = {width=100,height=100}
default_position = {x=100,y=100,anchor="top_left"}
[[window_rule]]
match.title = "^tint-second$"
default_floating = true
default_floating_size_px = {width=100,height=100}
default_position = {x=400,y=100,anchor="top_left"}
EOF_CONFIG
"$UMBRIEL" msg config-reload >/dev/null
"$CLIENT" tint-first 100 100 > "$UMBRIEL_RUNTIME_DIR/first.log" 2>&1 &
first_pid=$!
for _ in $(seq 100); do
  [[ $("$UMBRIEL" windows --json | jq 'length') == 1 ]] && break
  sleep 0.025
done
"$CLIENT" tint-second 100 100 > "$UMBRIEL_RUNTIME_DIR/second.log" 2>&1 &
for _ in $(seq 100); do
  [[ $("$UMBRIEL" windows --json | jq 'length') == 2 ]] && break
  sleep 0.025
done
"$UMBRIEL" settle
box() { "$UMBRIEL" windows --json | jq -r --arg title "$1" '.[] | select(.title==$title) | "\(.x) \(.y)"'; }
read -r x1 y1 < <(box tint-first)
read -r x2 y2 < <(box tint-second)
shot() { grim "$UMBRIEL_RUNTIME_DIR/shot.png"; }
check() {
  local x=$1 y=$2 expected=$3 actual
  actual=$(magick "$UMBRIEL_RUNTIME_DIR/shot.png" -alpha off -format "%[pixel:p{$x,$y}]" info:)
  [[ $actual == "$expected" ]] || { echo "pixel $x,$y expected $expected got $actual"; exit 1; }
}
shot
check "$x1" "$((y1-1))" 'srgb(128,0,0)'
check "$x2" "$((y2-1))" 'srgb(0,255,0)'
check "$((x2+1))" "$((y2-1))" 'srgb(0,128,0)'
check "$((x2+2))" "$((y2-1))" 'srgb(0,0,0)'
check "$((x2+3))" "$((y2-1))" 'srgb(0,128,0)'
"$UMBRIEL_POINTER_CLIENT" 1280 720 move "$((x1+50))" "$((y1+50))" press 272 release 272
"$UMBRIEL" settle
shot
check "$x1" "$((y1-1))" 'srgb(0,255,0)'
check "$x2" "$((y2-1))" 'srgb(128,0,0)'
sed -i 's/tint_with_border_color = true/tint_with_border_color = false/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL" settle
shot
check "$x1" "$((y1-1))" 'srgb(255,255,255)'
sed -i 's/tint_with_border_color = false/tint_with_border_color = true/;s/#00FF00FF/#0000FFFF/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL" settle
shot
check "$x1" "$((y1-1))" 'srgb(0,0,255)'
# A closing copy retains its tinted source generation and fades it only once.
sed -i '/^\[animation\]$/,/^\[/s/enabled = false/enabled = true/' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'CLOSE'
[animation.windows_out]
enabled = true
duration_ms = 1000
curve = "linear"
style = "fade"
CLOSE
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL" settle
"$UMBRIEL" clock-freeze
kill "$first_pid"
for _ in $(seq 100); do
  [[ $("$UMBRIEL" windows --json | jq 'length') == 1 ]] && break
  sleep 0.025
done
"$UMBRIEL" clock-advance 100
shot
# Closing transfers focus first, so the captured frame is half-alpha red.
red=$(magick "$UMBRIEL_RUNTIME_DIR/shot.png" -alpha off -format "%[fx:round(255*p{$x1,$((y1-1))}.r)]" info:)
(( red >= 110 && red <= 120 )) || { echo "snapshot lost tint/opacity: red=$red"; exit 1; }
"$UMBRIEL" clock-advance 1000
"$UMBRIEL" clock-resume
echo "albedo tint preserves shading/alpha and follows focus, theme reloads, opt-out and closing snapshots"
