#!/usr/bin/env bash
# harness: outputs=1
# Client blur remains rule-controlled and does not extend into transparent frames.
set -euo pipefail
readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
make_assets() {
  python3 - "$UMBRIEL_RUNTIME_DIR" <<'PY'
import struct,zlib,sys
from pathlib import Path
root=Path(sys.argv[1])
def write(name,channels,pixel):
    def chunk(t,d): return struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d))
    data=b''.join(b'\0'+b''.join(bytes(pixel(x,y)) for x in range(8)) for y in range(8))
    (root/name).write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',8,8,8,6 if channels==4 else 2,0,0,0))+chunk(b'IDAT',zlib.compress(data))+chunk(b'IEND',b''))
write('clear.png',4,lambda x,y:(0,0,0,0))
PY
}
make_assets
cat >> "$UMBRIEL_CONFIG" <<EOF_CONFIG

[animation]
enabled = false
[appearance]
use_nine_rect = true
[appearance.nine_rect]
texture = "$UMBRIEL_RUNTIME_DIR/clear.png"
slice_px = { top = 2, bottom = 2, left = 2, right = 2 }
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = true
optimized = false
passes = 2
radius = 6
noise = 0.0
brightness = 1.0
contrast = 1.0
saturation = 1.0
[[window_rule]]
match.title = "^blur-left$"
default_floating = true
default_floating_size_px = { width = 640, height = 720 }
default_position = { x = 0, y = 0, anchor = "top_left" }
blur = false
[[window_rule]]
match.title = "^blur-right$"
default_floating = true
default_floating_size_px = { width = 640, height = 720 }
default_position = { x = 640, y = 0, anchor = "top_left" }
blur = false
[[window_rule]]
match.title = "^blur-glass$"
default_floating = true
default_floating_size_px = { width = 80, height = 80 }
default_position = { x = 600, y = 200, anchor = "top_left" }
blur = false # client-blur-test
EOF_CONFIG
"$UMBRIEL" msg config-reload >/dev/null
FILL_COLOR=0xFFFFFFFF "$CLIENT" blur-left 640 720 > "$UMBRIEL_RUNTIME_DIR/left.log" 2>&1 &
FILL_COLOR=0xFF000000 "$CLIENT" blur-right 640 720 > "$UMBRIEL_RUNTIME_DIR/right.log" 2>&1 &
for _ in $(seq 100); do
  [[ $("$UMBRIEL" windows --json | jq 'length') == 2 ]] && break
  sleep 0.025
done
FILL_COLOR=0x00000000 "$CLIENT" blur-glass 80 80 > "$UMBRIEL_RUNTIME_DIR/glass.log" 2>&1 &
for _ in $(seq 100); do
  [[ $("$UMBRIEL" windows --json | jq 'length') == 3 ]] && break
  sleep 0.025
done
sample() {
  sed -i "s/^blur = .* # client-blur-test/blur = $1 # client-blur-test/" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload >/dev/null
  "$UMBRIEL" settle
  grim -o HEADLESS-1 "$UMBRIEL_RUNTIME_DIR/blur-$1.png"
  magick "$UMBRIEL_RUNTIME_DIR/blur-$1.png" -alpha off -format '%[fx:round(255*p{643,240}.r)]' info:
}
zero=$(sample false)
full=$(sample true)
(( zero < 3 && full > 20 )) || {
  echo "Client blur did not follow rule: off=$zero on=$full"
  exit 1
}
read -r x y w h < <("$UMBRIEL" windows --json | jq -r '.[] | select(.title=="blur-glass") | "\(.x) \(.y) \(.w) \(.h)"')
for row in "$((y-1))" "$((y+h))"; do
  edge=$(magick "$UMBRIEL_RUNTIME_DIR/blur-true.png" -alpha off -format "%[fx:round(255*p{643,$row}.r)]" info:)
  (( edge < 3 )) || { echo "blur escaped client at y=$row: $edge"; exit 1; }
done
echo "Client blur follows rules and leaves transparent frame bands unblurred"
