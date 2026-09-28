#pragma once

#include <chrono>
#include <cstdint>

namespace umbriel {

  // Milliseconds on the monotonic clock libinput stamps input events with, for work that has no event to read a
  // timestamp from.
  inline uint32_t monotonicMsec() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
  }

} // namespace umbriel
