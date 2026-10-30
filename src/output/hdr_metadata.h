#pragma once

#include "wlr_color.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

extern "C" {
#include <wlr/types/wlr_output.h>
}

namespace umbriel {

  struct HdrStaticMetadata {
    double minLuminance = 0.0;
    double maxLuminance = 0.0;
    double maxFrameAverageLuminance = 0.0;
    bool operator==(const HdrStaticMetadata&) const = default;
  };

  // True when the coordinates form a usable, non-degenerate gamut with a
  // positive white point.
  [[nodiscard]] bool validDisplayPrimaries(const wlr_color_primaries& primaries);

  // The raw EDID blob attached to a native DRM output. Nested and headless
  // outputs and unreadable EDIDs give an empty span.
  [[nodiscard]] std::vector<uint8_t> edidBlobForOutput(wlr_output* output);

  // Parse the sink's CTA-861 HDR Static Metadata Data Block. Values are the
  // display's desired content luminances in cd/m², with unavailable fields left at zero.
  [[nodiscard]] std::optional<HdrStaticMetadata> hdrStaticMetadataFromEdid(std::span<const uint8_t> edid);

  // Read and parse the EDID attached to a native DRM output. Nested and virtual
  // outputs, unreadable EDIDs, and sinks without PQ static metadata return no value.
  [[nodiscard]] std::optional<HdrStaticMetadata> hdrStaticMetadataForOutput(wlr_output* output);

  // Describe Umbriel's BT.2020 PQ composition together with the display target
  // volume. BT.2020/D65 is used when the display does not publish valid primaries.
  [[nodiscard]] wlr_output_image_description makeHdrOutputDescription(
      const wlr_color_primaries* displayPrimaries, const std::optional<HdrStaticMetadata>& metadata
  );

} // namespace umbriel
