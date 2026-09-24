#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace umbriel {
  class Section;
  struct FrameInsets {
    int top = 0, bottom = 0, left = 0, right = 0;
    bool operator==(const FrameInsets&) const = default;
  };
  struct NineRectAsset {
    int width = 0, height = 0;
    FrameInsets slices;
    std::optional<FrameInsets> contentInsets;
    [[nodiscard]] FrameInsets content() const { return contentInsets.value_or(slices); }
    [[nodiscard]] std::array<int, 4> destinationCuts(int client, bool horizontal, float scale = 1.0F) const;
    bool tileX = false, tileY = false, overlay = false;
    bool tintWithBorderColor = false;
    std::vector<uint32_t> pixels; // Premultiplied ARGB8888.
    bool operator==(const NineRectAsset&) const = default;
  };
  struct NineRectConfig {
    std::shared_ptr<const NineRectAsset> asset;
    bool operator==(const NineRectConfig& other) const {
      return asset == other.asset || (asset && other.asset && *asset == *other.asset);
    }
  };
  // Throws on invalid settings or assets. The caller reports a fatal configuration diagnostic.
  NineRectConfig readNineRect(Section& section, bool enabled);
} // namespace umbriel
