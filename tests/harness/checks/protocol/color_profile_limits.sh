#!/usr/bin/env bash
# Resource bounds on client-driven color work: get_information must not
# accumulate the icc_file memfd, and one client must not stage or retain more
# ICC bytes — or concurrent creators — than the per-client budget. Each phase
# drives the amplification request pattern a hostile client would loop and
# asserts the compositor stays bounded and alive.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly CLIENT="${UMBRIEL_COLOR_FEEDBACK_CLIENT:?color feedback client path is required}"
readonly RUNTIME="$UMBRIEL_RUNTIME_DIR"
readonly BASELINE=$(< "$UMBRIEL_CONFIG")
readonly PROFILE="$RUNTIME/color-profile.icc"
readonly PADDED="$RUNTIME/color-profile-padded.icc"
readonly SPARSE="$RUNTIME/icc-sparse.bin"
readonly STORM=64

write_icc_matrix_profile "$PROFILE" > /dev/null
write_icc_matrix_profile "$PADDED" 12 > /dev/null
truncate -s $((32 * 1024 * 1024)) "$SPARSE"

printf '%s\n\n[animation]\nenabled = false\n[output.HEADLESS-1]\ncolor_profile = "%s"\n' "$BASELINE" "$PROFILE" \
  > "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle > /dev/null

compositor_pid() {
  pgrep -f -- "-c $UMBRIEL_CONFIG" | head -1
}

compositor_fd_count() {
  ls /proc/"$1"/fd 2>/dev/null | wc -l
}

compositor_icc_memfd_count() {
  ls -l /proc/"$1"/fd 2>/dev/null | grep -c 'memfd:umbriel-icc-profile' || true
}

# --- get_information must not leak the advertised fd -------------------------
server_pid=$(compositor_pid)
if [[ -z $server_pid ]]; then
  echo "the compositor process could not be found for fd accounting"
  exit 1
fi
fd_before=$(compositor_fd_count "$server_pid")
memfd_before=$(compositor_icc_memfd_count "$server_pid")
if ! "$CLIENT" icc-storm "$STORM" > "$RUNTIME/icc-storm.log" 2>&1; then
  echo "the get_information storm did not complete: $(cat "$RUNTIME/icc-storm.log")"
  exit 1
fi
"$UMBRIEL" settle > /dev/null
fd_after=$(compositor_fd_count "$server_pid")
memfd_after=$(compositor_icc_memfd_count "$server_pid")
if ((memfd_after > memfd_before || fd_after > fd_before + 2)); then
  echo "$STORM get_information requests leaked compositor fds: icc memfds $memfd_before -> $memfd_after, all fds $fd_before -> $fd_after"
  exit 1
fi

# --- per-client creator budget ----------------------------------------------
run_icc_mode() {
  local log="$RUNTIME/icc-$1.log"
  shift
  set +e
  "$CLIENT" "$@" > "$log" 2>&1
  local rc=$?
  set -e
  echo "$rc"
}

rc=$(run_icc_mode cap-creators icc-cap-creators)
if [[ $rc != 2 ]] || ! grep -Fq '{"event":"icc_cap_creators"' "$RUNTIME/icc-cap-creators.log"; then
  echo "the creator budget did not refuse the fifth concurrent creator: rc=$rc $(cat "$RUNTIME/icc-cap-creators.log")"
  exit 1
fi

# --- per-client byte budget: staged bytes -----------------------------------
rc=$(run_icc_mode cap-bytes icc-cap-bytes "$SPARSE")
if [[ $rc != 2 ]] || ! grep -Fq '{"event":"icc_cap_bytes"' "$RUNTIME/icc-cap-bytes.log"; then
  echo "the byte budget did not refuse the overflowing set_icc_file: rc=$rc $(cat "$RUNTIME/icc-cap-bytes.log")"
  exit 1
fi

# --- per-client byte budget: bytes retained on created descriptions ---------
rc=$(run_icc_mode cap-retained icc-cap-retained "$PADDED")
if [[ $rc != 2 ]] \
    || ! grep -Fq '{"event":"icc_cap_retained","code":3,"iteration":2}' "$RUNTIME/icc-cap-retained.log"; then
  echo "the byte budget did not refuse the third retained description: rc=$rc $(cat "$RUNTIME/icc-cap-retained.log")"
  exit 1
fi

if ! "$UMBRIEL" color --json | jq -e '.color_manager == true' > /dev/null; then
  echo "the compositor did not survive the budget clients: $( "$UMBRIEL" color --json)"
  exit 1
fi

echo "icc_file fd stability, the creator budget and the staged/retained byte budgets verified live"
