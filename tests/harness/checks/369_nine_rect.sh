#!/usr/bin/env bash
# harness: outputs=1
# Pixel-exact slice repetition, center opt-in, live reload and frame resize input.
set -euo pipefail
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly SHOT="$UMBRIEL_RUNTIME_DIR/nine.png"
python3 - "$UMBRIEL_RUNTIME_DIR" <<'PY'
import struct, zlib, sys
from pathlib import Path
root = Path(sys.argv[1])
def png(name, channels, pixels):
    def chunk(tag, data): return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    data = b''.join(b'\0' + bytes(sum((pixels(x,y) for x in range(8)), ())) for y in range(8))
    (root / name).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB',8,8,8,6 if channels==4 else 2,0,0,0)) + chunk(b'IDAT',zlib.compress(data)) + chunk(b'IEND', b''))
colors = [(255,0,0,255),(0,255,0,255),(0,0,255,255),(255,255,0,255)]
def art(x,y):
    if 2 <= x < 6 and y < 2: return colors[x-2]
    if 2 <= x < 6 and 2 <= y < 6: return (0,255,255,255)
    return (255,0,255,255)
png('frame.png',4,art)
PY
cat >> "$UMBRIEL_CONFIG" <<EOF_CONFIG

[animation]
enabled = false
[appearance]
use_nine_rect = true
[appearance.nine_rect]
texture = "$UMBRIEL_RUNTIME_DIR/frame.png"
slice_px = { top = 2, bottom = 2, left = 2, right = 2 }
horizontal_stretch_mode = "tile"
vertical_stretch_mode = "stretch"
center_mode = "none"
[appearance.shadow]
enabled = false
[[window_rule]]
match.title = "^nine-rect$"
default_floating = true
default_floating_size_px = { width = 70, height = 50 }
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF_CONFIG
"$UMBRIEL" msg config-reload >/dev/null
RESIZE_FILL_COLOR=0xFF303030 "$CLIENT" nine-rect 70 50 > "$UMBRIEL_RUNTIME_DIR/nine-client.log" 2>&1 &
client_pid=$!
for _ in $(seq 100); do
  [[ $("$UMBRIEL" windows --json | jq 'length') == 1 ]] && break
  sleep 0.025
done
"$UMBRIEL" settle
box() { "$UMBRIEL" windows --json | jq -r '.[] | select(.title=="nine-rect") | "\(.x) \(.y) \(.w) \(.h)"'; }
read -r x y w h < <(box)
[[ $w == 70 && $h == 50 ]] || { echo "unexpected client box: $(box)"; exit 1; }
grim -o HEADLESS-1 "$SHOT"
# Use ImageMagick's raw RGB output to compare every top texel, including the cropped last tile.
magick "$SHOT" -alpha off -depth 8 "rgb:$UMBRIEL_RUNTIME_DIR/nine.rgb"
python3 - "$UMBRIEL_RUNTIME_DIR/nine.rgb" "$x" "$y" "$w" <<'PY'
from pathlib import Path
import sys
b=Path(sys.argv[1]).read_bytes(); x,y,w=map(int,sys.argv[2:]); stride=1280*3
colors=[bytes(c) for c in [(255,0,0),(0,255,0),(0,0,255),(255,255,0)]]
for i in range(w):
    p=(y-1)*stride+(x+i)*3
    assert b[p:p+3]==colors[i%4], (i,list(b[p:p+3]),list(colors[i%4]))
