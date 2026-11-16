// Inert ICC parsing for builds without lcms2. The ICC path reports itself
// unsupported and `color_profile = "<path>"` warns and falls back at apply time.

#include "output/colorimetry.h"
#include "output/colorimetry_internal.h"

#include <span>

namespace umbriel {

  IccParse parseIccProfile(std::span<const uint8_t> data, IccClassScope) {
    if (const std::string structureError = color_detail::iccStructureError(data); !structureError.empty()) {
      return {.colorimetry = std::nullopt, .error = structureError, .warnings = {}};
    }
    return {.colorimetry = std::nullopt, .error = "ICC profiles are unsupported (built without lcms2)", .warnings = {}};
  }

  IccProfileStore iccStoreForPanel(const PanelColorimetry*) {
    // No synthesis without lcms2: an EDID characterization advertises nothing.
    return {};
  }

} // namespace umbriel
