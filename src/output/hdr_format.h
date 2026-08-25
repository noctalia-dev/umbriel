#pragma once

#include <array>
#include <cstdint>
#include <drm_fourcc.h>
#include <optional>
#include <utility>

namespace umbriel {

  template <typename Probe>
  [[nodiscard]] std::optional<uint32_t> selectHdrRenderFormat(uint32_t currentFormat, Probe&& accepts) {
    std::array candidates{
        DRM_FORMAT_XRGB2101010,
        DRM_FORMAT_XBGR2101010,
    };
    if (currentFormat == DRM_FORMAT_XBGR2101010) {
      std::swap(candidates[0], candidates[1]);
    }
    for (const uint32_t candidate : candidates) {
      if (accepts(candidate)) {
        return candidate;
      }
    }
    return std::nullopt;
  }

} // namespace umbriel