p=(y+10)*stride+(x+10)*3
assert b[p:p+3] != bytes((0,255,255)), 'center=none covered the client'
# The frame extends two pixels beyond the client.
p=(y-2)*stride+(x-2)*3
assert b[p:p+3]==bytes((255,0,255)), 'frame was clipped'
PY
sed -i 's/center_mode = "none"/center_mode = "overlay"/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL" settle
grim -o HEADLESS-1 "$SHOT"
color=$(magick "$SHOT" -alpha off -format "%[pixel:p{$((x+10)),$((y+10))}]" info:)
[[ $color == 'srgb(0,255,255)' ]] || { echo "center overlay: $color"; exit 1; }
# A bad reload must retain the currently visible theme.
sed -i 's/left = 2/left = 20/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload >/dev/null || true
"$UMBRIEL" settle
grim -o HEADLESS-1 "$SHOT"
color=$(magick "$SHOT" -alpha off -format "%[pixel:p{$((x+10)),$((y+10))}]" info:)
[[ $color == 'srgb(0,255,255)' ]] || { echo "failed reload replaced live theme: $color"; exit 1; }
# A left press in the decoration band starts compositor resize.
"$UMBRIEL_POINTER_CLIENT" 1280 720 move "$((x+w+1))" "$((y+h/2))" press 272 pause 100 move "$((x+w+21))" "$((y+h/2))" release 272
"$UMBRIEL" settle
read -r _ _ resized _ < <(box)
(( resized > w )) || { echo "frame did not resize: $(box)"; exit 1; }
# Re-enable a valid configuration before testing scale changes.
sed -i 's/left = 20/left = 2/' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'SCALE'

[output."HEADLESS-1"]
scale = 1.0 # nine-test-scale
SCALE
for scale in 1.25 2.0; do
  sed -i "s/^scale = .* # nine-test-scale/scale = $scale # nine-test-scale/" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload >/dev/null
  "$UMBRIEL" settle
  read -r x y w h < <(box)
  grim -o HEADLESS-1 "$SHOT"
  magick "$SHOT" -alpha off -depth 8 "rgb:$UMBRIEL_RUNTIME_DIR/nine.rgb"
  python3 - "$UMBRIEL_RUNTIME_DIR/nine.rgb" "$x" "$y" "$w" "$scale" <<'PY_SCALE'
from pathlib import Path
import sys, math
b=Path(sys.argv[1]).read_bytes(); x,y,w=map(int,sys.argv[2:5]); scale=float(sys.argv[5])
roundpx=lambda x:math.floor(x*scale+0.5)
left,right,top=roundpx(x),roundpx(x+w),roundpx(y)
colors=[bytes(c) for c in [(255,0,0),(0,255,0),(0,0,255),(255,255,0)]]
for i in range(right-left):
    p=((top-1)*1280+left+i)*3
    sample=math.floor((i+0.5)*w/(right-left))%4
    assert b[p:p+3]==colors[sample],(scale,i,list(b[p:p+3]),sample)
PY_SCALE
done
# Preserve the texture after source upload and after the live view is destroyed.
sed -i 's/^scale = .* # nine-test-scale/scale = 1.0 # nine-test-scale/' "$UMBRIEL_CONFIG"
sed -i '/^\[animation\]$/,/^\[/s/enabled = false/enabled = true/' "$UMBRIEL_CONFIG"
cat >> "$UMBRIEL_CONFIG" <<'CLOSE'

[animation.windows_out]
enabled = true
duration_ms = 1000
curve = "linear"
style = "popin"
scale = 0.8
CLOSE
"$UMBRIEL" msg config-reload >/dev/null
"$UMBRIEL" settle
read -r x y w h < <(box)
"$UMBRIEL" clock-freeze
kill "$client_pid"
for _ in $(seq 100); do
  [[ $("$UMBRIEL" windows --json | jq 'length') == 0 ]] && break
  sleep 0.025
done
"$UMBRIEL" clock-advance 100
grim -o HEADLESS-1 "$SHOT"
red=$(magick "$SHOT" -alpha off -format "%[fx:round(255*p{$((x-1)),$((y-1))}.r)]" info:)
(( red > 100 )) || { echo "close snapshot lost frame: red=$red"; exit 1; }
"$UMBRIEL" clock-advance 1100
"$UMBRIEL" clock-resume
echo "nine-rect tiles, frame, overlay, reload, resize, fractional scale and close snapshot verified"
