#!/usr/bin/env bash
# Queries report the latest successful scene commit immediately. Counters retain scanout history across composition,
# output-management transactions and power cycles, while settled changes are pushed to subscribers and logged.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/tests/subsurface-client}"
readonly OUTPUT_MANAGEMENT="${UMBRIEL_OUTPUT_MANAGEMENT_CLIENT:-./build-debug/tests/output-management-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/direct-scanout-client.log"
readonly EVENTS="$UMBRIEL_RUNTIME_DIR/direct-scanout-events.jsonl"

if [[ ! -x $CLIENT ]]; then
  echo "subsurface client not built at $CLIENT"
  exit 1
fi
if [[ ! -x $OUTPUT_MANAGEMENT ]]; then
  echo "output-management client not built at $OUTPUT_MANAGEMENT"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1
curve = "linear"
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" subscribe outputs > "$EVENTS" &

scanout() {
  "$UMBRIEL" outputs --json | jq -c '.[] | select(.name == "HEADLESS-1") | .direct_scanout'
}

await_query() {
  local predicate=$1 reason=$2 state=
  for _ in $(seq 60); do
    state=$(scanout)
    if jq -e "$predicate" <<< "$state" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "$reason: the latest committed direct_scanout is $state"
  exit 1
}

# Waits until the newest pushed outputs event satisfies a jq predicate on HEADLESS-1's direct_scanout.
await_scanout() {
  local predicate=$1 reason=$2
  for _ in $(seq 60); do
    if jq -e -s "[.[] | select(.event == \"outputs\")] | last | .data[]
        | select(.name == \"HEADLESS-1\") | .direct_scanout | $predicate" "$EVENTS" > /dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "$reason: the newest direct_scanout is $(jq -c -s '[.[] | select(.event == "outputs")] | last | .data[]
    | select(.name == "HEADLESS-1") | .direct_scanout' "$EVENTS" 2> /dev/null)"
  exit 1
}

await_scanout '.active == false and (.reason | type == "string") and .window == null' \
  "an empty output did not report inactive scanout with a reason"
initial=$(scanout)
if ! jq -e '.commits > 0 and .scanout_commits == 0 and .commit_seq > 0' <<< "$initial" > /dev/null; then
  echo "an empty output did not account for its initial composited frame: $initial"
  exit 1
fi

# The opaque child subsurface covers its parent exactly, so the fullscreen window is one visible buffer.
env INITIAL_FULLSCREEN=1 OPAQUE_CONTENT=1 "$CLIENT" direct-scanout > "$CLIENT_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "fullscreen client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi
window_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "direct-scanout") | .id')

await_query ".active and .reason == null and .window.id == \"$window_id\"
    and .commits > $(jq '.commits' <<< "$initial") and .scanout_commits > 0" \
  "mapping a fullscreen window did not advance successful scanout counters"
await_scanout ".active and .reason == null and .window.id == \"$window_id\" and .window.title == \"direct-scanout\"" \
  "a fullscreen window was not reported as scanned out"

# A query can include newer commits than the settled subscription snapshot.
current=$(scanout)
pushed=$(jq -c -s '[.[] | select(.event == "outputs")] | last | .data[]
  | select(.name == "HEADLESS-1") | .direct_scanout' "$EVENTS")
if ! jq -en --argjson current "$current" --argjson pushed "$pushed" '
    ($current | {active, reason, window}) == ($pushed | {active, reason, window})
    and $current.commits >= $pushed.commits
    and $current.scanout_commits >= $pushed.scanout_commits
    and $current.scanout_commits <= $current.commits' > /dev/null; then
  echo "the query and subscription disagree on scanout state or counter ordering: $current versus $pushed"
  exit 1
fi
if ! "$UMBRIEL" outputs | grep -q '^  Direct scanout: active (.* "direct-scanout")$'; then
  echo "the outputs listing does not show the scanned-out window: $("$UMBRIEL" outputs)"
  exit 1
fi

before_disabled=$(scanout)
printf '\n[output.HEADLESS-1]\ndirect_scanout = false\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
await_query ".active == false and .reason == \"disabled\" and .window == null
    and .commits > $(jq '.commits' <<< "$before_disabled")
    and .scanout_commits == $(jq '.scanout_commits' <<< "$before_disabled")" \
  "composition did not retain scanout history while advancing the scene counter"
await_scanout '.active == false and .reason == "disabled" and .window == null' \
  "turning direct scanout off for the output did not report it disabled"
if ! "$UMBRIEL" outputs | grep -q '^  Direct scanout: inactive (disabled)$'; then
  echo "the outputs listing does not show scanout disabled: $("$UMBRIEL" outputs)"
  exit 1
fi

# Backend validation does not commit or advance history. This static client has no pending repaint after settling.
before_test=$(scanout)
"$OUTPUT_MANAGEMENT" test disable HEADLESS-1 > /dev/null
after_test=$(scanout)
if [[ $before_test != "$after_test" ]]; then
  echo "testing an output-management configuration changed committed scanout history: $before_test versus $after_test"
  exit 1
fi

# Applying an unchanged enabled head still commits a scene through the grouped backend path. Every commit in this
# quiet phase carries a scene buffer, so the scene counter and backend sequence must advance by the same amount.
before_apply=$(scanout)
"$OUTPUT_MANAGEMENT" apply enable HEADLESS-1 > /dev/null
after_apply=$(scanout)
if ! jq -en --argjson before "$before_apply" --argjson after "$after_apply" '
    ($after.active | not) and $after.reason == "disabled" and $after.commits > $before.commits
    and ($after.commits - $before.commits) == ($after.commit_seq - $before.commit_seq)
    and $after.scanout_commits == $before.scanout_commits' > /dev/null; then
  echo "output-management scene commits were not counted exactly once: $before_apply versus $after_apply"
  exit 1
fi

# The power-off commit has no scene buffer. Power-on builds a configured scene frame, and history must survive both.
before_power=$(scanout)
"$UMBRIEL" msg dpms-off:HEADLESS-1 > /dev/null
if [[ $(scanout) != null ]]; then
  echo "a powered-off output still reports a live scanout state"
  exit 1
fi
"$UMBRIEL" msg dpms-on:HEADLESS-1 > /dev/null
after_power=$(scanout)
if ! jq -en --argjson before "$before_power" --argjson after "$after_power" '
    ($after.active | not) and $after.reason == "disabled" and $after.commits > $before.commits
    and ($after.commit_seq - $before.commit_seq) == ($after.commits - $before.commits + 1)
    and $after.scanout_commits == $before.scanout_commits' > /dev/null; then
  echo "configured power-on frames lost history or counted a bufferless power-off commit: $before_power versus $after_power"
  exit 1
fi

# Other output events can publish the immediate state before the log's settling timer expires.
for _ in $(seq 60); do
  grep -q "output 'HEADLESS-1': direct scanout off (disabled)" "$UMBRIEL_LOG" && break
  sleep 0.1
done
if ! grep -q "output 'HEADLESS-1': direct scanout on (.* \"direct-scanout\")" "$UMBRIEL_LOG"; then
  echo "scanout starting was not logged"
  exit 1
fi
if ! grep -q "output 'HEADLESS-1': direct scanout off (disabled)" "$UMBRIEL_LOG"; then
  echo "scanout stopping was not logged"
  exit 1
fi
# Each lasting change is logged once.
if (($(grep -c "output 'HEADLESS-1': direct scanout o" "$UMBRIEL_LOG") != 2)); then
  echo "expected two direct scanout log lines: $(grep "direct scanout" "$UMBRIEL_LOG")"
  exit 1
fi
