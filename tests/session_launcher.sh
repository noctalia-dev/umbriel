#!/usr/bin/env bash
set -euo pipefail

readonly SOURCE_LAUNCHER=${1:?usage: session_launcher.sh <start-umbriel>}
readonly SOURCE_DIRECT_LAUNCHER=${2:?usage: session_launcher.sh <start-umbriel> <direct-start-umbriel>}
readonly TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT

readonly TEST_HOME="$TEST_DIR/home"
readonly TEST_BIN="$TEST_DIR/bin"
readonly PROFILE_TRACE="$TEST_DIR/profile-trace"
readonly IMPORT_TRACE="$TEST_DIR/import-trace"
readonly SERVICE_ENV="$TEST_DIR/service-environment"
readonly SERVICE_SESSION_ENV="$TEST_DIR/service-session-environment"
readonly DBUS_ENV="$TEST_DIR/dbus-environment"
readonly DIRECT_ENV="$TEST_DIR/direct-environment"
readonly DIRECT_ARGUMENTS="$TEST_DIR/direct-arguments"
readonly DIRECT_SESSION_ENV="$TEST_DIR/direct-session-environment"
readonly NESTED_ENV="$TEST_DIR/nested-environment"
readonly NESTED_ARGUMENTS="$TEST_DIR/nested-arguments"
readonly NESTED_SESSION_ENV="$TEST_DIR/nested-session-environment"
readonly EXTERNAL_ENV="$TEST_DIR/external-environment"
readonly EXTERNAL_ARGUMENTS="$TEST_DIR/external-arguments"
readonly EXTERNAL_SESSION_ENV="$TEST_DIR/external-session-environment"
readonly LAUNCHER="$TEST_DIR/start-umbriel"
readonly DIRECT_LAUNCHER="$TEST_DIR/direct-start-umbriel"
readonly DIRECT_TARGET="$TEST_DIR/direct-target"
readonly INJECTION_MARKER="$TEST_DIR/injected"
readonly DRS_INJECTION_MARKER="$TEST_DIR/drs-injected"
readonly DANGEROUS_ARGUMENT="\$(touch '$INJECTION_MARKER')"

mkdir -p "$TEST_HOME" "$TEST_BIN"
sed 's|/etc/shells|"$HOME/shells"|g' "$SOURCE_LAUNCHER" > "$LAUNCHER"
sed 's|/etc/shells|"$HOME/shells"|g' "$SOURCE_DIRECT_LAUNCHER" > "$DIRECT_LAUNCHER"
chmod 700 "$LAUNCHER" "$DIRECT_LAUNCHER"

login_shell=$BASH
printf '%s\n' "$login_shell" > "$TEST_HOME/shells"

cat > "$TEST_HOME/.bash_profile" <<'EOF'
printf 'profile\n' >> "$UMBRIEL_TEST_PROFILE_TRACE"
export UMBRIEL_LOGIN_PROFILE_MARKER='from login profile'
export PATH="$UMBRIEL_TEST_BIN:$UMBRIEL_TEST_HOST_PATH"
export DISPLAY=:from-login-profile
export WAYLAND_DISPLAY=profile-wayland
export WAYLAND_SOCKET=42
export UMBRIEL_SOCKET=profile-umbriel
export XDG_CURRENT_DESKTOP=profile-current
export XDG_SESSION_DESKTOP=profile-session
export XDG_SESSION_TYPE=profile-type
EOF

cat > "$TEST_BIN/systemctl" <<'EOF'
#!/bin/sh
if [ "${2:-}" = "show-environment" ]; then
  [ "${UMBRIEL_TEST_SYSTEMD_AVAILABLE:-true}" = true ]
  exit $?
fi
if [ "${3:-}" = "is-active" ]; then
  exit 1
fi
if [ "${2:-}" = "import-environment" ]; then
  printf '%s\n' "$@" > "$UMBRIEL_TEST_IMPORT_TRACE"
  exit 0
fi
if [ "${3:-}" = "start" ] && [ "${4:-}" = "umbriel.service" ]; then
  printf '%s\n' "${UMBRIEL_LOGIN_PROFILE_MARKER:-missing}" > "$UMBRIEL_TEST_SERVICE_ENV"
  {
    printf 'WAYLAND_DISPLAY=%s\n' "${WAYLAND_DISPLAY-unset}"
    printf 'WAYLAND_SOCKET=%s\n' "${WAYLAND_SOCKET-unset}"
    printf 'DISPLAY=%s\n' "${DISPLAY-unset}"
    printf 'UMBRIEL_SOCKET=%s\n' "${UMBRIEL_SOCKET-unset}"
    printf 'XDG_CURRENT_DESKTOP=%s\n' "${XDG_CURRENT_DESKTOP-unset}"
    printf 'XDG_SESSION_DESKTOP=%s\n' "${XDG_SESSION_DESKTOP-unset}"
    printf 'XDG_SESSION_TYPE=%s\n' "${XDG_SESSION_TYPE-unset}"
  } > "$UMBRIEL_TEST_SERVICE_SESSION_ENV"
  exit 0
fi
exit 0
EOF

