#!/usr/bin/env bash
# The per-client byte budget counts staged bytes: a set_icc_file that would push the client past the limit is refused
# with bad_size, which disconnects exactly that client.
set -euo pipefail
source "$UMBRIEL_REPO/tests/harness/color_lib.sh"

truncate -s $((32 * 1024 * 1024)) "$SPARSE"

rc=$(color_run icc-cap-bytes icc-cap-bytes "$SPARSE")
if [[ $rc != 2 ]] || ! grep -Fq '{"event":"icc_cap_bytes"' "$RUNTIME/icc-cap-bytes.log"; then
  echo "the byte budget did not refuse the overflowing set_icc_file: rc=$rc $(cat "$RUNTIME/icc-cap-bytes.log")"
  exit 1
fi

echo "a staged profile that would exceed the per-client byte budget is refused with bad_size"
