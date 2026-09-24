#!/usr/bin/env bash
# harness: outputs=1
# Content fills transparent inner padding while slices retain their source size.
set -euo pipefail
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
python3 - "$UMBRIEL_RUNTIME_DIR/frame.png" <<'PY'
import struct,zlib,sys
from pathlib import Path
def chunk(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d))
def pixel(x,y): return bytes((255,0,255,255) if x in (0,7) or y in (0,7) else (0,0,0,0))
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
content_inset_px = {top=1,bottom=1,left=1,right=1}
horizontal_stretch_mode = "tile"
vertical_stretch_mode = "tile"
[appearance.shadow]
enabled = false
[[window_rule]]
match.title = "^inset-preview$"
default_floating = true
default_floating_size_px = {width=70,height=50}
default_position = {x=100,y=100,anchor="top_left"}
EOF_CONFIG
"$UMBRIEL" msg config-reload >/dev/null
RESIZE_FILL_COLOR=0xFF5577AA "$CLIENT" inset-preview 70 50 > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
for _ in $(seq 100); do
  [[ $("$UMBRIEL" windows --json | jq 'length') == 1 ]] && break
  sleep 0.025
done
"$UMBRIEL" settle
read -r x y w h < <("$UMBRIEL" windows --json | jq -r '.[] | "\(.x) \(.y) \(.w) \(.h)"')
grim "$UMBRIEL_RUNTIME_DIR/frame-shot.png"
magick "$UMBRIEL_RUNTIME_DIR/frame-shot.png" -alpha off -depth 8 "rgb:$UMBRIEL_RUNTIME_DIR/frame.rgb"
python3 - "$UMBRIEL_RUNTIME_DIR/frame.rgb" "$x" "$y" "$w" "$h" <<'PY'
from pathlib import Path
import sys
b=Path(sys.argv[1]).read_bytes();x,y,w,h=map(int,sys.argv[2:])
def pixel(x,y):return b[(y*1280+x)*3:(y*1280+x)*3+3]
for i in range(w):
    assert pixel(x+i,y-1)==bytes((255,0,255)),('top border',i)
    assert pixel(x+i,y)==bytes((85,119,170)),('top content gap',i)
    assert pixel(x+i,y+h-1)==bytes((85,119,170)),('bottom content gap',i)
    assert pixel(x+i,y+h)==bytes((255,0,255)),('bottom border',i)
for i in range(h):
    assert pixel(x-1,y+i)==bytes((255,0,255)),('left border',i)
    assert pixel(x,y+i)==bytes((85,119,170)),('left content gap',i)
    assert pixel(x+w-1,y+i)==bytes((85,119,170)),('right content gap',i)
    assert pixel(x+w,y+i)==bytes((255,0,255)),('right border',i)
PY
echo "client touches all four borders through transparent slice padding"
# Modifier moves must take precedence over the frame's unmodified resize hit.
# Drag the right edge left far enough that an accidental resize hits minimum width.
"$UMBRIEL_POINTER_CLIENT" 1280 720 move "$((x+w))" "$((y+h/2))" mod logo press 272 pause 100 move "$((x+w-60))" "$((y+h/2+30))" pause 100 release 272 mod none
"$UMBRIEL" settle
read -r moved_x moved_y moved_w moved_h < <("$UMBRIEL" windows --json | jq -r '.[] | "\(.x) \(.y) \(.w) \(.h)"')
[[ $moved_w == "$w" && $moved_h == "$h" ]] || { echo "Super-drag on frame resized $w x $h to $moved_w x $moved_h"; exit 1; }
[[ $moved_x != "$x" && $moved_y != "$y" ]] || { echo "Super-drag on frame did not move the window"; exit 1; }
echo "Super-drag on textured frame moves without resizing"
# Detached tiled drags also retain their size through reattachment.
"$UMBRIEL" msg window-toggle-floating >/dev/null
"$UMBRIEL" msg window-set-primary-extent:0.5 >/dev/null
"$UMBRIEL" settle
read -r x y w h < <("$UMBRIEL" windows --json | jq -r '.[] | "\(.x) \(.y) \(.w) \(.h)"')
for start in center frame; do
  start_x=$((x+w/2)); [[ $start == frame ]] && start_x=$((x+w))
  "$UMBRIEL_POINTER_CLIENT" 1280 720 move "$start_x" "$((y+h/2))" mod logo press 272 pause 50 move "$((start_x-80))" "$((y+h/2+20))" pause 100 release 272 mod none
  "$UMBRIEL" settle
  read -r x y moved_w moved_h < <("$UMBRIEL" windows --json | jq -r '.[] | "\(.x) \(.y) \(.w) \(.h)"')
  [[ $moved_w == "$w" && $moved_h == "$h" ]] || { echo "Tiled Super-drag from $start resized $w x $h to $moved_w x $moved_h"; exit 1; }
done
echo "tiled Super-drags preserve size from client and frame"
