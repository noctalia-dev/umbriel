#include "output/hdr_metadata.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

extern "C" {
#include <libdisplay-info/info.h>
#if UMBRIEL_HAS_DRM_BACKEND
#include <unistd.h>
#include <wlr/backend/drm.h>
#include <xf86drmMode.h>
#endif
}

namespace umbriel {

  namespace {

    double finiteRange(float value, double maximum) {
      return std::isfinite(value) ? std::clamp(static_cast<double>(value), 0.0, maximum) : 0.0;
    }

    // The hull bound is Z >= 0 as x + y <= 1. Measured and s15Fixed16-quantized
    // colorants land a few 1e-5 on the wrong side of that line for legitimate
    // limb primaries (e.g. 0.68/0.32), so the bound admits that much noise.
    constexpr double kHullSlack = 1e-4;

    bool validCoordinate(const wlr_color_cie1931_xy& coordinate) {
      return std::isfinite(coordinate.x)
          && std::isfinite(coordinate.y)
          && coordinate.x >= 0.0
          && coordinate.y >= 0.0
          && coordinate.x + coordinate.y <= 1.0 + kHullSlack;
    }

#if UMBRIEL_HAS_DRM_BACKEND
    struct FdCloser {
      void operator()(int* fd) const {
        if (fd != nullptr) {
          close(*fd);
          delete fd;
        }
      }
    };

    struct ObjectPropertiesDeleter {
      void operator()(drmModeObjectProperties* properties) const { drmModeFreeObjectProperties(properties); }
    };

    struct PropertyDeleter {
      void operator()(drmModePropertyRes* property) const { drmModeFreeProperty(property); }
    };

    struct PropertyBlobDeleter {
      void operator()(drmModePropertyBlobRes* blob) const { drmModeFreePropertyBlob(blob); }
    };
#endif

  } // namespace

  bool validDisplayPrimaries(const wlr_color_primaries& primaries) {
    if (!validCoordinate(primaries.red)
        || !validCoordinate(primaries.green)
        || !validCoordinate(primaries.blue)
        || !validCoordinate(primaries.white)) {
      return false;
    }
    const double twiceArea = std::abs(
        (primaries.green.x - primaries.red.x) * (primaries.blue.y - primaries.red.y)
        - (primaries.blue.x - primaries.red.x) * (primaries.green.y - primaries.red.y)
    );
    return twiceArea > 0.000001 && primaries.white.x > 0.0 && primaries.white.y > 0.0;
  }

  std::vector<uint8_t> edidBlobForOutput(wlr_output* output) {
    std::vector<uint8_t> edid;
#if UMBRIEL_HAS_DRM_BACKEND
    if (output == nullptr || !wlr_output_is_drm(output)) {
      return edid;
    }

    const int rawFd = wlr_drm_backend_get_non_master_fd(output->backend);
    if (rawFd < 0) {
      return edid;
    }
    const std::unique_ptr<int, FdCloser> fd(new int(rawFd));
    const uint32_t connectorId = wlr_drm_connector_get_id(output);
    const std::unique_ptr<drmModeObjectProperties, ObjectPropertiesDeleter> properties(
        drmModeObjectGetProperties(*fd, connectorId, DRM_MODE_OBJECT_CONNECTOR)
    );
    if (!properties) {
      return edid;
    }

    for (uint32_t index = 0; index < properties->count_props; ++index) {
      const std::unique_ptr<drmModePropertyRes, PropertyDeleter> property(
          drmModeGetProperty(*fd, properties->props[index])
      );
      if (!property
          || (property->flags & DRM_MODE_PROP_BLOB) == 0
          || std::strcmp(property->name, "EDID") != 0
          || properties->prop_values[index] == 0) {
        continue;
      }
      const std::unique_ptr<drmModePropertyBlobRes, PropertyBlobDeleter> blob(
          drmModeGetPropertyBlob(*fd, properties->prop_values[index])
      );
      if (!blob || blob->data == nullptr || blob->length == 0) {
        return edid;
      }
      const auto* bytes = static_cast<const uint8_t*>(blob->data);
      edid.assign(bytes, bytes + blob->length);
      return edid;
    }
#else
    (void)output;
#endif
    return edid;
  }

  std::optional<HdrStaticMetadata> hdrStaticMetadataFromEdid(std::span<const uint8_t> edid) {
    if (edid.empty()) {
      return std::nullopt;
    }
    std::unique_ptr<di_info, decltype(&di_info_destroy)> info(
        di_info_parse_edid(edid.data(), edid.size()), di_info_destroy
    );
    if (!info) {
      return std::nullopt;
    }

    const di_hdr_static_metadata* source = di_info_get_hdr_static_metadata(info.get());
    if (!source->type1 || !source->pq) {
      return std::nullopt;
    }

    HdrStaticMetadata metadata{
        .minLuminance = finiteRange(source->desired_content_min_luminance, 6.5535),
        .maxLuminance = std::round(finiteRange(source->desired_content_max_luminance, 65535.0)),
        .maxFrameAverageLuminance = std::round(finiteRange(source->desired_content_max_frame_avg_luminance, 65535.0)),
    };
    if (metadata.maxLuminance > 0.0) {
      metadata.maxFrameAverageLuminance = std::min(metadata.maxFrameAverageLuminance, metadata.maxLuminance);
    }
    return metadata;
  }

  std::optional<HdrStaticMetadata> hdrStaticMetadataForOutput(wlr_output* output) {
    return hdrStaticMetadataFromEdid(edidBlobForOutput(output));
  }

  wlr_output_image_description makeHdrOutputDescription(
      const wlr_color_primaries* displayPrimaries, const std::optional<HdrStaticMetadata>& metadata
  ) {
    wlr_color_primaries targetPrimaries{};
    if (displayPrimaries != nullptr && validDisplayPrimaries(*displayPrimaries)) {
      targetPrimaries = *displayPrimaries;
    } else {
      wlr_color_primaries_from_named(&targetPrimaries, WLR_COLOR_NAMED_PRIMARIES_BT2020);
    }

    const double minLuminance = metadata ? metadata->minLuminance : 0.0;
    const double maxLuminance = metadata ? metadata->maxLuminance : 0.0;
    const double maxFrameAverageLuminance = metadata ? metadata->maxFrameAverageLuminance : 0.0;
    return {
        .primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020,
        .transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
        .mastering_display_primaries = targetPrimaries,
        .mastering_luminance = {.min = minLuminance, .max = maxLuminance},
        .max_cll = maxLuminance,
        .max_fall = maxFrameAverageLuminance,
    };
  }

} // namespace umbriel
