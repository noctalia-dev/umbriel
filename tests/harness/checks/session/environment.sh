#!/usr/bin/env bash
# Configured values reach a native systemd user manager before the session target. The same values stay local when
# systemd is unavailable or the compositor is nested, while D-Bus receives only the graphical connection variables.
set -euo pipefail

HARNESS_PATH=$PATH
HARNESS_SLEEP=$(command -v sleep)
WORK_ROOT=$(mktemp -d /tmp/ume.XXXXXXXX)
PRIVATE_PID=

cleanup() {
  if [[ -n $PRIVATE_PID ]] && kill -0 "$PRIVATE_PID" 2>/dev/null; then
    kill -KILL "$PRIVATE_PID" 2>/dev/null || true
    wait "$PRIVATE_PID" 2>/dev/null || true
  fi
  rm -rf "$WORK_ROOT"
}
trap cleanup EXIT

write_fixture() {
  local runtime=$1
  local xwayland=$2
  mkdir -p "$runtime/bin" "$runtime/manager" "$runtime/trace"

  cat > "$runtime/config.toml" << EOF
[general]
xwayland = $xwayland
show_cheatsheet = false
autostart = ["$runtime/bin/capture-environment"]

[environment]
UMBRIEL_TEST_ONE = "alpha beta"
UMBRIEL_TEST_TWO = "literal;\$HOME's"
PATH = "$runtime/hostile-bin"
XDG_RUNTIME_DIR = "$runtime/hostile-runtime"
DBUS_SESSION_BUS_ADDRESS = "unix:path=$runtime/hostile-dbus"
SYSTEMD_BUS_ADDRESS = "unix:path=$runtime/hostile-systemd"
EOF

  cat > "$runtime/bin/xwayland-satellite" << 'EOF'
#!/bin/sh
[ "${DISPLAY+x}" != x ] || exit 1
printf '%s\n' "$UMBRIEL_TEST_ONE" "$UMBRIEL_TEST_TWO" "$PATH" "${DISPLAY-unset}" "$1" \
  > "$FIXTURE_ROOT/xwayland-environment"
exec "$SLEEP_BIN" 120
EOF

  cat > "$runtime/bin/capture-environment" << 'EOF'
#!/bin/sh
printf '%s\n' \
  "$UMBRIEL_TEST_ONE" \
  "$UMBRIEL_TEST_TWO" \
  "$PATH" \
  "$XDG_RUNTIME_DIR" \
  "$DBUS_SESSION_BUS_ADDRESS" \
  "$SYSTEMD_BUS_ADDRESS" > "$FIXTURE_ROOT/autostart-environment"
EOF

  cat > "$runtime/bin/capture-reloaded-environment" << 'EOF'
#!/bin/sh
printf '%s' "$UMBRIEL_TEST_ONE" > "$FIXTURE_ROOT/reloaded-environment"
EOF

  cat > "$runtime/bin/systemctl" << 'EOF'
#!/bin/sh
set -eu

display_is_expected() {
  if [ "$EXPECT_DISPLAY" = true ]; then
    case "$DISPLAY" in :[0-9] | :[12][0-9] | :3[01]) return 0 ;; *) return 1 ;; esac
  fi
  [ "${DISPLAY+x}" != x ]
}

control_environment_is_original() {
  [ "$PATH" = "$EXPECTED_CONTROL_PATH" ] &&
    [ "$XDG_RUNTIME_DIR" = "$FIXTURE_ROOT" ] &&
    [ "$DBUS_SESSION_BUS_ADDRESS" = "unix:path=$FIXTURE_ROOT/control-dbus" ] &&
    [ "$SYSTEMD_BUS_ADDRESS" = "unix:path=$FIXTURE_ROOT/control-systemd" ] &&
    [ "${UMBRIEL_TEST_ONE+x}" != x ] && [ "${UMBRIEL_TEST_TWO+x}" != x ] &&
    [ "$WAYLAND_DISPLAY" = wayland-0 ] && display_is_expected &&
    [ "$XDG_CURRENT_DESKTOP" = umbriel ] && [ "$XDG_SESSION_DESKTOP" = umbriel ] &&
    [ "$XDG_SESSION_TYPE" = wayland ] &&
    [ "$UMBRIEL_SOCKET" = "$FIXTURE_ROOT/umbriel-$WAYLAND_DISPLAY.sock" ]
}

