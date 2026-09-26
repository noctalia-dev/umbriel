#include "scene/tab_bar.h"

#include "config/config.h"
#include "scene/color.h"
#include "scene/tab_bar_geometry.h"
#include "scene/text_buffer.h"
#include "view/border_ring.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace umbriel {

  namespace {

    // Space between the bar's edge and the active tab's fill.
    constexpr int kFillInset = 3;
    // Space a label keeps from either side of its slot.
    constexpr int kLabelPadding = 8;

    bool rejectInput(wlr_scene_buffer* /*buffer*/, double* /*x*/, double* /*y*/) { return false; }

    void setRectColor(wlr_scene_rect* rect, const std::array<float, 4>& color, float alpha) {
      float premultipliedColor[4]{};
      premultiplied(premultipliedColor, color, alpha);
      wlr_scene_rect_set_color(rect, premultipliedColor);
    }

  } // namespace

  TabBar::TabBar(wlr_scene_tree* parent) {
    m_tree = wlr_scene_tree_create(parent);
    const std::array<float, 4> clear{0.0F, 0.0F, 0.0F, 0.0F};
    float color[4]{};
    premultiplied(color, clear, 1.0F);
    m_background = wlr_scene_rect_create(m_tree, 0, 0, color);
    m_activeFill = wlr_scene_rect_create(m_tree, 0, 0, color);
    // Clicks on the bar are the compositor's (Workspace::tabAt), never a window's.
    m_background->accepts_input = false;
    m_activeFill->accepts_input = false;
  }

  TabBar::~TabBar() {
    if (m_tree != nullptr) {
      wlr_scene_node_destroy(&m_tree->node);
    }
  }

  void TabBar::setEnabled(bool enabled) { wlr_scene_node_set_enabled(&m_tree->node, enabled); }

  void TabBar::setAlpha(float alpha) {
    alpha = std::clamp(alpha, 0.0F, 1.0F);
    if (alpha == m_alpha) {
      return;
    }
    m_alpha = alpha;
    applyColors();
  }

  void TabBar::invalidate() {
    for (Label& label : m_labels) {
      label.key.clear();
    }
  }

  void TabBar::applyColors() {
    const auto& colors = config().colors.tabBar;
    setRectColor(m_background, colors.background, m_alpha);
    setRectColor(m_activeFill, m_focused ? colors.active : colors.activeUnfocused, m_alpha);
    for (const Label& label : m_labels) {
      if (label.buffer != nullptr) {
        wlr_scene_buffer_set_opacity(label.buffer, m_alpha);
      }
    }
  }

  void TabBar::update(const TabBarModel& model, bool focused, int x, int y, int width, float scale) {
    const auto& tabs = config().layout.tabs;
    const auto& colors = config().colors.tabBar;
    const int height = tabs.barHeight;
    const int radius = std::min(config().appearance.cornerRadius, height / 2);
    const size_t count = model.titles.size();
    width = std::max(1, width);
    m_focused = focused;

    wlr_scene_node_set_position(&m_tree->node, x, y);
    wlr_scene_rect_set_size(m_background, width, height);
    wlr_scene_rect_set_corner_radius(m_background, radius);

    const TabSlot activeSlot = tabSlot(width, count, model.active);
    const bool hasFill = activeSlot.width > 2 * kFillInset && height > 2 * kFillInset;
    wlr_scene_node_set_enabled(&m_activeFill->node, hasFill);
    if (hasFill) {
      wlr_scene_node_set_position(&m_activeFill->node, activeSlot.x + kFillInset, kFillInset);
      wlr_scene_rect_set_size(m_activeFill, activeSlot.width - 2 * kFillInset, height - 2 * kFillInset);
      wlr_scene_rect_set_corner_radius(m_activeFill, nestedRadius(radius, kFillInset));
    }

    while (m_labels.size() > count) {
      if (m_labels.back().buffer != nullptr) {
        wlr_scene_node_destroy(&m_labels.back().buffer->node);
      }
      m_labels.pop_back();
    }
    m_labels.resize(count);

    const double deviceScale = std::max(1.0, std::ceil(static_cast<double>(scale)));
    const std::string font = std::format("sans {}", tabs.fontSize);
    for (size_t index = 0; index < count; ++index) {
      Label& label = m_labels[index];
      const TabSlot slot = tabSlot(width, count, index);
      const int maxWidth = slot.width - 2 * kLabelPadding;
      const std::array<float, 4>& textColor = index != model.active ? colors.text
          : focused                                                 ? colors.activeText
                                                                    : colors.activeUnfocusedText;
      const std::string markup =
          std::format("<span foreground='{}'>{}</span>", rgbaHex(textColor), escapeMarkup(model.titles[index]));
      const std::string key = std::format("{}\x1f{}\x1f{}\x1f{}", markup, maxWidth, font, deviceScale);

      if (maxWidth <= 0 || model.titles[index].empty()) {
        if (label.buffer != nullptr) {
          wlr_scene_node_destroy(&label.buffer->node);
          label = {};
        }
        continue;
      }
      if (label.key != key) {
        const TextBufferResult rendered = renderTextBuffer({
            .markup = markup,
            .font = font,
            .maxWidth = maxWidth,
            .padding = 0,
            .scale = deviceScale,
            .ellipsize = true,
        });
        if (rendered.buffer == nullptr) {
          continue;
        }
        if (label.buffer == nullptr) {
          label.buffer = wlr_scene_buffer_create(m_tree, rendered.buffer);
          if (label.buffer != nullptr) {
            label.buffer->point_accepts_input = rejectInput;
          }
        } else {
          wlr_scene_buffer_set_buffer(label.buffer, rendered.buffer);
        }
        wlr_buffer_drop(rendered.buffer); // the scene holds the lock
        if (label.buffer == nullptr) {
          label = {};
          continue;
        }
        label.key = key;
        label.width = rendered.logicalWidth;
        label.height = rendered.logicalHeight;
        wlr_scene_buffer_set_dest_size(label.buffer, label.width, label.height);
      }
      wlr_scene_node_set_position(
          &label.buffer->node, slot.x + std::max(0, (slot.width - label.width) / 2),
          std::max(0, (height - label.height) / 2)
      );
    }
    applyColors();
  }

} // namespace umbriel
