# Helpers shared by harness checks. A check sources it with: source "$UMBRIEL_HARNESS_LIB"

# Runs the pointer helper through a sequence and holds the resulting input state (a pressed button, a held modifier)
# until pointer_release: pointer_hold <width> <height> <command>... [-- <command>...]. It returns once every command
# before `--` has been processed; the commands after `--` run on release. One hold at a time.
pointer_hold() {
  local width=$1 height=$2
  shift 2
  local -a before=()
  while (($# > 0)) && [[ $1 != -- ]]; do
    before+=("$1")
    shift
  done
  (($# > 0)) && shift
  local fifo=$UMBRIEL_RUNTIME_DIR/pointer-hold.fifo
  POINTER_HOLD_LOG=$UMBRIEL_RUNTIME_DIR/pointer-hold.log
  rm -f "$fifo"
  mkfifo "$fifo"
  "$UMBRIEL_POINTER_CLIENT" "$width" "$height" "${before[@]}" mark held hold "$@" < "$fifo" > "$POINTER_HOLD_LOG" 2>&1 &
  POINTER_HOLD_PID=$!
  exec {POINTER_HOLD_FD}> "$fifo"
  for _ in $(seq 200); do
    grep -q '^held$' "$POINTER_HOLD_LOG" && return 0
    sleep 0.025
  done
  echo "the pointer helper never reached its hold: $(< "$POINTER_HOLD_LOG")"
  return 1
}

# Several holds in one run: put `mark <label> hold` among the commands after `--`, and pointer_step <label> runs the
# helper on to that hold.
pointer_step() {
  echo >&"$POINTER_HOLD_FD"
  for _ in $(seq 200); do
    grep -q "^$1\$" "$POINTER_HOLD_LOG" && return 0
    sleep 0.025
  done
  echo "the pointer helper never reached $1: $(< "$POINTER_HOLD_LOG")"
  return 1
}

# Runs the remaining commands after `--` and waits for the helper to exit.
pointer_release() {
  echo >&"$POINTER_HOLD_FD"
  exec {POINTER_HOLD_FD}>&-
  wait "$POINTER_HOLD_PID"
}

# The window observer suffixes its enter with the held-key count, so events are matched as line prefixes.
events() {
  local count
  count=$(grep -c "^$2" "$1" 2>/dev/null) || true
  echo "${count:-0}"
}

await_events() {
  local file=$1 event=$2 expected=$3 label=${4:-$1}
  for _ in $(seq 60); do
    (($(events "$file" "$event") >= expected)) && return 0
    sleep 0.1
  done
  echo "timed out waiting for $expected '$event' on $label: $(tr '\n' '|' < "$file")"
  return 1
}

# Waits for at least $3 lines matching $2 in $1, then returns. Poll step is $4 (default 0.1s), matching
# await_events; a check whose waits are tight under parallel stress passes a shorter step.
await_lines() {
  local file=$1 pattern=$2 expected=$3 step=${4:-0.1}
  for _ in $(seq 60); do
    (($(events "$file" "$pattern") >= expected)) && return 0
    sleep "$step"
  done
  echo "timed out waiting for $expected '$pattern' in $file: $(tr '\n' '|' < "$file")"
  return 1
}

# Shared fixture for live workspace reveal checks.
workspace_reveal_config() {
  cp "$UMBRIEL_REPO/examples/effects/animation/workspace_wipe/shader.glsl" "$UMBRIEL_RUNTIME_DIR/wipe.glsl"
  cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = true
[animation.windows_in]
enabled = false
[animation.windows_out]
enabled = false
[animation.windows_move]
enabled = false
[animation.workspaces]
style = "reveal"
effect = "wipe"
duration_ms = 1000
curve = "linear"
[effects.preset.wipe]
kind = "animation"
shader = "wipe.glsl"
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
CONFIG
}

# Builds the matrix-shaper ICC v2 display profile the color checks pin: Display-P3-like colorants with a gamma 1.8 TRC,
# wide enough to desaturate pure red under "srgb" and far enough from any named curve for the power-fit path. An optional
# second argument inflates the file with a 'pad ' tag of that many MiB, kept inside the profile so the header size and
# the tag table still describe the whole file: clients must budget retained bytes for the file exactly as received.
write_icc_matrix_profile() {
  local path=$1 pad_mib=${2:-0}
  python3 - "$path" "$pad_mib" <<'PY'
import struct
import sys


def s15(value):
    return struct.pack('>i', round(value * 65536))


def xyz(x, y):
    return (x / y, 1.0, (1.0 - x - y) / y)


def inv3(m):
    det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6])
    return [
        (m[4] * m[8] - m[5] * m[7]) / det,
        (m[2] * m[7] - m[1] * m[8]) / det,
        (m[1] * m[5] - m[2] * m[4]) / det,
        (m[5] * m[6] - m[3] * m[8]) / det,
        (m[0] * m[8] - m[2] * m[6]) / det,
        (m[2] * m[3] - m[0] * m[5]) / det,
        (m[3] * m[7] - m[4] * m[6]) / det,
        (m[1] * m[6] - m[0] * m[7]) / det,
        (m[0] * m[4] - m[1] * m[3]) / det,
    ]


def mulv(a, v):
    return [a[i * 3] * v[0] + a[i * 3 + 1] * v[1] + a[i * 3 + 2] * v[2] for i in range(3)]


def mul(a, b):
    return [sum(a[i * 3 + k] * b[k * 3 + j] for k in range(3)) for i in range(3) for j in range(3)]


def bradford_adapt(frm, to):
    brad = [0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296]
    scale = [t / f for t, f in zip(mulv(brad, to), mulv(brad, frm))]
    return mul(
        inv3(brad),
        mul([scale[0], 0.0, 0.0, 0.0, scale[1], 0.0, 0.0, 0.0, scale[2]], brad),
    )


primaries = {
    'rXYZ': (0.680, 0.320),
    'gXYZ': (0.265, 0.690),
    'bXYZ': (0.150, 0.060),
    'wtpt': (0.3127, 0.3290),
}
# Spec-correct layout: unit-luminance columns scaled so the three colorants sum
# exactly to the declared white, written PCS-relative (Bradford-adapted to D50)
# beside a measured wtpt tag, the v2 display-profile shape. A parser that
# recovers the measured colorimetry therefore reports the xy values above.
columns = {name: xyz(*xy) for name, xy in primaries.items()}
white = columns['wtpt']
unit = [columns['rXYZ'], columns['gXYZ'], columns['bXYZ']]
weights = mulv(inv3([unit[j][i] for i in range(3) for j in range(3)]), white)
to_pcs = bradford_adapt(white, (0.9642, 1.0, 0.8249))
blobs = {}
for index, name in enumerate(('rXYZ', 'gXYZ', 'bXYZ')):
    column = mulv(to_pcs, [weights[index] * value for value in unit[index]])
    blobs[name.encode()] = b'XYZ ' + b'\0' * 4 + s15(column[0]) + s15(column[1]) + s15(column[2])
blobs[b'wtpt'] = b'XYZ ' + b'\0' * 4 + s15(white[0]) + s15(white[1]) + s15(white[2])
curve = b'curv' + b'\0' * 4 + struct.pack('>I', 1) + struct.pack('>H', round(1.8 * 256))
for name in ('rTRC', 'gTRC', 'bTRC'):
    blobs[name.encode()] = curve
pad_mib = int(sys.argv[2])
if pad_mib > 0:
    blobs[b'pad '] = b'pad ' + b'\0' * (pad_mib * 1024 * 1024)

order = sorted(blobs)
unique = []
offsets = {}
cursor = 128 + 4 + 12 * len(order)
for name in order:
    blob = blobs[name]
    if blob not in unique:
        unique.append(blob)
        offsets[blob] = cursor
        cursor += len(blob)
        cursor += (-cursor) % 4

header = bytearray(128)
struct.pack_into('>I', header, 0, cursor)
header[4:8] = b'lcms'
struct.pack_into('>I', header, 8, 0x02400000)
header[12:16] = b'mntr'
header[16:20] = b'RGB '
header[20:24] = b'XYZ '
header[36:40] = b'acsp'
header[40:44] = b'APPL'
header[68:80] = s15(0.9642) + s15(1.0) + s15(0.8249)
header[80:84] = b'lcms'

out = bytearray(header)
out += struct.pack('>I', len(order))
for name in order:
    blob = blobs[name]
    out += name + struct.pack('>II', offsets[blob], len(blob))
for blob in unique:
    out += blob
    out += b'\0' * ((-len(out)) % 4)
assert len(out) == cursor

with open(sys.argv[1], 'wb') as handle:
    handle.write(out)
print(' '.join(f'{value:.6g}' for name in ('rXYZ', 'gXYZ', 'bXYZ', 'wtpt') for value in primaries[name]))
PY
}

# Query a mapped window by its exact title; wait for it before reading its state.
window_by_title() { "$UMBRIEL" windows --json | jq -ce --arg title "$1" '.[] | select(.title == $title)'; }
await_window() {
  for _ in $(seq 100); do
    window_by_title "$1" > /dev/null && return
    sleep 0.025
  done
  echo "missing window: $1"; return 1
}

# Plain colours make capture pixels independent of decorations and animation.
capture_config() {
  cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.title_bar]
mode = "off"
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = false
CONFIG
}

# Two repaintable windows under an opaque magenta window. FDs 8/9 control
# the lower clients; each check supplies its own layout and protection rules.
capture_clients() {
  mkfifo "$UMBRIEL_RUNTIME_DIR/left" "$UMBRIEL_RUNTIME_DIR/right"
  exec 8<> "$UMBRIEL_RUNTIME_DIR/left"
  exec 9<> "$UMBRIEL_RUNTIME_DIR/right"
  REPAINT_ON_STDIN=1 FOLLOW_CONFIGURES=1 FILL_COLOR=0xFF00FF00 \
    "$UMBRIEL_UNMAP_CLIENT" left "$1" "$1" <&8 > "$UMBRIEL_RUNTIME_DIR/left.log" 2>&1 &
  await_window left
  REPAINT_ON_STDIN=1 FOLLOW_CONFIGURES=1 FILL_COLOR=0xFF0000FF \
    "$UMBRIEL_UNMAP_CLIENT" right "$1" "$1" <&9 > "$UMBRIEL_RUNTIME_DIR/right.log" 2>&1 &
  await_window right
  OPAQUE_BUFFER=1 FOLLOW_CONFIGURES=1 FILL_COLOR=0xFFFF00FF \
    "$UMBRIEL_UNMAP_CLIENT" protected 1280 720 > "$UMBRIEL_RUNTIME_DIR/protected.log" 2>&1 &
  await_window protected
}

# Save the frame for subsequent colour/geometry assertions as well.
capture_assert_hidden() {
  grim "$1"
  local count
  count=$("$UMBRIEL_PIXEL_PROBE" "$1" count 'r>0.9&&b>0.9&&g<0.1')
  [[ $count == 0 ]] || { echo "protected pixels leaked: $count"; return 1; }
}

# The first protection field belongs to the magenta window. Later rules stay
# unchanged so feedback tests can keep another owner protected throughout.
capture_set_protection() {
  sed -i "0,/^block_capture = /s/^block_capture = .*/block_capture = $1/" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
}