fail() {
  : > "$TRACE_DIR/unexpected-systemctl"
  exit 1
}

control_environment_is_original || fail
[ "${1:-}" = "--user" ] || fail
shift
action=${1:-}
[ "$#" -gt 0 ] && shift

if [ "${SYSTEMCTL_UNAVAILABLE:-false}" = true ]; then
  : > "$TRACE_DIR/systemctl-probed"
  [ "$action" = show-environment ] && exit 1
  fail
fi

record_configured() {
  case "$1" in
    'UMBRIEL_TEST_ONE=alpha beta') file=one ;;
    "UMBRIEL_TEST_TWO=literal;\$HOME's") file=two ;;
    "PATH=$FIXTURE_ROOT/hostile-bin") file=path ;;
    "XDG_RUNTIME_DIR=$FIXTURE_ROOT/hostile-runtime") file=runtime ;;
    "DBUS_SESSION_BUS_ADDRESS=unix:path=$FIXTURE_ROOT/hostile-dbus") file=dbus ;;
    "SYSTEMD_BUS_ADDRESS=unix:path=$FIXTURE_ROOT/hostile-systemd") file=systemd ;;
    *) return 1 ;;
  esac
  printf '%s' "${1#*=}" > "$MANAGER_DIR/$file"
}

case "$action" in
  show-environment)
    [ "$#" -eq 0 ] || fail
    ;;
  set-environment)
    [ "$#" -eq 6 ] || fail
    for assignment in "$@"; do
      record_configured "$assignment" || fail
    done
    for file in one two path runtime dbus systemd; do
      [ -f "$MANAGER_DIR/$file" ] || fail
    done
    printf '%s\n' systemd-configured >> "$TRACE_DIR/order"
    ;;
  import-environment)
    [ "$*" = "WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_DESKTOP XDG_SESSION_TYPE UMBRIEL_SOCKET" ] ||
      fail
    printf '%s\n' systemd-graphical >> "$TRACE_DIR/order"
    printf '%s' "${DISPLAY-}" > "$MANAGER_DIR/display"
    : > "$TRACE_DIR/systemd-graphical"
    ;;
  start)
    [ "$*" = "--no-block umbriel-session.target" ] || fail
    [ -e "$TRACE_DIR/systemd-graphical" ] || fail
    [ "$(cat "$MANAGER_DIR/one")" = "alpha beta" ] || fail
    [ "$(cat "$MANAGER_DIR/two")" = "literal;\$HOME's" ] || fail
    [ "$(cat "$MANAGER_DIR/path")" = "$FIXTURE_ROOT/hostile-bin" ] || fail
    [ "$(cat "$MANAGER_DIR/runtime")" = "$FIXTURE_ROOT/hostile-runtime" ] || fail
    [ "$(cat "$MANAGER_DIR/dbus")" = "unix:path=$FIXTURE_ROOT/hostile-dbus" ] || fail
    [ "$(cat "$MANAGER_DIR/systemd")" = "unix:path=$FIXTURE_ROOT/hostile-systemd" ] || fail
    printf '%s\n' target >> "$TRACE_DIR/order"
    : > "$TRACE_DIR/target-inherited"
    ;;
  *) fail ;;
esac
EOF

  cat > "$runtime/bin/systemd-run" << 'EOF'
#!/bin/sh
: > "$TRACE_DIR/unexpected-systemd-run"
exit 1
EOF

  cat > "$runtime/bin/dbus-update-activation-environment" << 'EOF'
#!/bin/sh
set -eu

[ "$PATH" = "$EXPECTED_CONTROL_PATH" ] || exit 1
[ "$XDG_RUNTIME_DIR" = "$FIXTURE_ROOT" ] || exit 1
[ "$DBUS_SESSION_BUS_ADDRESS" = "unix:path=$FIXTURE_ROOT/control-dbus" ] || exit 1
[ "$SYSTEMD_BUS_ADDRESS" = "unix:path=$FIXTURE_ROOT/control-systemd" ] || exit 1
[ "${UMBRIEL_TEST_ONE+x}" != x ] && [ "${UMBRIEL_TEST_TWO+x}" != x ] || exit 1
[ "$WAYLAND_DISPLAY" = wayland-0 ] || exit 1
if [ "$EXPECT_DISPLAY" = true ]; then
  case "$DISPLAY" in :[0-9] | :[12][0-9] | :3[01]) ;; *) exit 1 ;; esac
