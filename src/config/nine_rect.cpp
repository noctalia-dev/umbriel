#include "config/nine_rect.h"

#include "config/section.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <filesystem>
#include <png.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace umbriel {
  namespace {
    struct Image {
      int width, height;
      std::vector<uint8_t> rgba;
    };
    Image readPng(const std::string& path) {
      const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
      struct stat st{};
      if (fd < 0)
        throw std::runtime_error("cannot open PNG: " + path);
      if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > 64 * 1024 * 1024) {
        close(fd);
        throw std::runtime_error("PNG must be a regular file of at most 64 MiB: " + path);
      }
      FILE* file = fdopen(fd, "rb");
      if (!file) {
        close(fd);
        throw std::runtime_error("cannot read PNG: " + path);
      }
      png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
      png_infop info = png ? png_create_info_struct(png) : nullptr;
      if (!info) {
        if (png)
          png_destroy_read_struct(&png, nullptr, nullptr);
        fclose(file);
        throw std::runtime_error("PNG allocation failed");
      }
      struct Resources {
        FILE* file;
        png_structp png;
        png_infop info;
        ~Resources() {
          png_destroy_read_struct(&png, &info, nullptr);
          fclose(file);
        }
      } resources{file, png, info};
      if (setjmp(png_jmpbuf(png))) {
        throw std::runtime_error("invalid PNG (expected RGB8/RGBA8 artwork, maximum 4096 per axis): " + path);
      }
      png_init_io(png, file);
      png_set_user_limits(png, 4096, 4096);
      png_set_chunk_malloc_max(png, 8 * 1024 * 1024);
      png_set_chunk_cache_max(png, 128);
      png_read_info(png, info);
      const auto w = png_get_image_width(png, info), h = png_get_image_height(png, info);
      const int type = png_get_color_type(png, info);
      if (png_get_bit_depth(png, info) != 8 || (type != PNG_COLOR_TYPE_RGB && type != PNG_COLOR_TYPE_RGBA))
        png_error(png, "unsupported PNG format");
      // Preserve artwork samples without applying metadata color transforms.
      if (type == PNG_COLOR_TYPE_RGB)
        png_set_add_alpha(png, 255, PNG_FILLER_AFTER);
      png_set_interlace_handling(png);
      png_read_update_info(png, info);
      // libpng owns rows so its error path can release every allocation.
      auto rowPointers = static_cast<png_bytepp>(png_calloc(png, h * sizeof(png_bytep)));
      png_set_rows(png, info, rowPointers);
      png_data_freer(png, info, PNG_DESTROY_WILL_FREE_DATA, PNG_FREE_ROWS);
      for (unsigned y = 0; y < h; ++y)
        rowPointers[y] = static_cast<png_bytep>(png_malloc(png, w * 4));
      png_read_image(png, rowPointers);
      png_read_end(png, nullptr);
      Image result{static_cast<int>(w), static_cast<int>(h), {}};
      result.rgba.resize(static_cast<size_t>(w) * h * 4);
      auto rows = png_get_rows(png, info);
      for (unsigned y = 0; y < h; ++y)
        std::copy_n(rows[y], w * 4, result.rgba.data() + static_cast<size_t>(y) * w * 4);
      return result;
    }
    std::string pathValue(const toml::node* node, bool required) {
      if (!node) {
        if (required)
          throw std::runtime_error("nine_rect.texture is required");
        return {};
      }
      auto value = node->value<std::string>();
      if (!value || value->empty() || value->contains('\0'))
        throw std::runtime_error("nine_rect paths must be nonempty strings");
      std::filesystem::path path(*value);
      if (path.is_relative()) {
        if (!node->source().path)
          throw std::runtime_error("relative asset path requires config source");
        path = std::filesystem::path(*node->source().path).parent_path() / path;
      }
      return path.lexically_normal().string();
    }
    bool mode(Section& s, const char* key, const char* first, const char* second) {
      const auto* n = s.take(key);
      if (!n)
        return false;
      auto v = n->value<std::string>();
      if (!v || (*v != first && *v != second))
        throw std::runtime_error(std::string("invalid nine_rect.") + key);
      return *v == second;
    }
  } // namespace
  std::array<int, 4> NineRectAsset::destinationCuts(int client, bool horizontal, float scale) const {
    const auto c = content();
    const int before = horizontal ? slices.left : slices.top;
    const int after = horizontal ? slices.right : slices.bottom;
    const int insetBefore = horizontal ? c.left : c.top;
    const int insetAfter = horizontal ? c.right : c.bottom;
    const auto px = [scale](int value) { return static_cast<int>(std::lround(value * scale)); };
    std::array<int, 4> cuts{
        -px(insetBefore), px(before - insetBefore), std::max(0, client) - px(after - insetAfter),
        std::max(0, client) + px(insetAfter)
    };
    // Very small clients compress their corners rather than overlapping or inverting them.
    if (cuts[1] > cuts[2]) {
      cuts[1] = cuts[2] =
          cuts[0] + static_cast<int>(std::lround(static_cast<double>(cuts[3] - cuts[0]) * before / (before + after)));
    }
    return cuts;
  }
  NineRectConfig readNineRect(Section& s, bool enabled) {
    const auto texture = pathValue(s.take("texture"), enabled);
    auto asset = std::make_shared<NineRectAsset>();
    asset->tileX = mode(s, "horizontal_stretch_mode", "stretch", "tile");
    asset->tileY = mode(s, "vertical_stretch_mode", "stretch", "tile");
    asset->overlay = mode(s, "center_mode", "none", "overlay");
    if (const auto* node = s.take("tint_with_border_color")) {
      const auto value = node->value<bool>();
      if (!value)
        throw std::runtime_error("nine_rect.tint_with_border_color must be a boolean");
      asset->tintWithBorderColor = *value;
    }
    const auto* pixels = s.take("slice_px");
    const auto* fractions = s.take("slice");
    if ((pixels && fractions) || (enabled && !pixels && !fractions))
      throw std::runtime_error("nine_rect requires exactly one of slice_px and slice");
    std::array<double, 4> values{};
    const auto* node = pixels ? pixels : fractions;
    if (node) {
      if (!node->is_table())
        throw std::runtime_error("nine_rect slices must be a table");
      const std::array<const char*, 4> keys{"top", "bottom", "left", "right"};
      if (node->as_table()->size() != 4)
        throw std::runtime_error("nine_rect slices require top, bottom, left, right");
      for (size_t i = 0; i < 4; ++i) {
        const auto* v = node->as_table()->get(keys[i]);
        const auto number = v ? v->value<double>() : std::nullopt;
        if (!number
            || !std::isfinite(*number)
            || *number < 0
            || *number > (fractions ? 1 : 4096)
            || (pixels && std::floor(*number) != *number))
          throw std::runtime_error("invalid nine_rect slice (pixels must be integers, fractions between zero and one)");
        values[i] = *number;
      }
      if (fractions && (values[0] + values[1] > 1 || values[2] + values[3] > 1))
        throw std::runtime_error("nine_rect slices exceed source dimensions");
    }
    const auto* insetPixels = s.take("content_inset_px");
    const auto* insetFractions = s.take("content_inset");
    if (insetPixels && insetFractions)
      throw std::runtime_error("nine_rect accepts only one of content_inset_px and content_inset");
    std::array<double, 4> content{};
    const auto* inset = insetPixels ? insetPixels : insetFractions;
    if (inset) {
      if (!inset->is_table() || inset->as_table()->size() != 4)
        throw std::runtime_error("nine_rect content insets require top, bottom, left, right");
      const std::array<const char*, 4> keys{"top", "bottom", "left", "right"};
      for (size_t i = 0; i < keys.size(); ++i) {
        const auto* value = inset->as_table()->get(keys[i]);
        const auto number = value ? value->value<double>() : std::nullopt;
        if (!number
            || !std::isfinite(*number)
            || *number < 0
            || *number > (insetFractions ? 1 : 4096)
            || (insetPixels && std::floor(*number) != *number))
          throw std::runtime_error(
              "invalid nine_rect content inset (pixels must be integers, fractions between zero and one)"
          );
        content[i] = *number;
      }
    }
    if (!s.allKeysKnown())
      throw std::runtime_error("unknown nine_rect option");
    if (!enabled)
      return {};
    const Image visual = readPng(texture);
    asset->width = visual.width;
    asset->height = visual.height;
    for (size_t i = 0; i < 4; ++i)
      if (fractions)
        values[i] = std::floor(values[i] * (i < 2 ? visual.height : visual.width) + 0.5);
    asset->slices = {
        static_cast<int>(values[0]), static_cast<int>(values[1]), static_cast<int>(values[2]),
        static_cast<int>(values[3])
    };
    const auto& b = asset->slices;
    if (b.top + b.bottom > visual.height || b.left + b.right > visual.width)
      throw std::runtime_error("nine_rect slices exceed source dimensions");
    if (inset) {
      for (size_t i = 0; i < 4; ++i)
        if (insetFractions)
          content[i] = std::floor(content[i] * (i < 2 ? visual.height : visual.width) + 0.5);
      asset->contentInsets = FrameInsets{
          static_cast<int>(content[0]), static_cast<int>(content[1]), static_cast<int>(content[2]),
          static_cast<int>(content[3])
      };
    }
    const auto c = asset->content();
    if (c.top > b.top || c.bottom > b.bottom || c.left > b.left || c.right > b.right)
      throw std::runtime_error("nine_rect content insets must not exceed the corresponding slice");
    const size_t count = static_cast<size_t>(visual.width) * visual.height;
    asset->pixels.resize(count);
    for (size_t i = 0; i < count; ++i) {
      const auto* c = visual.rgba.data() + i * 4;
      const uint32_t a = c[3];
      asset->pixels[i] =
          (a << 24) | (((c[0] * a + 127) / 255) << 16) | (((c[1] * a + 127) / 255) << 8) | ((c[2] * a + 127) / 255);
    }
    return {std::move(asset)};
  }
} // namespace umbriel
