#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace umbriel {

  // Perceived brightness of an encoded color.
  inline float luminance(const std::array<float, 4>& color) {
    return color[0] * 0.2126F + color[1] * 0.7152F + color[2] * 0.0722F;
  }

  // How far apart two brightnesses must be before a title written in one stays readable on the other. Below it the
  // title vanishes into the bar, which an adaptive fill makes possible.
  constexpr float kTitleContrastThreshold = 0.35F;

  // `color` itself, or black or white when it would sit too close to `background` to read. A color with no alpha is
  // left alone: there is nothing to read, and a flipped one would carry brightness the composite cannot ignore. A
  // background with no alpha is left alone too: it is not what shows, so there is nothing to judge against.
  inline std::array<float, 4>
  contrastAdjusted(const std::array<float, 4>& color, const std::array<float, 4>& background) {
    if (color[3] <= 0.0F
        || background[3] <= 0.0F
        || std::abs(luminance(color) - luminance(background)) >= kTitleContrastThreshold) {
      return color;
    }
    const float value = luminance(background) < 0.5F ? 1.0F : 0.0F;
    return {value, value, value, color[3]};
  }

  // What the eye sees when `fill` is drawn at `fillOpacity` over `backdrop`, both straight-alpha. The palette sits
  // under every adaptive fill, and an inactive bar draws that fill dimmed over the unfocused palette, so a color
  // judged for readability must be this composite rather than the fill alone. An opaque fill at full opacity is
  // itself, and the palette drawn over itself is unchanged.
  inline std::array<float, 4>
  asShownBarColor(const std::array<float, 4>& fill, float fillOpacity, const std::array<float, 4>& backdrop) {
    // The palette drawn over itself is the palette, alpha included: the general formula would inflate the coverage.
    if (fill == backdrop) {
      return fill;
    }
    const float sourceAlpha = fill[3] * std::clamp(fillOpacity, 0.0F, 1.0F);
    const float alpha = sourceAlpha + backdrop[3] * (1.0F - sourceAlpha);
    if (alpha <= 0.0F) {
      return {0.0F, 0.0F, 0.0F, 0.0F};
    }
    std::array<float, 4> shown{};
    for (std::size_t i = 0; i < 3; ++i) {
      shown[i] = (fill[i] * sourceAlpha + backdrop[i] * backdrop[3] * (1.0F - sourceAlpha)) / alpha;
    }
    shown[3] = alpha;
    return shown;
  }

} // namespace umbriel
