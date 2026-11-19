#pragma once

#include "input/global_shortcuts.h"
#include "scene/text_buffer.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace umbriel {

  inline constexpr std::string_view kShortcutApprovalFont = "monospace 11";
  inline constexpr int kShortcutApprovalPadding = 22;

  struct ShortcutApprovalPage {
    size_t begin = 0;
    size_t end = 0;
    bool final = false;
  };

  struct ShortcutApprovalLayout {
    std::string header;
    std::string content;
    std::vector<ShortcutApprovalPage> pages;
    int width = 0;
    int height = 0;
    double scale = 1.0;

    std::string pageText(size_t page) const;
    TextBufferParams pageParams(size_t page) const;
  };

  // Dimensions bound the complete panel, including padding. Page ranges cover
  // every byte of the sanitized content and split only at grapheme boundaries.
  std::optional<ShortcutApprovalLayout> layoutShortcutApproval(
      std::string_view application, std::span<const GlobalShortcutRegistration> shortcuts, int maxWidth, int maxHeight,
      double scale = 1.0
  );

} // namespace umbriel
