#!/usr/bin/env bash
# Shrinking a static workspace list has to relocate every window off each removed workspace. The relocation loop walks
# the source workspace's own view list while setWorkspace() erases from it, so this needs three windows: erasing the
# first shifts the third into the second slot, and an iteration that reads the live vector then skips the middle
# window and revisits the last one. The skipped window keeps a pointer to a workspace that is freed moments later.
# Asserting that every window reports the one surviving workspace id observes that transition, where the relocation
# count alone cannot: the double visit keeps the tally at three.
set -euo pipefail

readonly WORKSPACE="${UMBRIEL_WORKSPACE_CLIENT:-./build-debug/workspace-client}"

window_count() { "$UMBRIEL" windows --json | jq 'length'; }

wait_for_count() {
  for _ in $(seq 60); do
    [[ $(window_count) -eq $1 ]] && return 0
    sleep 0.1
  done
  echo "timed out waiting for $1 window(s), have $(window_count)"
  return 1
}

# Distinct workspace ids the mapped windows claim, as ids rather than names: a window stranded on a destroyed
# workspace keeps that workspace's id, and ids are never reissued, so the stale one cannot alias a survivor.
window_workspace_ids() { "$UMBRIEL" windows --json | jq -r '[.[].workspace] | unique | join(" ")'; }

workspace_id_named() { "$WORKSPACE" --all | awk -F'\t' -v name="$1" '$2 == name { print $1 }'; }

printf '\n[output.HEADLESS-1]\nworkspaces = ["one", "two", "three"]\n' >> "$UMBRIEL_CONFIG"
cp "$UMBRIEL_CONFIG" "$UMBRIEL_CONFIG.three"
"$UMBRIEL" msg config-reload > /dev/null

"$UMBRIEL" msg workspace-switch:three > /dev/null
for index in a b c; do
  foot --title="shrink-$index" sh -c 'sleep 120' > /dev/null 2>&1 &
done
wait_for_count 3 || exit 1

doomed=$(workspace_id_named three)
if [[ $(window_workspace_ids) != "$doomed" ]]; then
  echo "windows did not start together on the workspace about to be removed: $(window_workspace_ids), want $doomed"
  exit 1
fi

mark=$(wc -l < "$UMBRIEL_LOG")
sed 's/^workspaces = .*/workspaces = ["one", "two"]/' "$UMBRIEL_CONFIG.three" > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null

survivor=$(workspace_id_named two)
if [[ -z $survivor ]]; then
  echo "workspace 'two' did not survive the shrink: $("$WORKSPACE" --all)"
  exit 1
fi
if [[ $(window_workspace_ids) != "$survivor" ]]; then
  echo "a window was left behind on the removed workspace: ids=$(window_workspace_ids), removed=$doomed, want $survivor"
  tail -n +"$mark" "$UMBRIEL_LOG" | sed 's/^/    /'
  exit 1
fi
if [[ $(window_count) -ne 3 ]]; then
  echo "expected all three windows to survive the shrink, got $(window_count)"
  exit 1
fi

echo "shrinking a static workspace list moved all three windows off the removed workspace onto 'two'"
