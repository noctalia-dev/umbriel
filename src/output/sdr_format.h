#pragma once

#include <cstdint>
#include <drm_fourcc.h>
#include <optional>

namespace umbriel {

  template <typename Probe> [[nodiscard]] std::optional<uint32_t> selectSdr10RenderFormat(Probe&& accepts) {
    for (const uint32_t candidate : {DRM_FORMAT_XRGB2101010, DRM_FORMAT_XBGR2101010}) {
      if (accepts(candidate)) {
        return candidate;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] inline bool deriveTenBitSdrActive(bool enabled, bool hdrActive, uint32_t renderFormat) {
    return enabled && !hdrActive && (renderFormat == DRM_FORMAT_XRGB2101010 || renderFormat == DRM_FORMAT_XBGR2101010);
  }

} // namespace umbriel
