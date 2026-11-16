#pragma once

// Implementation details shared by the colorimetry translation units. This is
// not part of the module's public surface (colorimetry.h): sampled-curve
// resolution and the adaptation model are invariants the parser and the
// transform builder must agree on, and a single definition keeps them from
// drifting apart.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace umbriel::color_detail {

  // Sampled-curve resolution for PanelTransfer::Kind::Curve tables and for the
  // forward curves read from ICC profiles.
  inline constexpr size_t kCurveSamples = 1024;

  // Sampled encode tables sit on a quadratic input grid. Encode curves are
  // near-vertical at zero (x^(1/g)); on a uniform grid the first lerp segment
  // alone costs several 8-bit codes in the shadows no matter how many samples
  // are taken, while the quadratic grid spends its resolution where the curve
  // rises fastest and keeps the top end at the spacing a uniform 256-entry
  // table had.
  constexpr float encodeGridLinear(size_t index, size_t size) {
    const float position = size > 1 ? static_cast<float>(index) / static_cast<float>(size - 1) : 0.0F;
    return position * position;
  }

  inline float encodeGridPosition(float linear, size_t size) {
    return std::sqrt(std::clamp(linear, 0.0F, 1.0F)) * static_cast<float>(size > 1 ? size - 1 : 0);
  }

  // Row-major 3x3 inversion shared by the chad handling and the gamut matrix.
  [[nodiscard]] bool invert3x3(const double in[9], double out[9]);

  // Bradford chromatic adaptation between two XYZ whites (Y = 1). The gamut
  // mapping and the ICC white derivation must share one adaptation model or
  // their whites drift apart; identity is returned for degenerate whites.
  void bradfordAdapt(const double fromXyz[3], const double toXyz[3], double out[9]);

  // Structural bounds validation for raw ICC bytes before any parser touches
  // them: header size/magic/version, tag-table counts, offset+length overflow,
  // curv/para counts. Empty result means the bytes are structurally sound.
  // Client profiles are untrusted and can run to 32 MB, so this gate is what
  // keeps malformed offsets and counts away from the parsers.
  [[nodiscard]] std::string iccStructureError(std::span<const uint8_t> data);

} // namespace umbriel::color_detail