else
  [ "${DISPLAY+x}" != x ] || exit 1
fi
[ "$UMBRIEL_SOCKET" = "$FIXTURE_ROOT/umbriel-$WAYLAND_DISPLAY.sock" ] || exit 1
[ "$*" = "WAYLAND_DISPLAY DISPLAY XDG_CURRENT_DESKTOP XDG_SESSION_DESKTOP XDG_SESSION_TYPE UMBRIEL_SOCKET" ] ||
  exit 1

printf '%s\n' dbus-graphical >> "$TRACE_DIR/order"
: > "$TRACE_DIR/dbus-graphical"
EOF

  chmod +x "$runtime/bin/capture-environment" "$runtime/bin/capture-reloaded-environment" \
    "$runtime/bin/xwayland-satellite" "$runtime/bin/systemctl" "$runtime/bin/systemd-run" \
    "$runtime/bin/dbus-update-activation-environment"
}

start_private() {
  local runtime=$1
  local nested=$2
  local unavailable=$3
  local expect_display=$4
  local control_path=$runtime/bin:$HARNESS_PATH
  local -a environment=(
    "PATH=$control_path"
    "XDG_RUNTIME_DIR=$runtime"
    "DBUS_SESSION_BUS_ADDRESS=unix:path=$runtime/control-dbus"
    "SYSTEMD_BUS_ADDRESS=unix:path=$runtime/control-systemd"
    "EXPECTED_CONTROL_PATH=$control_path"
    "FIXTURE_ROOT=$runtime"
    "TRACE_DIR=$runtime/trace"
    "MANAGER_DIR=$runtime/manager"
    "SYSTEMCTL_UNAVAILABLE=$unavailable"
    "EXPECT_DISPLAY=$expect_display"
    "SLEEP_BIN=$HARNESS_SLEEP"
    "WLR_BACKENDS=headless"
    "WLR_LIBINPUT_NO_DEVICES=1"
    "WLR_HEADLESS_OUTPUTS=1"
  )
  local -a unset=(
    -u WAYLAND_SOCKET -u DISPLAY -u UMBRIEL_SOCKET
    -u XDG_CURRENT_DESKTOP -u XDG_SESSION_DESKTOP -u XDG_SESSION_TYPE
    -u UMBRIEL_TEST_ONE -u UMBRIEL_TEST_TWO -u SYSTEMD_EXEC_PID
  )

  if [[ $nested == true ]]; then
    env "${unset[@]}" WAYLAND_DISPLAY=host-wayland "${environment[@]}" \
      "$UMBRIEL" -c "$runtime/config.toml" > "$runtime/compositor.log" 2>&1 &
  else
    env "${unset[@]}" -u WAYLAND_DISPLAY "${environment[@]}" \
      "$UMBRIEL" -c "$runtime/config.toml" > "$runtime/compositor.log" 2>&1 &
  fi
  PRIVATE_PID=$!
}

wait_for_file() {
  local runtime=$1
  local expected=$2
  local require_content=${3:-false}
  for _ in $(seq 100); do
    if [[ $require_content == true ]]; then
      [[ -s $expected ]] && return
    else
      [[ -e $expected ]] && return
    fi
    if ! kill -0 "$PRIVATE_PID" 2>/dev/null; then
      echo "the private compositor died during environment synchronization"
      sed 's/^/  | /' "$runtime/compositor.log"
      exit 1
    fi
    sleep 0.05
  done
  echo "timed out waiting for $expected"
  sed 's/^/  | /' "$runtime/compositor.log"
  exit 1
}

stop_private() {
  local runtime=$1
  kill -TERM "$PRIVATE_PID"
  local status=0
  wait "$PRIVATE_PID" || status=$?
  PRIVATE_PID=
  if [[ $status -ne 0 ]]; then
    echo "the private compositor exited with status $status, expected 0"
    sed 's/^/  | /' "$runtime/compositor.log"
    exit 1
  fi
}

