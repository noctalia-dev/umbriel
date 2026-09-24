#!/usr/bin/env bash
# harness: outputs=1
# Fractional asymmetric content insets occupy layout slots.
set -euo pipefail
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
python3 - "$UMBRIEL_RUNTIME_DIR" <<'PY'
import struct,zlib,sys
from pathlib import Path
root=Path(sys.argv[1])
def write(name,channels,pixel):
    def chunk(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d))
    data=b''.join(b'\0'+b''.join(bytes(pixel(x,y)) for x in range(12)) for y in range(12))
    (root/name).write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',12,12,8,6 if channels==4 else 2,0,0,0))+chunk(b'IDAT',zlib.compress(data))+chunk(b'IEND',b''))
write('frame.png',4,lambda x,y:(255,0,255,255))
PY
cat >> "$UMBRIEL_CONFIG" <<EOF_CONFIG

[animation]
enabled = false
[layout]
mode = "dwindle"
gap = 0
[layout.scrolling]
default_extent_fraction = 0.5
[appearance]
use_nine_rect = true
[appearance.nine_rect]
texture = "$UMBRIEL_RUNTIME_DIR/frame.png"
content_inset = { top = 0.16666667, bottom = 0.25, left = 0.08333333, right = 0.16666667 }
slice_px = { top = 4, bottom = 4, left = 4, right = 4 }
[appearance.shadow]
enabled = false
EOF_CONFIG
"$UMBRIEL" msg config-reload >/dev/null
for name in nine-left nine-right; do
  RESIZE_FILL_COLOR=0xFF334455 "$CLIENT" "$name" 200 200 > "$UMBRIEL_RUNTIME_DIR/$name.log" 2>&1 &
  for _ in $(seq 100); do
    grep -q '^mapped$' "$UMBRIEL_RUNTIME_DIR/$name.log" && break
    sleep 0.025
  done
  "$UMBRIEL" settle
done
for mode in dwindle master scrolling; do
  "$UMBRIEL" msg "workspace-set-layout:$mode" >/dev/null
  "$UMBRIEL" settle
  "$UMBRIEL" windows --json > "$UMBRIEL_RUNTIME_DIR/layout.json"
  python3 - "$UMBRIEL_RUNTIME_DIR/layout.json" "$mode" <<'PY'
import json,sys
windows=json.load(open(sys.argv[1])); assert len(windows)==2
boxes=[(w['x']-1,w['y']-2,w['w']+3,w['h']+5) for w in windows]
assert min(b[0] for b in boxes)==0, (sys.argv[2],boxes)
assert min(b[1] for b in boxes)==0, (sys.argv[2],boxes)
assert max(b[0]+b[2] for b in boxes)==1280, (sys.argv[2],boxes)
assert max(b[1]+b[3] for b in boxes)==720, (sys.argv[2],boxes)
a,b=boxes
assert a[0]+a[2]<=b[0] or b[0]+b[2]<=a[0] or a[1]+a[3]<=b[1] or b[1]+b[3]<=a[1],boxes
assert sum(b[2]*b[3] for b in boxes)==1280*720,boxes
PY
done
"$UMBRIEL" msg overview-open >/dev/null
"$UMBRIEL" settle
grim -o HEADLESS-1 "$UMBRIEL_RUNTIME_DIR/cards.png"
magick "$UMBRIEL_RUNTIME_DIR/cards.png" -alpha off -depth 8 "rgb:$UMBRIEL_RUNTIME_DIR/cards.rgb"
python3 - "$UMBRIEL_RUNTIME_DIR/cards.rgb" <<'PY'
from pathlib import Path
import sys
b=Path(sys.argv[1]).read_bytes()
assert sum(b[i:i+3]==bytes((255,0,255)) for i in range(0,len(b),3))>100, 'overview lost textured decoration'
PY
"$UMBRIEL" msg overview-close >/dev/null
"$UMBRIEL" settle
echo "asymmetric frames align in all layouts and appear in overview"
