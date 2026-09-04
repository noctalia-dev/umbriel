#!/usr/bin/env bash
# A version 1 client can set the regular clipboard through the legacy
# data-control protocol, and the selection reaches the modern replacement.
set -euo pipefail

readonly DATA_CONTROL_CLIENT="${UMBRIEL_DATA_CONTROL_CLIENT:-./build-debug/tests/data-control-client}"

if [[ ! -x $DATA_CONTROL_CLIENT ]]; then
  echo "data-control client not built at $DATA_CONTROL_CLIENT"
  exit 1
fi

"$DATA_CONTROL_CLIENT"
