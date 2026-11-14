#pragma once

#include "scene/tab_bar_geometry.h"
#include "scene/tab_bar_style.h"
#include "view/chrome_attachment.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <wlr/util/box.h>
}

struct wlr_buffer;
struct wlr_scene_buffer;
struct wlr_scene_rect;
struct wlr_scene_tree;

namespace umbriel {

  class TabLabelCache;

  struct TabBarEntry {
    std::string title;
    bool urgent = false;
    bool operator==(const TabBarEntry&) const = default;
  };

  // What a tabbed column's bar shows: one entry per tab in column order, and which of them is on show.
  struct TabBarModel {
    std::vector<TabBarEntry> tabs;
    size_t active = 0;
    // The tabs the bar shows: every one, or a scrolled run of them when there are more than it has slots for.
    TabStrip strip;
    // Space between the bar and the border of the window it sits over, the bar gap the column reserved with it.
    int gap = 0;
    bool operator==(const TabBarModel&) const = default;
  };

  // Where a window dropped on a bar joins its tabs, and the marker between two slots that shows it, in the bar's own
  // coordinates.
  struct TabBarDrop {
    size_t row = 0;
    wlr_box marker{};
  };

  // The strip along one edge of a tabbed column: a rounded slot per tab, coloured by its state, with the tab's title
  // in it unless the style draws slots alone. On the top or bottom edge the slots sit side by side; on the left or
  // right they stack, their titles still reading across. While it shows only some of its tabs, a button at each end
  // cycles to the previous or next tab. The window on show carries it as its chrome attachment, so the bar follows
  // every move, slide, fade, and hide that window makes. A title renders once into the shared label cache, so repeating
  // an update, or moving the bar to another window, is cheap.
  class TabBar final : public ViewChromeAttachment {
  public:
    TabBar(wlr_scene_tree* parent, std::shared_ptr<TabLabelCache> labels);
    ~TabBar() override;

    void setModel(TabBarModel model);
    const TabBarModel& model() const { return m_model; }
    const TabBarStyle& style() const { return m_style; }
    // The drawn bar in layout coordinates; empty while nothing is drawn.
    wlr_box layoutBox() const;
    // The tabs the bar shows, its model's strip kept inside its tabs.
    TabStrip visibleStrip() const;
    // What lies at a point in the bar's own coordinates: a tab, a cycle button, or nothing.
    TabBarPoint pointAt(double localX, double localY) const;
    // Where a window dropped at a point in the bar's own coordinates joins the tabs.
    TabBarDrop dropAt(double localX, double localY) const;

    void layout(const ViewChromeGeometry& geometry) override;
    void setFocused(bool focused) override;
    void setAlpha(float alpha) override;
    void reloadConfig() override;
    // A preview's copy of this bar: the same tabs, drawn at the preview's zoom.
    std::unique_ptr<ViewChromeAttachment> makePreview(wlr_scene_tree* parent) const override;
    void syncPreview(ViewChromeAttachment& preview) const override;

  private:
    // A tab slot or a cycle button. Its text, when it has some, is created after its fill and so stacks above it.
    struct Slot {
      wlr_scene_rect* fill = nullptr;
      wlr_scene_buffer* label = nullptr;
    };

    bool drawn() const;
    // Whether the bar runs across the column, top or bottom, rather than down it.
    bool across() const { return tabBarAcross(m_style.position); }
    // The bar relative to the frame of the window carrying it.
    wlr_box localBox() const;
    // The bar's extent along its slots, and across them.
    int length() const;
    int thickness() const;
    // The part of the bar from `start` along its slots, `extent` long, the bar's whole thickness across.
    wlr_box span(int start, int extent) const;
    // Logical bar units scaled to the size the bar is drawn at: unchanged at full size, smaller on a preview.
    // Layout stays in logical units, so a preview asks the label cache for the titles the live bar already rendered.
    int zoomed(int value) const;
    wlr_box zoomed(const wlr_box& box) const;
    void redraw();
    // Tab `index` drawn in `rect`.
    void drawSlot(Slot& slot, size_t index, const wlr_box& rect);
    // A button in `rect` that cycles to the previous or next tab.
    void drawButton(Slot& button, bool shown, const wlr_box& rect, std::string_view glyph);
    void drawFill(Slot& slot, const wlr_box& rect, const std::array<float, 4>& color, float alpha);
    // `text` cut to the width of `rect` less `padding` at each end, placed in it by `align` and centred in its height;
    // no text drops what was there.
    void drawText(
        Slot& slot, std::string_view text, const std::array<float, 4>& color, const wlr_box& rect, int padding,
        TitleAlign align, float alpha
    );
    // The slot fill and title colours of tab `index`, by its state.
    const std::array<float, 4>& fillColor(size_t index) const;
    const std::array<float, 4>& textColor(size_t index) const;
    void dropLabel(Slot& slot);
    void destroySlot(Slot& slot);

    wlr_scene_tree* m_tree = nullptr;
    std::shared_ptr<TabLabelCache> m_labels;
    TabBarStyle m_style;
    TabBarModel m_model;
    ViewChromeGeometry m_geometry;
    std::vector<Slot> m_slots;
    Slot m_previousButton;
    Slot m_nextButton;
    bool m_focused = false;
    float m_alpha = 1.0F;
  };

} // namespace umbriel
