#!/usr/bin/env bash
# A reload that changes nothing does nothing; one that changes something applies only that. Reload used to re-apply every subsystem unconditionally, which is visible: the view loop clears every focus ring before refocus puts one back, so a no-op reload flickers and can land focus somewhere else entirely. This asserts the two halves of the fix, that an unchanged file is inert, and that a changed one still takes effect.
set -euo pipefail
readonly RECOVERY_ROOT="$UMBRIEL_CONFIG.recovery-root"
readonly RECOVERY_INCLUDE="${UMBRIEL_CONFIG%/*}/reload-recovery.toml"

spawn_client() {
  foot sh -c 'sleep 120' > /dev/null 2>&1 &
}

wait_for_count() {
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $1 ]] && return 0
    sleep 0.25
  done
  echo "timed out waiting for $1 window(s)"
  return 1
}

snapshot() { "$UMBRIEL" windows --json | jq -Sc '[.[] | {w, h, x, focused}] | sort_by(.x)'; }
log_mark() { wc -l < "$UMBRIEL_LOG"; }
log_since() { tail -n +"$1" "$UMBRIEL_LOG"; }

settle() {
  local previous="" current=""
  for _ in $(seq 40); do
    current=$(snapshot)
    [[ -n $previous && $current == "$previous" ]] && return 0
    previous=$current
    sleep 0.25
  done
  echo "state never settled (last: $previous)"
  return 1
}

spawn_client
spawn_client
wait_for_count 2 || exit 1
settle || exit 1

before=$(snapshot)

# Rewrite the file with identical content: mtime moves, content does not. The
# copy is also the pristine base every later variant is composed from, since
# redirecting into the file being read would truncate it first.
cp "$UMBRIEL_CONFIG" "$UMBRIEL_CONFIG.bak"
cat "$UMBRIEL_CONFIG.bak" > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
settle || exit 1

after=$(snapshot)
if [[ $before != "$after" ]]; then
  echo "a no-op reload changed the session"
  echo "  before: $before"
  echo "  after:  $after"
  exit 1
fi

# Window state alone cannot tell the two apart: re-applying everything lands on the same geometry and refocuses the same window. What it cannot fake is the
# reload reporting that it had nothing to do.
if ! tail -n 20 "$UMBRIEL_LOG" | grep -q "config reloaded (sections: none; effects: none)"; then
  echo "a no-op reload reported work:"
  tail -n 5 "$UMBRIEL_LOG" | sed "s/^/    /"
  exit 1
fi

# A real change must still land. Gaps feed the layout, so the tiles move.
{
  cat "$UMBRIEL_CONFIG.bak"
  printf '\n[layout]\ngap = 40\n'
} > "$UMBRIEL_CONFIG"
gap_log_mark=$(($(log_mark) + 1))
"$UMBRIEL" msg config-reload > /dev/null
settle || exit 1

changed=$(snapshot)
if [[ $changed == "$after" ]]; then
  echo "a gap change did not reach the layout (still $changed)"
  exit 1
fi
if ! tail -n 20 "$UMBRIEL_LOG" | grep -q "config reloaded (sections: layout; effects: workspace layout)"; then
  echo "a gap change was not reported as a layout change:"
  tail -n 5 "$UMBRIEL_LOG" | sed "s/^/    /"
  exit 1
fi
if log_since "$gap_log_mark" | grep -q "applied mode="; then
  echo "a gap change re-applied output state:"
  log_since "$gap_log_mark" | grep "applied mode=" | sed "s/^/    /"
  exit 1
fi

# Border widths feed both chrome and the resolved workspace spacing. Keep the
# gap fixed so a geometry change here proves the cross-section dependency.
{
  cat "$UMBRIEL_CONFIG.bak"
  printf '\n[layout]\ngap = 40\n'
  printf '\n[appearance]\nborder_width = 12\n'
} > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
settle || exit 1

bordered=$(snapshot)
if [[ $bordered == "$changed" ]]; then
  echo "a border width change did not refresh workspace spacing (still $bordered)"
  exit 1
fi
if ! tail -n 20 "$UMBRIEL_LOG" \
  | grep -q "config reloaded (sections: appearance; effects: workspace layout, view chrome)"; then
  echo "a border width change was not reported as an appearance change:"
  tail -n 5 "$UMBRIEL_LOG" | sed "s/^/    /"
  exit 1
fi

# A failed include parse must still leave that include watched. Fixing only the
# included file should recover automatically without another manual reload.
cp "$UMBRIEL_CONFIG" "$RECOVERY_ROOT"
printf '[layout\n' > "$RECOVERY_INCLUDE"
{
  printf '[include]\nfiles = ["reload-recovery.toml"]\n'
  cat "$RECOVERY_ROOT"
} > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
if [[ $(snapshot) != "$bordered" ]]; then
  echo "a failed include reload changed the live session"
  exit 1
fi

recovery_log_mark=$(($(log_mark) + 1))
printf '[workspaces]\nback_and_forth = true\n' > "$RECOVERY_INCLUDE"
for _ in $(seq 60); do
  if log_since "$recovery_log_mark" \
    | grep -q "config reloaded (sections: workspaces; effects: none)"; then
    recovered=true
    break
  fi
  sleep 0.1
done
if [[ ${recovered:-false} != true ]]; then
  echo "fixing a failed include did not trigger recovery"
  log_since "$recovery_log_mark" | tail -n 10 | sed "s/^/    /"
  exit 1
fi
if [[ $(snapshot) != "$bordered" ]]; then
  echo "an effect-free recovery changed the live session"
  exit 1
fi

echo "no-op reload inert; selective changes and failed-include recovery passed"
