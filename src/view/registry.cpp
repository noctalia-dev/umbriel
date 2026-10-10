#include "view/registry.h"

#include "view/view.h"

#include <algorithm>

namespace umbriel {

  // Out of line so the header needs only a forward declaration of View.
  ViewRegistry::ViewRegistry() = default;
  ViewRegistry::~ViewRegistry() = default;

  View& ViewRegistry::add(std::unique_ptr<View> view) {
    m_views.push_back(std::move(view));
    return *m_views.back();
  }

  void ViewRegistry::remove(View* view) {
    std::erase_if(m_views, [view](const std::unique_ptr<View>& entry) { return entry.get() == view; });
  }

  void ViewRegistry::clear() { m_views.clear(); }

  void ViewRegistry::promote(View* view) {
    auto it = std::ranges::find_if(m_views, [view](const std::unique_ptr<View>& entry) { return entry.get() == view; });
    if (it == m_views.end() || it == m_views.begin()) {
      return;
    }
    auto entry = std::move(*it);
    m_views.erase(it);
    m_views.insert(m_views.begin(), std::move(entry));
  }

} // namespace umbriel
