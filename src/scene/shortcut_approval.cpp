#include "scene/shortcut_approval.h"

#include "scene/text_buffer.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <glib.h>
#include <limits>
#include <pango/pango.h>
#include <utility>

namespace umbriel {
  namespace {
    constexpr int kMaxPanelExtent = 960;

    std::string displayText(std::string_view text) {
      std::string result;
      if (text.empty()) {
        return result;
      }
      const char* current = text.data();
      const char* end = current + text.size();
      while (current < end) {
        const char* next = g_utf8_next_char(current);
        switch (g_unichar_type(g_utf8_get_char(current))) {
        case G_UNICODE_CONTROL:
        case G_UNICODE_FORMAT:
        case G_UNICODE_LINE_SEPARATOR:
        case G_UNICODE_PARAGRAPH_SEPARATOR:
        case G_UNICODE_SURROGATE:
        case G_UNICODE_UNASSIGNED:
          result += '?';
          break;
        default:
          result.append(current, static_cast<size_t>(next - current));
          break;
        }
        current = next;
      }
      return result;
    }

    std::string_view conflictText(GlobalShortcutConflict conflict) {
      switch (conflict) {
      case GlobalShortcutConflict::None:
        return "Available";
      case GlobalShortcutConflict::Unassigned:
        return "No assignable trigger";
      case GlobalShortcutConflict::Disabled:
        return "Disabled by configuration";
      case GlobalShortcutConflict::Reserved:
        return "Reserved for compositor bindings";
      case GlobalShortcutConflict::InUse:
        return "Waiting for another live session";
      }
      std::unreachable();
    }

    std::string footer(size_t page, size_t count, bool final) {
      return std::format(
          "\n\nPage {} of {}\n{}\nAny other key or click cancels.", page, count,
          final ? "Enter: approve all shortcuts" : "Enter: review next page"
      );
    }

    std::vector<size_t> boundaries(std::string_view text) {
      const auto characters = g_utf8_strlen(text.data(), static_cast<gssize>(text.size()));
      std::vector<PangoLogAttr> attributes(static_cast<size_t>(characters) + 1);
      pango_get_log_attrs(
          text.data(), static_cast<int>(text.size()), 0, pango_language_get_default(), attributes.data(),
          static_cast<int>(attributes.size())
      );
      std::vector<size_t> result;
      result.reserve(attributes.size());
      const char* current = text.data();
      for (const auto& attribute : attributes) {
        if (attribute.is_cursor_position != 0) {
          result.push_back(static_cast<size_t>(current - text.data()));
        }
        if (current < text.data() + text.size()) {
          current = g_utf8_next_char(current);
        }
      }
      return result;
    }

    TextBufferParams textParams(const ShortcutApprovalLayout& layout, std::string_view text) {
      return {
          .markup = escapeMarkup(text),
          .font = std::string(kShortcutApprovalFont),
          .maxWidth = layout.width - kShortcutApprovalPadding * 2,
          .padding = kShortcutApprovalPadding,
          .scale = layout.scale,
          .fixedWidth = true,
      };
    }

    bool fits(const ShortcutApprovalLayout& layout, std::string_view text) {
      const auto size = measureTextBuffer(textParams(layout, text));
      return size.logicalWidth <= layout.width && size.logicalHeight <= layout.height;
    }
  } // namespace

  std::string ShortcutApprovalLayout::pageText(size_t page) const {
    const auto& range = pages.at(page);
    return header + content.substr(range.begin, range.end - range.begin) + footer(page + 1, pages.size(), range.final);
  }

  TextBufferParams ShortcutApprovalLayout::pageParams(size_t page) const { return textParams(*this, pageText(page)); }

  std::optional<ShortcutApprovalLayout> layoutShortcutApproval(
      std::string_view application, std::span<const GlobalShortcutRegistration> shortcuts, int maxWidth, int maxHeight,
      double scale
  ) {
    if (!validGlobalShortcutId(application)
        || shortcuts.empty()
        || shortcuts.size() > kMaxGlobalShortcuts
        || maxWidth <= kShortcutApprovalPadding * 2
        || maxHeight <= kShortcutApprovalPadding * 2
        || !std::isfinite(scale)
        || scale <= 0.0) {
      return std::nullopt;
    }
    std::vector<GlobalShortcut> validation;
    validation.reserve(shortcuts.size());
    for (const auto& shortcut : shortcuts) {
      validation.push_back({shortcut.id, shortcut.description, shortcut.candidate});
    }
    if (!validGlobalShortcutList(validation)) {
      return std::nullopt;
    }
    ShortcutApprovalLayout layout;
    layout.width = std::min(maxWidth, kMaxPanelExtent);
    layout.height = std::min(maxHeight, kMaxPanelExtent);
    layout.scale = std::max(1.0, scale);
    if (layout.scale * std::max(layout.width, layout.height) > std::numeric_limits<int>::max()) {
      return std::nullopt;
    }
    layout.header = std::format(
        "Allow global shortcuts?\nApplication label: {}\nThis label is supplied by the requester.\n\n",
        displayText(application)
    );
    for (const auto& shortcut : shortcuts) {
      layout.content += std::format(
          "Shortcut: {}\nDescription: {}\nTrigger: {}\nStatus: {}\n\n", displayText(shortcut.id),
          displayText(shortcut.description),
          shortcut.candidate ? describeGlobalShortcutTrigger(*shortcut.candidate) : "Unassigned",
          conflictText(shortcut.conflict)
      );
    }
    const auto positions = boundaries(layout.content);
    if (positions.size() < 2 || positions.front() != 0 || positions.back() != layout.content.size()) {
      return std::nullopt;
    }
    // Page counts cannot exceed the number of content bytes. Reserve both
    // control labels using that count before choosing any page boundary.
    const auto nextFooter = footer(layout.content.size(), layout.content.size(), false);
    const auto finalFooter = footer(layout.content.size(), layout.content.size(), true);
    const auto pageFits = [&](size_t begin, size_t end) {
      const auto text = layout.header + layout.content.substr(positions[begin], positions[end] - positions[begin]);
      return fits(layout, text + nextFooter) && fits(layout, text + finalFooter);
    };
    size_t begin = 0;
    const auto last = positions.size() - 1;
    while (begin < last) {
      size_t best = begin;
      size_t step = 1;
      size_t upper = begin;
      do {
        upper = begin + std::min(step, last - begin);
        if (!pageFits(begin, upper)) {
          break;
        }
        best = upper;
        step *= 2;
      } while (upper < last);
      size_t low = best + 1;
      size_t high = upper;
      while (low < high) {
        const auto middle = low + (high - low) / 2;
        if (pageFits(begin, middle)) {
          best = middle;
          low = middle + 1;
        } else {
          high = middle;
        }
      }
      if (best == begin) {
        return std::nullopt;
      }
      if (best < last) {
        const auto chunk =
            std::string_view(layout.content).substr(positions[begin], positions[best] - positions[begin]);
        const auto space = chunk.find_last_of(" \n");
        if (space != std::string_view::npos && space >= chunk.size() * 3 / 4) {
          const auto found = std::ranges::lower_bound(positions, positions[begin] + space + 1);
          if (found != positions.end() && *found == positions[begin] + space + 1) {
            best = static_cast<size_t>(found - positions.begin());
          }
        }
      }
      layout.pages.push_back({positions[begin], positions[best]});
      begin = best;
    }
    layout.pages.back().final = true;
    for (size_t page = 0; page < layout.pages.size(); ++page) {
      if (!fits(layout, layout.pageText(page))) {
        return std::nullopt;
      }
    }
    return layout;
  }

} // namespace umbriel
