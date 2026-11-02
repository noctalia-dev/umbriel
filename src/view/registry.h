#pragma once

#include <memory>
#include <ranges>
#include <span>
#include <vector>

namespace umbriel {

  class View;

  // Owns every View, ordered most-recently-focused first. Scans in this order
  // pick focus fallbacks: the most recently focused candidate comes first.
  class ViewRegistry {
  public:
    ViewRegistry();
    ~ViewRegistry();

    ViewRegistry(const ViewRegistry&) = delete;
    ViewRegistry& operator=(const ViewRegistry&) = delete;

    // New windows go to the back: an unfocused window is by definition the least
    // recently focused one. Focusing it promotes it.
    View& add(std::unique_ptr<View> view);
    void remove(View* view);
    void clear();

    std::span<const std::unique_ptr<View>> all() const { return m_views; }
    // The views that own a placement, in the same order: an attached dialog lives inside its parent, so a scan picking
    // a window to focus or act on skips it.
    auto placementRoots() const { return std::views::filter(m_views, &ViewRegistry::isPlacementRoot); }
    bool empty() const { return m_views.empty(); }

    // Move `view` to the front. No-op when it is absent or already there.
    void promote(View* view);

  private:
    static bool isPlacementRoot(const std::unique_ptr<View>& view);

    std::vector<std::unique_ptr<View>> m_views;
  };

} // namespace umbriel
