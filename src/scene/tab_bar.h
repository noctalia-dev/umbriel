#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct wlr_scene_buffer;
struct wlr_scene_rect;
struct wlr_scene_tree;

namespace umbriel {

  // What a tabbed column's bar shows: one title per tab, in column order, and which of them is on show.
  struct TabBarModel {
    std::vector<std::string> titles;
    size_t active = 0;
    bool operator==(const TabBarModel&) const = default;
  };

  // The strip across the top of a tabbed column: a rounded background, a fill behind the active tab, and each tab's
  // title centred in its slot (tab_bar_geometry.h). The shown tab's View owns it under its frame, so the bar follows
  // every position, slide, and visibility change that view makes. Colours and sizes are read from the config directly,
  // as the other scene classes do.
  class TabBar {
  public:
    explicit TabBar(wlr_scene_tree* parent);
    ~TabBar();
    TabBar(const TabBar&) = delete;
    TabBar& operator=(const TabBar&) = delete;

    // Lay the bar out with its top-left corner at (x, y) in the parent's coordinates, `width` logical pixels wide.
    // `focused` picks the focused colours for the active tab. A label renders again only when its text, width,
    // colour, font or scale changed, so repeating an update is cheap.
    void update(const TabBarModel& model, bool focused, int x, int y, int width, float scale);
    void setEnabled(bool enabled);
    // Multiplies every colour, so the bar fades with its view.
    void setAlpha(float alpha);
    // Drop every rendered label, so the next update draws them from the current config.
    void invalidate();

  private:
    struct Label {
      wlr_scene_buffer* buffer = nullptr;
      std::string key;
      int width = 0;
      int height = 0;
    };

    void applyColors();

    wlr_scene_tree* m_tree = nullptr;
    wlr_scene_rect* m_background = nullptr;
    wlr_scene_rect* m_activeFill = nullptr;
    std::vector<Label> m_labels;
    bool m_focused = false;
    float m_alpha = 1.0F;
  };

} // namespace umbriel
