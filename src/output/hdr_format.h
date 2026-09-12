#pragma once

#include <array>
#include <cstdint>
#include <drm_fourcc.h>
#include <optional>
#include <utility>

namespace umbriel {

  [[nodiscard]] inline std::array<uint32_t, 2> hdrRenderFormatCandidates(uint32_t currentFormat) {
    std::array candidates{DRM_FORMAT_XRGB2101010, DRM_FORMAT_XBGR2101010};
    if (currentFormat == DRM_FORMAT_XBGR2101010) {
      std::swap(candidates[0], candidates[1]);
    }
    return candidates;
  }

  template <typename Probe>
  [[nodiscard]] std::optional<uint32_t> selectHdrRenderFormat(uint32_t currentFormat, Probe&& accepts) {
    for (const uint32_t candidate : hdrRenderFormatCandidates(currentFormat)) {
      if (accepts(candidate)) {
        return candidate;
      }
    }
    return std::nullopt;
  }

} // namespace umbriel