cat > "$TEST_BIN/dbus-update-activation-environment" <<'EOF'
#!/bin/sh
if [ "$#" -ne 1 ] || [ "$1" != "--all" ]; then
  exit 1
fi
printf '%s\n' "${UMBRIEL_LOGIN_PROFILE_MARKER:-missing}" > "$UMBRIEL_TEST_DBUS_ENV"
EOF

cat > "$DIRECT_TARGET" <<'EOF'
#!/bin/sh
printf '%s\n' "${UMBRIEL_LOGIN_PROFILE_MARKER:-missing}" > "$UMBRIEL_TEST_DIRECT_ENV"
printf '%s\n' "$@" > "$UMBRIEL_TEST_DIRECT_ARGUMENTS"
{
  printf 'WAYLAND_DISPLAY=%s\n' "${WAYLAND_DISPLAY-unset}"
  printf 'WAYLAND_SOCKET=%s\n' "${WAYLAND_SOCKET-unset}"
  printf 'DISPLAY=%s\n' "${DISPLAY-unset}"
  printf 'UMBRIEL_SOCKET=%s\n' "${UMBRIEL_SOCKET-unset}"
  printf 'XDG_CURRENT_DESKTOP=%s\n' "${XDG_CURRENT_DESKTOP-unset}"
  printf 'XDG_SESSION_DESKTOP=%s\n' "${XDG_SESSION_DESKTOP-unset}"
  printf 'XDG_SESSION_TYPE=%s\n' "${XDG_SESSION_TYPE-unset}"
} > "$UMBRIEL_TEST_SESSION_ENV"
EOF

chmod 700 "$TEST_BIN/systemctl" "$TEST_BIN/dbus-update-activation-environment"

env -i \
  HOME="$TEST_HOME" \
  SHELL="$login_shell" \
  USER="${USER:-umbriel-test}" \
  LOGNAME="${LOGNAME:-umbriel-test}" \
  PATH="$TEST_BIN:$PATH" \
  DBUS_SESSION_BUS_ADDRESS="unix:path=$TEST_DIR/nonexistent-bus" \
  UMBRIEL_TEST_PROFILE_TRACE="$PROFILE_TRACE" \
  UMBRIEL_TEST_IMPORT_TRACE="$IMPORT_TRACE" \
  UMBRIEL_TEST_SERVICE_ENV="$SERVICE_ENV" \
  UMBRIEL_TEST_SERVICE_SESSION_ENV="$SERVICE_SESSION_ENV" \
  UMBRIEL_TEST_DBUS_ENV="$DBUS_ENV" \
  UMBRIEL_TEST_BIN="$TEST_BIN" \
  UMBRIEL_TEST_HOST_PATH="$PATH" \
  UMBRIEL_TEST_SYSTEMD_AVAILABLE=true \
  drs="touch $DRS_INJECTION_MARKER" \
  "$LAUNCHER" plain "two words" 'semi;colon' "quote'and\"double"

if [[ ! -f $PROFILE_TRACE || $(< "$PROFILE_TRACE") != profile ]]; then
  echo "login profile did not run exactly once"
  exit 1
fi
if ! grep -Fxq UMBRIEL_LOGIN_PROFILE_MARKER "$IMPORT_TRACE"; then
  echo "profile variable was not included in the systemd environment import"
  exit 1
fi
if [[ $(< "$SERVICE_ENV") != 'from login profile' ]]; then
  echo "profile variable did not reach the compositor service"
  exit 1
fi
if [[ $(< "$DBUS_ENV") != 'from login profile' ]]; then
  echo "profile variable did not reach D-Bus activation"
  exit 1
fi
cat > "$TEST_DIR/expected-native-session-environment" <<'EOF'
WAYLAND_DISPLAY=unset
WAYLAND_SOCKET=unset
DISPLAY=unset
UMBRIEL_SOCKET=unset
XDG_CURRENT_DESKTOP=unset
XDG_SESSION_DESKTOP=unset
XDG_SESSION_TYPE=unset
EOF
if ! diff -u "$TEST_DIR/expected-native-session-environment" "$SERVICE_SESSION_ENV"; then
  echo "profile graphical variables reached the compositor service"
  exit 1
fi

env -i \
  HOME="$TEST_HOME" \
  SHELL="$login_shell" \
  USER="${USER:-umbriel-test}" \
  LOGNAME="${LOGNAME:-umbriel-test}" \
  PATH="$TEST_BIN:$PATH" \
  DBUS_SESSION_BUS_ADDRESS="unix:path=$TEST_DIR/nonexistent-bus" \
  UMBRIEL_TEST_PROFILE_TRACE="$PROFILE_TRACE" \
  UMBRIEL_TEST_DIRECT_ENV="$DIRECT_ENV" \
  UMBRIEL_TEST_DIRECT_ARGUMENTS="$DIRECT_ARGUMENTS" \
  UMBRIEL_TEST_SESSION_ENV="$DIRECT_SESSION_ENV" \
  UMBRIEL_TEST_BIN="$TEST_BIN" \
  UMBRIEL_TEST_HOST_PATH="$PATH" \
  UMBRIEL_TEST_SYSTEMD_AVAILABLE=false \
  drs="touch $DRS_INJECTION_MARKER" \
  "$DIRECT_LAUNCHER" "$DIRECT_TARGET" plain "two words" 'semi;colon' "quote'and\"double" "$DANGEROUS_ARGUMENT"

