#!/usr/bin/env bash
# Every action `umbriel msg --help` advertises is parseable and accepted by `msg`. This is the regression net for the action registry. The action list is spread across the KeybindAction enum, the kActionSpecs table, and the dispatch switch, so any change that consolidates or reshapes them can silently drop an action without the compiler noticing.
set -euo pipefail

# Actions deliberately not exercised: they act on the harness itself rather than
# on compositor state.
skip_action() {
  case $1 in
    session-quit) return 0 ;;  # opens the modal confirm dialog, and confirming it ends the instance mid-sweep; covered by 030_session_quit
    spawn)        return 0 ;;  # would start a process outside the container
    window-focus|window-focus-warp) return 0 ;;  # needs a live window id; covered by focused IPC checks
    # These require a second output; single-output rejection paths are covered by 610_output_actions.
    output-focus-*)             return 0 ;;
    window-focus-or-output-*)   return 0 ;;
    window-move-or-output-*)    return 0 ;;
    window-move-to-output-*)    return 0 ;;
    column-move-to-output-*)    return 0 ;;
    workspace-move-to-output-*) return 0 ;;
    *) return 1 ;;
  esac
}

# Sample argument per parameterized action, keyed by the spec's param text.
sample_arg() {
  case $1 in
    '<cmd>')                     echo 'true' ;;
    '<fraction>')                echo '0.5' ;;
    '<delta>')                   echo '0.1' ;;
    '<workspace>[/<output>]')    echo '1' ;;
    '<name>')                    echo 'harness' ;;
    '<scrolling|dwindle|toggle>') echo 'scrolling' ;; # harness default: exercising it is a no-op
    '[<output>]')                echo '' ;;
    '[<window-id>]')             echo '' ;;
    *)                           echo '' ;;
  esac
}

failures=0
count=0
while read -r spec; do
  [[ -z $spec ]] && continue
  name=${spec%%:*}
  param=''
  [[ $spec == *:* ]] && param=${spec#*:}

  skip_action "$name" && continue

  arg=$(sample_arg "$param")
  action=$name
  [[ -n $arg ]] && action="$name:$arg"

  count=$((count + 1))
  if ! out=$("$UMBRIEL" msg "$action" 2>&1); then
    echo "rejected: $action -> $out"
    failures=$((failures + 1))
  fi
done < <("$UMBRIEL" msg --help | sed -n 's/^  //p')

if [[ $count -eq 0 ]]; then
  echo "no actions were exercised"
  exit 1
fi

if [[ $failures -gt 0 ]]; then
  echo "$failures of $count actions failed"
  exit 1
fi
echo "$count actions accepted"
