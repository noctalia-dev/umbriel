#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace umbriel {
  struct OkLab {
    float lightness = 0.0F;
    float a = 0.0F;
    float b = 0.0F;
    bool operator==(const OkLab&) const = default;
  };

  namespace color_detail {
    inline float srgbToLinear(float component) {
      const float clamped = std::clamp(component, 0.0F, 1.0F);
      return clamped <= 0.04045F ? clamped / 12.92F : std::pow((clamped + 0.055F) / 1.055F, 2.4F);
    }

    inline float linearToSrgb(float component) {
      const float clamped = std::clamp(component, 0.0F, 1.0F);
      return clamped <= 0.0031308F ? clamped * 12.92F : 1.055F * std::pow(clamped, 1.0F / 2.4F) - 0.055F;
    }
  } // namespace color_detail

  inline OkLab srgbToOkLab(const std::array<float, 4>& color) {
    const float red = color_detail::srgbToLinear(color[0]);
    const float green = color_detail::srgbToLinear(color[1]);
    const float blue = color_detail::srgbToLinear(color[2]);

    const float l = 0.4122214708F * red + 0.5363325363F * green + 0.0514459929F * blue;
    const float m = 0.2119034982F * red + 0.6806995451F * green + 0.1073969566F * blue;
    const float s = 0.0883024619F * red + 0.2817188376F * green + 0.6299787005F * blue;

    const float lRoot = std::cbrt(l);
    const float mRoot = std::cbrt(m);
    const float sRoot = std::cbrt(s);
    return {
        .lightness = 0.2104542553F * lRoot + 0.7936177850F * mRoot - 0.0040720468F * sRoot,
        .a = 1.9779984951F * lRoot - 2.4285922050F * mRoot + 0.4505937099F * sRoot,
        .b = 0.0259040371F * lRoot + 0.7827717662F * mRoot - 0.8086757660F * sRoot,
    };
  }

  inline std::array<float, 4> okLabToSrgb(const OkLab& color, float alpha) {
    const float lRoot = color.lightness + 0.3963377774F * color.a + 0.2158037573F * color.b;
    const float mRoot = color.lightness - 0.1055613458F * color.a - 0.0638541728F * color.b;
    const float sRoot = color.lightness - 0.0894841775F * color.a - 1.2914855480F * color.b;

    const float l = lRoot * lRoot * lRoot;
    const float m = mRoot * mRoot * mRoot;
    const float s = sRoot * sRoot * sRoot;
    const float red = 4.0767439362F * l - 3.3077115913F * m + 0.2309699292F * s;
    const float green = -1.2684380046F * l + 2.6097574011F * m - 0.3413193965F * s;
    const float blue = -0.0041960863F * l - 0.7034186147F * m + 1.7076147010F * s;

    return {
        std::clamp(color_detail::linearToSrgb(red), 0.0F, 1.0F),
        std::clamp(color_detail::linearToSrgb(green), 0.0F, 1.0F),
        std::clamp(color_detail::linearToSrgb(blue), 0.0F, 1.0F),
        std::clamp(alpha, 0.0F, 1.0F),
    };
  }

  inline OkLab interpolateOkLab(const OkLab& from, const OkLab& to, float progress) {
    return {
        .lightness = std::lerp(from.lightness, to.lightness, progress),
        .a = std::lerp(from.a, to.a, progress),
        .b = std::lerp(from.b, to.b, progress),
    };
  }

  // Premultiplied RGBA from straight-alpha base × opacity multiplier.
  inline void premultiplied(float out[4], const std::array<float, 4>& base, float opacity) {
    const float a = base[3] * opacity;
    out[0] = base[0] * a;
    out[1] = base[1] * a;
    out[2] = base[2] * a;
    out[3] = a;
  }

  // Keycap surface: a subtle lift from the panel background toward the key accent.
  inline std::array<float, 4>
  keycapBackgroundColor(const std::array<float, 4>& background, const std::array<float, 4>& accent) {
    constexpr float kLift = 0.09F;
    std::array<float, 4> result{};
    for (size_t component = 0; component < 3; ++component) {
      result[component] = std::lerp(background[component], accent[component], kLift);
    }
    result[3] = 1.0F;
    return result;
  }

  // Mean colour of packed DRM_FORMAT_ARGB8888 pixels, weighted by alpha so a transparent pixel contributes no hue.
  // Null when there is nothing to average, or the whole strip is transparent.
  inline std::optional<std::array<float, 4>> averageArgb(const uint32_t* pixels, size_t count) {
    if (pixels == nullptr || count == 0) {
      return std::nullopt;
    }
    double alpha = 0.0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    for (size_t index = 0; index < count; ++index) {
      const uint32_t pixel = pixels[index];
      const double weight = ((pixel >> 24U) & 0xFFU) / 255.0;
      alpha += weight;
      red += ((pixel >> 16U) & 0xFFU) / 255.0 * weight;
      green += ((pixel >> 8U) & 0xFFU) / 255.0 * weight;
      blue += (pixel & 0xFFU) / 255.0 * weight;
    }
    if (alpha <= 0.0) {
      return std::nullopt;
    }
    return std::array<float, 4>{
        static_cast<float>(red / alpha),
        static_cast<float>(green / alpha),
        static_cast<float>(blue / alpha),
        static_cast<float>(alpha / static_cast<double>(count)),
    };
  }

  inline std::string rgbaHex(const std::array<float, 4>& color) {
    const auto byte = [](float component) {
      return static_cast<int>(std::lround(std::clamp(component, 0.0F, 1.0F) * 255.0F));
    };
    return std::format("#{:02X}{:02X}{:02X}{:02X}", byte(color[0]), byte(color[1]), byte(color[2]), byte(color[3]));
  }

} // namespace umbriel
