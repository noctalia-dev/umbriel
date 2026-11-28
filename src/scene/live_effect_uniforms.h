#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

extern "C" {
#include <umbrielfx/render/effect.h>
}

namespace umbriel {

  // Reserve room in fx_animation_parameters for time, palette, palette_count
  // and the compositor-owned availability uniform (FX_UNIFORMS_MAX == 8).
  inline constexpr size_t kMaxLiveEffectUniforms = 4;

  // A complete output-scoped snapshot. Producers assign meaning to the names;
  // the compositor only validates their GLSL types and forwards their values.
  struct LiveEffectUniforms {
    std::string output;
    std::array<fx_uniform, kMaxLiveEffectUniforms> uniforms{};
    size_t count = 0;

    bool operator==(const LiveEffectUniforms& other) const {
      return output == other.output
          && std::ranges::is_permutation(
                 std::span(uniforms).first(count), std::span(other.uniforms).first(other.count),
                 [](const fx_uniform& a, const fx_uniform& b) {
                   return std::string_view(a.name) == b.name
                       && a.type == b.type
                       && a.count == b.count
                       && std::ranges::equal(a.floats, b.floats);
                 }
          );
    }
  };

} // namespace umbriel
