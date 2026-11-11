#include "scene/tab_bar.h"

#include "scene/color.h"
#include "scene/tab_label_cache.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace umbriel {

  namespace {

    // Thickness of the marker a drop onto the bar draws between two slots.
    constexpr int kDropMarkerThickness = 6;

    // Clicks on the bar are the compositor's (TabPresenter::tabAt), never a window's.
    bool rejectInput(wlr_scene_buffer* /*buffer*/, double* /*x*/, double* /*y*/) { return false; }

    void setRectColor(wlr_scene_rect* rect, const std::array<float, 4>& color, float alpha) {
      float premultipliedColor[4]{};
      premultiplied(premultipliedColor, color, alpha);
      wlr_scene_rect_set_color(rect, premultipliedColor);
    }

  } // namespace

  TabBar::TabBar(wlr_scene_tree* parent, std::shared_ptr<TabLabelCache> labels)
      : m_tree(wlr_scene_tree_create(parent)), m_labels(std::move(labels)), m_style(resolveTabBarStyle(config())) {
    wlr_scene_node_set_enabled(&m_tree->node, false);
  }

  TabBar::~TabBar() {
    if (m_tree != nullptr) {
      wlr_scene_node_destroy(&m_tree->node);
    }
  }

  void TabBar::setModel(TabBarModel model) {
    if (m_model == model) {
      return;
    }
    m_model = std::move(model);
    redraw();
  }

  void TabBar::layout(const ViewChromeGeometry& geometry) {
    if (m_geometry == geometry) {
      return;
    }
    m_geometry = geometry;
    redraw();
  }

  void TabBar::setFocused(bool focused) {
    if (m_focused == focused) {
      return;
    }
    m_focused = focused;
    redraw();
  }

  void TabBar::setAlpha(float alpha) {
    alpha = std::clamp(alpha, 0.0F, 1.0F);
    if (m_alpha == alpha) {
      return;
    }
    m_alpha = alpha;
    redraw();
  }

  void TabBar::reloadConfig() {
    TabBarStyle style = resolveTabBarStyle(config());
    if (m_style == style) {
      return;
    }
    m_style = std::move(style);
    redraw();
  }

  std::unique_ptr<ViewChromeAttachment> TabBar::makePreview(wlr_scene_tree* parent) const {
    auto preview = std::make_unique<TabBar>(parent, m_labels);
    syncPreview(*preview);
    return preview;
  }

  void TabBar::syncPreview(ViewChromeAttachment& preview) const {
    auto* bar = dynamic_cast<TabBar*>(&preview);
    if (bar == nullptr) {
      return;
    }
    bar->reloadConfig();
    bar->setFocused(m_focused);
    bar->setModel(m_model);
  }

  int TabBar::zoomed(int value) const {
    return m_geometry.zoom == 1.0 ? value : static_cast<int>(std::lround(value * m_geometry.zoom));
  }

  wlr_box TabBar::zoomed(const wlr_box& box) const {
    // Both edges scale, so slots that met at full size still meet.
    const int x = zoomed(box.x);
    const int y = zoomed(box.y);
    return {.x = x, .y = y, .width = zoomed(box.x + box.width) - x, .height = zoomed(box.y + box.height) - y};
  }

  bool TabBar::drawn() const {
    return !m_model.tabs.empty()
        && !m_geometry.suppressed
        && m_geometry.contentWidth > 0
        && m_geometry.contentHeight > 0
        && m_style.height > 0;
  }

  wlr_box TabBar::localBox() const {
    // The column reserved the bar and one gap beside its tabs' borders, so the bar spans the border's outer extent
    // along that edge and ends one gap short of it.
    const int inset = m_geometry.borderInset;
    const int outerX = m_geometry.contentX - inset;
    const int outerY = m_geometry.contentY - inset;
    const int outerWidth = m_geometry.contentWidth + 2 * inset;
    const int outerHeight = m_geometry.contentHeight + 2 * inset;
    const int bar = m_style.height;
    const int gap = m_model.gap;
    switch (m_style.position) {
    case TabBarPosition::Top:
      return {.x = outerX, .y = outerY - gap - bar, .width = outerWidth, .height = bar};
    case TabBarPosition::Bottom:
      return {.x = outerX, .y = outerY + outerHeight + gap, .width = outerWidth, .height = bar};
    case TabBarPosition::Left:
      return {.x = outerX - gap - bar, .y = outerY, .width = bar, .height = outerHeight};
    case TabBarPosition::Right:
      return {.x = outerX + outerWidth + gap, .y = outerY, .width = bar, .height = outerHeight};
    }
    return {};
  }

  int TabBar::length() const {
    const wlr_box bar = localBox();
    return across() ? bar.width : bar.height;
  }

  int TabBar::thickness() const {
    const wlr_box bar = localBox();
    return across() ? bar.height : bar.width;
  }

  wlr_box TabBar::span(int start, int extent) const {
    const int depth = thickness();
    if (across()) {
      return {.x = start, .y = 0, .width = extent, .height = depth};
    }
    return {.x = 0, .y = start, .width = depth, .height = extent};
  }

  wlr_box TabBar::layoutBox() const {
    int frameX = 0;
    int frameY = 0;
    if (!drawn() || m_tree->node.parent == nullptr) {
      return {};
    }
    // The position holds while the window tree is hidden under the overview.
    wlr_scene_node_coords(&m_tree->node.parent->node, &frameX, &frameY);
    const wlr_box local = localBox();
    return {.x = frameX + local.x, .y = frameY + local.y, .width = local.width, .height = local.height};
  }

  void TabBar::redraw() {
    if (!drawn()) {
      wlr_scene_node_set_enabled(&m_tree->node, false);
      return;
    }
    const wlr_box bar = localBox();
    wlr_scene_node_set_position(&m_tree->node, zoomed(bar.x), zoomed(bar.y));
    // Only the slots on show exist, so a column of many tabs draws, and renders titles for, no more than its strip.
    const TabStrip strip = visibleStrip();
    const size_t tabs = m_model.tabs.size();
    const int barLength = length();
    const TabBarParts parts = tabBarParts(barLength, thickness(), m_style.tabGap, strip.count < tabs);
    while (m_slots.size() > strip.count) {
      destroySlot(m_slots.back());
      m_slots.pop_back();
    }
    m_slots.resize(strip.count);
    for (size_t position = 0; position < m_slots.size(); ++position) {
      const TabSlot slot = tabSlot(parts.slotsWidth, strip.count, position, m_style.tabGap);
      drawSlot(m_slots[position], strip.first + position, span(parts.slotsX + slot.x, slot.width));
    }
    // The buttons cycle through every tab, wrapping at the ends, so neither ever has nothing to do.
    const bool cycles = parts.buttonWidth > 0;
    drawButton(m_previousButton, cycles, span(0, parts.buttonWidth), across() ? "‹" : "▴");
    drawButton(m_nextButton, cycles, span(barLength - parts.buttonWidth, parts.buttonWidth), across() ? "›" : "▾");
    wlr_scene_node_set_enabled(&m_tree->node, true);
  }

  TabStrip TabBar::visibleStrip() const {
    const size_t tabs = m_model.tabs.size();
    const size_t first = std::min(m_model.strip.first, tabs);
    const size_t count = m_model.strip.count == 0 ? tabs - first : std::min(m_model.strip.count, tabs - first);
    return {.first = first, .count = count};
  }

  TabBarPoint TabBar::pointAt(double localX, double localY) const {
    if (!drawn()) {
      return {};
    }
    // Across its slots the bar is the line the geometry measures; across its thickness, any point inside counts.
    const double along = across() ? localX : localY;
    const double through = across() ? localY : localX;
    if (through < 0.0 || through >= static_cast<double>(thickness())) {
      return {};
    }
    return tabBarPointAt(length(), thickness(), m_style.tabGap, visibleStrip(), m_model.tabs.size(), along);
  }

  TabBarDrop TabBar::dropAt(double localX, double localY) const {
    const int barLength = length();
    const TabDropPoint point = tabBarDropAt(
        barLength, thickness(), m_style.tabGap, visibleStrip(), m_model.tabs.size(), across() ? localX : localY
    );
    const int start =
        std::clamp(point.boundary - kDropMarkerThickness / 2, 0, std::max(0, barLength - kDropMarkerThickness));
    return {.row = point.row, .marker = span(start, kDropMarkerThickness)};
  }

  const std::array<float, 4>& TabBar::fillColor(size_t index) const {
    const Config::Colors::TabBar& colors = m_style.colors;
    if (index == m_model.active) {
      return m_focused ? colors.active : colors.activeUnfocused;
    }
    return m_model.tabs[index].urgent ? colors.urgent : colors.background;
  }

  const std::array<float, 4>& TabBar::textColor(size_t index) const {
    const Config::Colors::TabBar& colors = m_style.colors;
    if (index == m_model.active) {
      return m_focused ? colors.activeText : colors.activeUnfocusedText;
    }
    return m_model.tabs[index].urgent ? colors.urgentText : colors.text;
  }

  void TabBar::drawSlot(Slot& slot, size_t index, const wlr_box& rect) {
    drawFill(slot, rect, fillColor(index), m_alpha);
    const std::string_view title = m_style.look == TabBarLook::Titles ? m_model.tabs[index].title : std::string_view{};
    drawText(slot, title, textColor(index), rect, m_style.padding, m_style.titleAlign, m_alpha);
  }

  void TabBar::drawButton(Slot& button, bool shown, const wlr_box& rect, std::string_view glyph) {
    if (!shown) {
      destroySlot(button);
      return;
    }
    drawFill(button, rect, m_style.colors.background, m_alpha);
    const std::string_view text = m_style.look == TabBarLook::Titles ? glyph : std::string_view{};
    drawText(button, text, m_style.colors.text, rect, 0, TitleAlign::Center, m_alpha);
  }

  void TabBar::drawFill(Slot& slot, const wlr_box& rect, const std::array<float, 4>& color, float alpha) {
    if (slot.fill == nullptr) {
      const float clear[4]{};
      slot.fill = wlr_scene_rect_create(m_tree, 1, 1, clear);
      slot.fill->accepts_input = false;
    }
    const wlr_box drawn = zoomed(rect);
    const int width = std::max(1, drawn.width);
    const int height = std::max(1, drawn.height);
    wlr_scene_node_set_position(&slot.fill->node, drawn.x, drawn.y);
    wlr_scene_rect_set_size(slot.fill, width, height);
    wlr_scene_rect_set_corner_radius(slot.fill, std::min({zoomed(m_style.cornerRadius), height / 2, width / 2}));
    setRectColor(slot.fill, color, alpha);
  }

  void TabBar::drawText(
      Slot& slot, std::string_view text, const std::array<float, 4>& color, const wlr_box& rect, int padding,
      TitleAlign align, float alpha
  ) {
    const TabLabel* label = m_labels->label({
        .text = text,
        .color = color,
        .font = m_style.font,
        .maxWidth = rect.width - 2 * padding,
        .scale = std::max(1.0, std::ceil(static_cast<double>(m_geometry.scale))),
    });
    if (label == nullptr) {
      dropLabel(slot);
      return;
    }
    if (slot.label == nullptr) {
      slot.label = wlr_scene_buffer_create(m_tree, label->buffer);
      if (slot.label == nullptr) {
        return;
      }
      slot.label->point_accepts_input = rejectInput;
    } else {
      wlr_scene_buffer_set_buffer(slot.label, label->buffer);
    }
    wlr_scene_buffer_set_dest_size(slot.label, std::max(1, zoomed(label->width)), std::max(1, zoomed(label->height)));
    wlr_scene_buffer_set_opacity(slot.label, alpha);
    int x = rect.x + padding;
    switch (align) {
    case TitleAlign::Left:
      break;
    case TitleAlign::Center:
      x = rect.x + std::max(0, (rect.width - label->width) / 2);
      break;
    case TitleAlign::Right:
      x = rect.x + std::max(padding, rect.width - padding - label->width);
      break;
    }
    wlr_scene_node_set_position(
        &slot.label->node, zoomed(x), zoomed(rect.y + std::max(0, (rect.height - label->height) / 2))
    );
  }

  void TabBar::dropLabel(Slot& slot) {
    if (slot.label != nullptr) {
      wlr_scene_node_destroy(&slot.label->node);
      slot.label = nullptr;
    }
  }

  void TabBar::destroySlot(Slot& slot) {
    dropLabel(slot);
    if (slot.fill != nullptr) {
      wlr_scene_node_destroy(&slot.fill->node);
      slot.fill = nullptr;
    }
  }

} // namespace umbriel
