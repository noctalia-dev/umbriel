#include "output/sdr_format.h"

#include "check.h"

#include <optional>
#include <vector>

using umbriel::deriveTenBitSdrActive;
using umbriel::selectSdr10RenderFormat;

UMBRIEL_TEST(sdr10XR30AcceptedShortCircuits) {
  std::vector<uint32_t> probed;
  const auto selected = selectSdr10RenderFormat([&](uint32_t format) {
    probed.push_back(format);
    return true;
  });

  CHECK_EQ(selected, std::optional<uint32_t>{DRM_FORMAT_XRGB2101010});
  CHECK_EQ(probed.size(), size_t{1});
  CHECK_EQ(probed[0], uint32_t{DRM_FORMAT_XRGB2101010});
}

UMBRIEL_TEST(sdr10XR30RejectedFallsBackToXB30) {
  std::vector<uint32_t> probed;
  const auto selected = selectSdr10RenderFormat([&](uint32_t format) {
    probed.push_back(format);
    return format == DRM_FORMAT_XBGR2101010;
  });

  CHECK_EQ(selected, std::optional<uint32_t>{DRM_FORMAT_XBGR2101010});
  CHECK_EQ(probed.size(), size_t{2});
  CHECK_EQ(probed[0], uint32_t{DRM_FORMAT_XRGB2101010});
  CHECK_EQ(probed[1], uint32_t{DRM_FORMAT_XBGR2101010});
}

UMBRIEL_TEST(sdr10BothRejectedReturnsNothing) {
  std::vector<uint32_t> probed;
  const auto selected = selectSdr10RenderFormat([&](uint32_t format) {
    probed.push_back(format);
    return false;
  });

  CHECK_EQ(selected, std::optional<uint32_t>{});
  CHECK_EQ(probed.size(), size_t{2});
  CHECK_EQ(probed[0], uint32_t{DRM_FORMAT_XRGB2101010});
  CHECK_EQ(probed[1], uint32_t{DRM_FORMAT_XBGR2101010});
}

// XR30 (DRM_FORMAT_XRGB2101010) on an enabled, non-HDR output is active SDR10.
UMBRIEL_TEST(deriveEnabledXr30IsActive) { CHECK(deriveTenBitSdrActive(true, false, DRM_FORMAT_XRGB2101010)); }

// XB30 (DRM_FORMAT_XBGR2101010) on an enabled, non-HDR output is active SDR10.
UMBRIEL_TEST(deriveEnabledXb30IsActive) { CHECK(deriveTenBitSdrActive(true, false, DRM_FORMAT_XBGR2101010)); }

// The 8-bit SDR fallback format is never reported as 10-bit SDR active.
UMBRIEL_TEST(deriveSdr8FallbackIsNotActive) { CHECK(!deriveTenBitSdrActive(true, false, DRM_FORMAT_XRGB8888)); }

// An 8-bit alpha format (e.g. a nested/wl backend target) is not SDR10 either.
UMBRIEL_TEST(deriveSdr8AlphaFormatIsNotActive) { CHECK(!deriveTenBitSdrActive(true, false, DRM_FORMAT_ARGB8888)); }

// HDR also commits a 10-bit render format, but an HDR-active output must not be
// misreported as 10-bit SDR: the HDR pipeline owns the format.
UMBRIEL_TEST(deriveHdrActiveTenBitFormatIsNotSdr10) {
  CHECK(!deriveTenBitSdrActive(true, true, DRM_FORMAT_XRGB2101010));
  CHECK(!deriveTenBitSdrActive(true, true, DRM_FORMAT_XBGR2101010));
}

// A disabled output reports no active SDR10 even if the last committed format
// was 10-bit. A DPMS-off output is disabled at the wlr_output level, so it
// takes this same path.
UMBRIEL_TEST(deriveDisabledOutputIsNotActive) {
  CHECK(!deriveTenBitSdrActive(false, false, DRM_FORMAT_XRGB2101010));
  CHECK(!deriveTenBitSdrActive(false, false, DRM_FORMAT_XBGR2101010));
  CHECK(!deriveTenBitSdrActive(false, false, DRM_FORMAT_XRGB8888));
}

// The disabled check dominates the format check: even a disabled + HDR + 10-bit
// combination is not SDR10.
UMBRIEL_TEST(deriveDisabledHdrTenBitIsNotActive) { CHECK(!deriveTenBitSdrActive(false, true, DRM_FORMAT_XRGB2101010)); }

// An invalid/uninitialized render format is not SDR10.
UMBRIEL_TEST(deriveInvalidFormatIsNotActive) { CHECK(!deriveTenBitSdrActive(true, false, DRM_FORMAT_INVALID)); }

int main() { return RUN_TESTS(); }
