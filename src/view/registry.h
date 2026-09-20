#pragma once

#include <memory>
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

    [[nodiscard]] std::span<const std::unique_ptr<View>> all() const { return m_views; }
    [[nodiscard]] bool empty() const { return m_views.empty(); }

    // Move `view` to the front. No-op when it is absent or already there.
    void promote(View* view);

    // The mapped modal dialogs, so finding what blocks a window does not scan every view.
    void setModalDialog(View* view, bool modal);
    [[nodiscard]] std::span<View* const> modalDialogs() const { return m_modalDialogs; }

  private:
    std::vector<std::unique_ptr<View>> m_views;
    std::vector<View*> m_modalDialogs;
  };

} // namespace umbriel
