#!/usr/bin/env bash
# get_information must not accumulate the icc_file memfd: a client that asks 64 times leaves the compositor's fd table
# where it started.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

readonly STORM=64
color_fixture
# "srgb" stages the gamut transform, which is what carries the characterization's profile onto the advertised
# descriptions — without it the storm has no fd to leak.
color_config "color_profile = \"$PROFILE\"
sdr_gamut = \"srgb\""

server_pid=$(color_pid)
if [[ -z $server_pid ]]; then
  echo "the compositor process could not be found for fd accounting"
  exit 1
fi
fd_before=$(color_fd_count "$server_pid")
memfd_before=$(color_memfd_count "$server_pid")
if ! "$CLIENT" icc-storm "$STORM" > "$RUNTIME/icc-storm.log" 2>&1; then
  echo "the get_information storm did not complete: $(cat "$RUNTIME/icc-storm.log")"
  exit 1
fi
"$UMBRIEL" settle > /dev/null
fd_after=$(color_fd_count "$server_pid")
memfd_after=$(color_memfd_count "$server_pid")
if ((memfd_after > memfd_before || fd_after > fd_before + 2)); then
  echo "$STORM get_information requests leaked compositor fds:" \
    "icc memfds $memfd_before -> $memfd_after, all fds $fd_before -> $fd_after"
  exit 1
fi

echo "$STORM get_information requests left the compositor's fd table unchanged"
