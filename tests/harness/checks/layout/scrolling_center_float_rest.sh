#!/usr/bin/env bash
# With center_focused = "always", floating the centered column keeps the strip where it rests for the whole float
# episode, because a float has no column to re-center on. The rest only survives while some remaining column can still
# be centered there: floating the last column pulls the strip back onto the new last one.
set -euo pipefail

declare -A CLIENT_PID=()

spawn_titled() {
  foot --title="$1" sh -c 'sleep 120' > /dev/null 2>&1 &
  CLIENT_PID[$1]=$!
}

window_count() { "$UMBRIEL" windows --json | jq 'length'; }

wait_for_count() {
  for _ in $(seq 80); do
    [[ $(window_count) -eq $1 ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for $1 window(s), have $(window_count)"
  return 1
}

field_of() {
  "$UMBRIEL" windows --json \
    | jq -r --arg t "$1" --arg f "$2" \
      '[.[] | select(.title == $t) | .[$f]] | if length == 1 then .[0] | tostring else "missing" end'
}

id_of() { field_of "$1" id; }

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[layout.scrolling]
direction = "horizontal"
center_focused = "always"
center_underfull_strip = true

[[window_rule]]
match.title = "^F2$"
default_floating = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

# T1 is column 0 and stays tiled for the whole check, never adjacent to the column that detaches (T2, the middle
# one): removing T2 does not renumber T1's column, so T1.x is a clean readout of the strip's scroll offset alone,
# undisturbed by the legitimate column-compaction a removal next door would also cause.
spawn_titled T1
wait_for_count 1
spawn_titled T2
wait_for_count 2
spawn_titled T3
wait_for_count 3
"$UMBRIEL" msg "window-focus:$(id_of T2)" > /dev/null
"$UMBRIEL" settle

t1_x_centered=$(field_of T1 x)
if [[ $t1_x_centered == "missing" ]]; then
  echo "T1 did not map: $("$UMBRIEL" windows --json)"
  exit 1
fi

"$UMBRIEL" msg window-toggle-floating > /dev/null
"$UMBRIEL" settle

t1_x_after_detach=$(field_of T1 x)
if [[ $t1_x_after_detach != "$t1_x_centered" ]]; then
  echo "strip moved when the centered column detached to float: T1.x $t1_x_centered -> $t1_x_after_detach"
  exit 1
fi

# A second float (opens floating directly, via the window rule above) opens and closes while focus stays on floats
# throughout, mirroring the reported episode.
spawn_titled F2
wait_for_count 4
"$UMBRIEL" msg "window-focus:$(id_of F2)" > /dev/null
"$UMBRIEL" settle
kill -TERM "${CLIENT_PID[F2]}" 2>/dev/null || true
wait_for_count 3
"$UMBRIEL" settle

t1_x_final=$(field_of T1 x)
if [[ $t1_x_final != "$t1_x_centered" ]]; then
  echo "strip did not keep its centered rest across the float episode: T1.x $t1_x_centered -> $t1_x_final"
  exit 1
fi

# T3 is the last column, so its centered rest lies past maxScroll. Once it floats, T1 is alone and the strip has to
# center it rather than keep resting where T3 was.
"$UMBRIEL" msg "window-focus:$(id_of T3)" > /dev/null
"$UMBRIEL" settle
"$UMBRIEL" msg window-toggle-floating > /dev/null
"$UMBRIEL" settle
t1_x=$(field_of T1 x)
t1_w=$(field_of T1 w)
if ((t1_x != (1280 - t1_w) / 2)); then
  echo "strip kept a stale centered rest after the last column floated: T1.x=$t1_x w=$t1_w"
  exit 1
fi

echo "the scrolling strip keeps its centered rest through a float episode and recenters once the rest outlives its column"
