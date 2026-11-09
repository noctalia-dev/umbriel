#pragma once

#include <array>
#include <cmath>

namespace umbriel {

  // Perceived brightness of an encoded colour, with the weights the button hover shift already uses.
  inline float luminance(const std::array<float, 4>& color) {
    return color[0] * 0.2126F + color[1] * 0.7152F + color[2] * 0.0722F;
  }

  // How far apart two brightnesses must be before a title written in one stays readable on the other. Below it the
  // title vanishes into the bar, which an adaptive fill makes possible.
  constexpr float kTitleContrastThreshold = 0.35F;

  // `color` itself, or black or white when it would sit too close to `background` to read. A colour with no alpha is
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

} // namespace umbriel