assert_autostart_environment() {
  local runtime=$1
  mapfile -t values < "$runtime/autostart-environment"
  [[ ${values[0]} == "alpha beta" ]]
  [[ ${values[1]} == "literal;\$HOME's" ]]
  [[ ${values[2]} == "$runtime/hostile-bin" ]]
  [[ ${values[3]} == "$runtime/hostile-runtime" ]]
  [[ ${values[4]} == "unix:path=$runtime/hostile-dbus" ]]
  [[ ${values[5]} == "unix:path=$runtime/hostile-systemd" ]]
}

assert_xwayland_environment() {
  local runtime=$1
  mapfile -t values < "$runtime/xwayland-environment"
  [[ ${values[0]} == "alpha beta" ]]
  [[ ${values[1]} == "literal;\$HOME's" ]]
  [[ ${values[2]} == "$runtime/hostile-bin" ]]
  [[ ${values[3]} == unset ]]
  [[ ${values[4]} == "$(< "$runtime/manager/display")" ]]
}

NATIVE=$WORK_ROOT/native
write_fixture "$NATIVE" true
start_private "$NATIVE" false false true
wait_for_file "$NATIVE" "$NATIVE/trace/target-inherited"
wait_for_file "$NATIVE" "$NATIVE/trace/dbus-graphical"
wait_for_file "$NATIVE" "$NATIVE/autostart-environment" true
wait_for_file "$NATIVE" "$NATIVE/xwayland-environment" true
assert_autostart_environment "$NATIVE"
assert_xwayland_environment "$NATIVE"
[[ $(< "$NATIVE/trace/order") == $'systemd-graphical\nsystemd-configured\ndbus-graphical\ntarget' ]]
grep -Fq "spawned 'session environment synchronization'" "$NATIVE/compositor.log"
! grep -Fq 'systemctl --user set-environment' "$NATIVE/compositor.log"

sed -i 's/UMBRIEL_TEST_ONE = "alpha beta"/UMBRIEL_TEST_ONE = "reloaded"/' "$NATIVE/config.toml"
env UMBRIEL_SOCKET="$NATIVE/umbriel-wayland-0.sock" "$UMBRIEL" msg config-reload > /dev/null
env UMBRIEL_SOCKET="$NATIVE/umbriel-wayland-0.sock" \
  "$UMBRIEL" msg "spawn:$NATIVE/bin/capture-reloaded-environment" > /dev/null
wait_for_file "$NATIVE" "$NATIVE/reloaded-environment" true
reloaded_value=$(< "$NATIVE/reloaded-environment")
if [[ $reloaded_value != "alpha beta" ]]; then
  echo "an environment reload changed newly spawned processes before restart: $reloaded_value"
  exit 1
fi
[[ $(< "$NATIVE/manager/one") == "alpha beta" ]]
[[ $(< "$NATIVE/trace/order") == $'systemd-graphical\nsystemd-configured\ndbus-graphical\ntarget' ]]
stop_private "$NATIVE"
[[ ! -e $NATIVE/trace/unexpected-systemctl ]]
[[ ! -e $NATIVE/trace/unexpected-systemd-run ]]
[[ $(< "$NATIVE/manager/one") == "alpha beta" ]]

NO_SYSTEMD=$WORK_ROOT/no-systemd
write_fixture "$NO_SYSTEMD" false
start_private "$NO_SYSTEMD" false true false
wait_for_file "$NO_SYSTEMD" "$NO_SYSTEMD/trace/dbus-graphical"
wait_for_file "$NO_SYSTEMD" "$NO_SYSTEMD/autostart-environment" true
assert_autostart_environment "$NO_SYSTEMD"
[[ -e $NO_SYSTEMD/trace/systemctl-probed ]]
[[ ! -e $NO_SYSTEMD/trace/unexpected-systemctl ]]
[[ ! -e $NO_SYSTEMD/trace/unexpected-systemd-run ]]
[[ ! -e $NO_SYSTEMD/trace/target-inherited ]]
[[ $(< "$NO_SYSTEMD/trace/order") == dbus-graphical ]]
stop_private "$NO_SYSTEMD"

NESTED=$WORK_ROOT/nested
write_fixture "$NESTED" false
start_private "$NESTED" true false false
wait_for_file "$NESTED" "$NESTED/autostart-environment" true
assert_autostart_environment "$NESTED"
[[ ! -e $NESTED/trace/order ]]
stop_private "$NESTED"
[[ ! -e $NESTED/trace/unexpected-systemd-run ]]

echo "configured environment reached systemd before target startup and remained startup-only"
