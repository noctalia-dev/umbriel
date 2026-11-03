#include "core/fdlimit.h"

#include "core/log.h"

#include <cerrno>
#include <cstring>
#include <string>
#include <sys/resource.h>

namespace {
  constexpr Logger kLog("fdlimit");

  rlimit gOriginalLimit{};
  bool gRaised = false;

  std::string rlimitValue(rlim_t value) {
    if (value == RLIM_INFINITY) {
      return "infinity";
    }
    return std::to_string(static_cast<unsigned long long>(value));
  }

} // namespace

void raiseFileDescriptorLimit() {
  if (gRaised) {
    return;
  }

  rlimit limit{};
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
    kLog.warn("getrlimit(RLIMIT_NOFILE) failed: {}", std::strerror(errno));
    return;
  }

  gOriginalLimit = limit;
  if (limit.rlim_cur >= limit.rlim_max) {
    gRaised = true; // Nothing to raise, but children still inherit the right value.
    kLog.info("RLIMIT_NOFILE already at hard limit ({})", rlimitValue(limit.rlim_cur));
    return;
  }

  const rlim_t previous = limit.rlim_cur;
  limit.rlim_cur = limit.rlim_max;
  if (setrlimit(RLIMIT_NOFILE, &limit) != 0) {
    kLog.warn("failed to raise RLIMIT_NOFILE from {}: {}", rlimitValue(previous), std::strerror(errno));
    return;
  }

  gRaised = true;
  kLog.info("raised RLIMIT_NOFILE {} -> {}", rlimitValue(previous), rlimitValue(limit.rlim_max));
}

void restoreFileDescriptorLimit() {
  if (!gRaised) {
    return;
  }
  // Runs between fork() and exec(): must stay async-signal-safe, so no logging
  // on failure: the child is about to be replaced anyway.
  (void)setrlimit(RLIMIT_NOFILE, &gOriginalLimit);
}
