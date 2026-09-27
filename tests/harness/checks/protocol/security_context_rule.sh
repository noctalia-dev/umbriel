#!/usr/bin/env bash
# A [[security_context_rule]] grants extra globals to security-context clients whose metadata matches, on top of the
# fixed base allowlist. Grants do not leak between rules, an unmatched app keeps the base set, the security-context
# manager itself stays blocked even when a rule lists it, and a reload leaves connected clients as they were.
set -euo pipefail

readonly GLOBAL_CLIENT="${UMBRIEL_GLOBAL_CLIENT:-./build-debug/tests/global-client}"
readonly SECURITY_CONTEXT_CLIENT="${UMBRIEL_SECURITY_CONTEXT_CLIENT:-./build-debug/tests/security-context-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/security-context-rule-client.log"
readonly CONTROL_FIFO="$UMBRIEL_RUNTIME_DIR/security-context-rule-control"

if [[ ! -x $GLOBAL_CLIENT || ! -x $SECURITY_CONTEXT_CLIENT ]]; then
  echo "required harness clients are not built"
  exit 1
fi

# Baseline: without rules the globals granted below are hidden.
"$SECURITY_CONTEXT_CLIENT" "$GLOBAL_CLIENT" zwlr_layer_shell_v1 absent
"$SECURITY_CONTEXT_CLIENT" "$GLOBAL_CLIENT" ext_data_control_manager_v1 absent

# A client connected before the reload keeps the registry it was given, even when it reads the registry again.
mkfifo "$CONTROL_FIFO"
exec {control_fd}<>"$CONTROL_FIFO"
"$SECURITY_CONTEXT_CLIENT" "$GLOBAL_CLIENT" recheck zwlr_layer_shell_v1 absent <&"$control_fd" > "$CLIENT_LOG" 2>&1 &
client_pid=$!

for _ in $(seq 100); do
  grep -q '^ready$' "$CLIENT_LOG" && break
  if ! kill -0 "$client_pid" 2>/dev/null; then
    wait "$client_pid" 2>/dev/null || true
    echo "persistent restricted client exited before its recheck: $(< "$CLIENT_LOG")"
    exit 1
  fi
  sleep 0.02
done
if ! grep -q '^ready$' "$CLIENT_LOG"; then
  echo "persistent restricted client did not become ready: $(< "$CLIENT_LOG")"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" << 'EOF'

[[security_context_rule]]
match.sandbox_engine = '^org\.umbriel\.harness$'
match.app_id = '^org\.umbriel\.SecurityContextTest$'
allow_globals = ["zwlr_layer_shell_v1", "wp_security_context_manager_v1"]

[[security_context_rule]]
match.app_id = '^org\.umbriel\.OtherApp$'
allow_globals = ["ext_data_control_manager_v1"]

[[security_context_rule]]
match.app_id = 'SecurityContextTest'
allow_globals = ["ext_workspace_manager_v1"]
EOF
"$UMBRIEL" msg config-reload > /dev/null

if ! tail -n 20 "$UMBRIEL_LOG" | grep -q "config reloaded (sections: security context rules; effects: none)"; then
  echo "reload did not report the security context rules section:"
  tail -n 5 "$UMBRIEL_LOG" | sed "s/^/    /"
  exit 1
fi

printf '\n' >&"$control_fd"
if ! wait "$client_pid"; then
  echo "a connected client saw its grants change after the reload: $(< "$CLIENT_LOG")"
  exit 1
fi

# The matching rule grants layer-shell to connections made after the reload.
"$SECURITY_CONTEXT_CLIENT" "$GLOBAL_CLIENT" zwlr_layer_shell_v1 present
# The other rule's grant does not leak across app ids.
"$SECURITY_CONTEXT_CLIENT" "$GLOBAL_CLIENT" ext_data_control_manager_v1 absent
SECURITY_CONTEXT_APP_ID=org.umbriel.OtherApp "$SECURITY_CONTEXT_CLIENT" "$GLOBAL_CLIENT" \
  ext_data_control_manager_v1 present
SECURITY_CONTEXT_APP_ID=org.umbriel.OtherApp "$SECURITY_CONTEXT_CLIENT" "$GLOBAL_CLIENT" \
  zwlr_layer_shell_v1 absent
# A pattern must match the entire app ID; a substring grants nothing.
"$SECURITY_CONTEXT_CLIENT" "$GLOBAL_CLIENT" ext_workspace_manager_v1 absent
# Listing the manager in allow_globals never enables nested contexts.
"$SECURITY_CONTEXT_CLIENT" "$GLOBAL_CLIENT" wp_security_context_manager_v1 absent
# Clients on the ordinary socket are unaffected by the rules.
"$GLOBAL_CLIENT" zwlr_layer_shell_v1 present

echo "security context rules grant extra globals per matching app"