if [[ $(wc -l < "$PROFILE_TRACE") -ne 2 ]]; then
  echo "login profile did not run exactly once for each native launch"
  exit 1
fi
if [[ $(< "$DIRECT_ENV") != 'from login profile' ]]; then
  echo "profile variable did not reach the direct compositor fallback"
  exit 1
fi
printf '%s\n' plain "two words" 'semi;colon' "quote'and\"double" "$DANGEROUS_ARGUMENT" > "$TEST_DIR/expected-arguments"
if ! diff -u "$TEST_DIR/expected-arguments" "$DIRECT_ARGUMENTS"; then
  echo "login shell re-entry changed launcher arguments"
  exit 1
fi
if [[ -e $INJECTION_MARKER ]]; then
  echo "a launcher argument was evaluated as shell input"
  exit 1
fi
if ! diff -u "$TEST_DIR/expected-native-session-environment" "$DIRECT_SESSION_ENV"; then
  echo "profile graphical variables reached the direct compositor fallback"
  exit 1
fi

env -i \
  HOME="$TEST_HOME" \
  SHELL="$login_shell" \
  USER="${USER:-umbriel-test}" \
  LOGNAME="${LOGNAME:-umbriel-test}" \
  PATH="$TEST_BIN:$PATH" \
  DBUS_SESSION_BUS_ADDRESS="unix:path=$TEST_DIR/nonexistent-bus" \
  WAYLAND_DISPLAY=wayland-test \
  UMBRIEL_TEST_PROFILE_TRACE="$PROFILE_TRACE" \
  UMBRIEL_TEST_DIRECT_ENV="$NESTED_ENV" \
  UMBRIEL_TEST_DIRECT_ARGUMENTS="$NESTED_ARGUMENTS" \
  UMBRIEL_TEST_SESSION_ENV="$NESTED_SESSION_ENV" \
  UMBRIEL_TEST_BIN="$TEST_BIN" \
  UMBRIEL_TEST_HOST_PATH="$PATH" \
  UMBRIEL_TEST_SYSTEMD_AVAILABLE=false \
  drs="touch $DRS_INJECTION_MARKER" \
  "$DIRECT_LAUNCHER" "$DIRECT_TARGET" nested

if [[ $(wc -l < "$PROFILE_TRACE") -ne 2 ]]; then
  echo "nested launch unexpectedly loaded the login profile"
  exit 1
fi
if [[ $(< "$NESTED_ENV") != missing ]]; then
  echo "nested launch inherited the native login profile marker"
  exit 1
fi
if [[ $(< "$NESTED_ARGUMENTS") != nested ]]; then
  echo "nested launcher arguments changed"
  exit 1
fi
if ! grep -Fxq 'WAYLAND_DISPLAY=wayland-test' "$NESTED_SESSION_ENV"; then
  echo "nested launch lost its parent Wayland display"
  exit 1
fi

env -i \
  HOME="$TEST_HOME" \
  SHELL="$login_shell" \
  USER="${USER:-umbriel-test}" \
  LOGNAME="${LOGNAME:-umbriel-test}" \
  PATH="$TEST_BIN:$PATH" \
  DBUS_SESSION_BUS_ADDRESS="unix:path=$TEST_DIR/nonexistent-bus" \
  MANAGERPID=1 \
  UMBRIEL_TEST_PROFILE_TRACE="$PROFILE_TRACE" \
  UMBRIEL_TEST_DIRECT_ENV="$EXTERNAL_ENV" \
  UMBRIEL_TEST_DIRECT_ARGUMENTS="$EXTERNAL_ARGUMENTS" \
  UMBRIEL_TEST_SESSION_ENV="$EXTERNAL_SESSION_ENV" \
  UMBRIEL_TEST_BIN="$TEST_BIN" \
  UMBRIEL_TEST_HOST_PATH="$PATH" \
  UMBRIEL_TEST_SYSTEMD_AVAILABLE=false \
  drs="touch $DRS_INJECTION_MARKER" \
  /bin/sh -c 'SYSTEMD_EXEC_PID=$$; export SYSTEMD_EXEC_PID; exec "$@"' \
    sh "$DIRECT_LAUNCHER" "$DIRECT_TARGET" external

if [[ $(wc -l < "$PROFILE_TRACE") -ne 2 ]]; then
  echo "externally managed launch unexpectedly loaded the login profile"
  exit 1
fi
if [[ $(< "$EXTERNAL_ENV") != missing || $(< "$EXTERNAL_ARGUMENTS") != external ]]; then
  echo "externally managed launch did not execute directly"
  exit 1
fi
if [[ -e $DRS_INJECTION_MARKER ]]; then
  echo "launcher used an inherited internal command variable"
  exit 1
fi

echo "native session launcher imports the login profile environment"
