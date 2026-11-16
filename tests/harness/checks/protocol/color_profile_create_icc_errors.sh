#!/usr/bin/env bash
# The two request-contract errors of set_icc_file: a file over the size limit is bad_size and an fd that is not a
# seekable regular file is bad_fd. Both kill exactly the offending client and nothing compositor-side.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

write_icc_matrix_profile "$PADDED" 0 2.2 > /dev/null

# bad_size is a request-contract protocol error: it kills exactly this client invocation (exit 2 after observing it).
rc=$(color_run icc-oversize-over icc-oversize "$PADDED")
if [[ $rc != 2 ]] || ! grep -Fq '{"event":"icc_bad_size"' "$RUNTIME/icc-oversize-over.log"; then
  echo "oversized set_icc_file did not raise bad_size for the client: rc=$rc $(cat "$RUNTIME/icc-oversize-over.log")"
  exit 1
fi
if ! "$UMBRIEL" color --json | jq -e '.color_manager == true' > /dev/null; then
  echo "the compositor did not survive the bad_size client: $("$UMBRIEL" color --json)"
  exit 1
fi

# A directory fd must be refused at set_icc_file instead of surfacing later as a read failure.
rc=$(color_run icc-nonregular-nonreg icc-nonregular "$PADDED")
if [[ $rc != 2 ]] || ! grep -Fq '{"event":"icc_bad_fd"' "$RUNTIME/icc-nonregular-nonreg.log"; then
  echo "directory fd did not raise bad_fd for the client: rc=$rc $(cat "$RUNTIME/icc-nonregular-nonreg.log")"
  exit 1
fi
if ! "$UMBRIEL" color --json | jq -e '.color_manager == true' > /dev/null; then
  echo "the compositor did not survive the bad_fd client: $("$UMBRIEL" color --json)"
  exit 1
fi

echo "set_icc_file raises bad_size for an oversized file and bad_fd for a non-regular fd, killing only the client"
