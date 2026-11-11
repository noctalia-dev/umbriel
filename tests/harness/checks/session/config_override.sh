#!/usr/bin/env bash
# A runtime override reaches the live layout, outlives a reload of a file that sets the same key, and unsetting it
# hands the key back to the file.
set -euo pipefail

snapshot() { "$UMBRIEL" windows --json | jq -Sc '[.[] | {x, w, h}] | sort_by(.x)'; }

foot sh -c 'sleep 120' > /dev/null 2>&1 &
foot sh -c 'sleep 120' > /dev/null 2>&1 &
for _ in $(seq 60); do
  [[ $("$UMBRIEL" windows --json | jq 'length') -eq 2 ]] && break
  sleep 0.1
done
"$UMBRIEL" settle
base=$(snapshot)

"$UMBRIEL" config set layout.gap 40
"$UMBRIEL" settle
gapped=$(snapshot)
if [[ $("$UMBRIEL" config get layout.gap) != 40 || $gapped == "$base" ]]; then
  echo "config set did not reach the layout: get=$("$UMBRIEL" config get layout.gap) $gapped"
  exit 1
fi
if ! tail -n 20 "$UMBRIEL_LOG" | grep -q "config reloaded (sections: layout; effects: workspace layout)"; then
  echo "config set was not applied as a layout change:"
  tail -n 5 "$UMBRIEL_LOG" | sed "s/^/    /"
  exit 1
fi

printf '\n[layout]\ngap = 5\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
if [[ $("$UMBRIEL" config get layout.gap --json) != '{"override":true,"value":40}' || $(snapshot) != "$gapped" ]]; then
  echo "a file reload undid the override: $("$UMBRIEL" config get layout.gap --json) $(snapshot)"
  exit 1
fi

if "$UMBRIEL" config set layout.gap '"wide"' 2> "$UMBRIEL_RUNTIME_DIR/refused"; then
  echo "an invalid override was accepted"
  exit 1
fi
if ! grep -q "layout.gap" "$UMBRIEL_RUNTIME_DIR/refused" || [[ $("$UMBRIEL" config get layout.gap) != 40 ]]; then
  echo "a refused override was not reported or changed the value: $(cat "$UMBRIEL_RUNTIME_DIR/refused")"
  exit 1
fi

"$UMBRIEL" config unset layout.gap
"$UMBRIEL" settle
if [[ $("$UMBRIEL" config get layout.gap --json) != '{"override":false,"value":5}' || $(snapshot) == "$gapped" ]]; then
  echo "config unset did not hand the key back to the file: $("$UMBRIEL" config get layout.gap --json) $(snapshot)"
  exit 1
fi
if "$UMBRIEL" config unset layout.gap 2> /dev/null; then
  echo "unsetting a key without an override succeeded"
  exit 1
fi

echo "override applied, survived a reload, refused a bad value, and unset back to the file"
