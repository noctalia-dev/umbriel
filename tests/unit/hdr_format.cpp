#include "output/hdr_format.h"

#include "check.h"

#include <optional>
#include <vector>

using umbriel::selectHdrRenderFormat;

UMBRIEL_TEST(rejectedXrgbFallsBackToXbgr) {
  std::vector<uint32_t> probed;
  const auto selected = selectHdrRenderFormat(DRM_FORMAT_XRGB8888, [&](uint32_t format) {
    probed.push_back(format);
    return format == DRM_FORMAT_XBGR2101010;
  });

  CHECK_EQ(selected, std::optional<uint32_t>{DRM_FORMAT_XBGR2101010});
  CHECK_EQ(probed.size(), size_t{2});
  CHECK_EQ(probed[0], uint32_t{DRM_FORMAT_XRGB2101010});
  CHECK_EQ(probed[1], uint32_t{DRM_FORMAT_XBGR2101010});
}

UMBRIEL_TEST(acceptedXrgbShortCircuitsSelection) {
  std::vector<uint32_t> probed;
  const auto selected = selectHdrRenderFormat(DRM_FORMAT_XRGB8888, [&](uint32_t format) {
    probed.push_back(format);
    return true;
  });

  CHECK_EQ(selected, std::optional<uint32_t>{DRM_FORMAT_XRGB2101010});
  CHECK_EQ(probed.size(), size_t{1});
}

UMBRIEL_TEST(currentXbgrIsRetriedFirst) {
  std::vector<uint32_t> probed;
  const auto selected = selectHdrRenderFormat(DRM_FORMAT_XBGR2101010, [&](uint32_t format) {
    probed.push_back(format);
    return true;
  });

  CHECK_EQ(selected, std::optional<uint32_t>{DRM_FORMAT_XBGR2101010});
  CHECK_EQ(probed.size(), size_t{1});
}

UMBRIEL_TEST(rejectedTenBitFormatsReturnNothing) {
  std::vector<uint32_t> probed;
  const auto selected = selectHdrRenderFormat(DRM_FORMAT_XRGB8888, [&](uint32_t format) {
    probed.push_back(format);
    return false;
  });

  CHECK_EQ(selected, std::optional<uint32_t>{});
  CHECK_EQ(probed.size(), size_t{2});
}

int main() { return RUN_TESTS(); }
