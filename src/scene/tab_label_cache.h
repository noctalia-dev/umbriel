#pragma once

#include <array>
#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

struct wlr_buffer;

namespace umbriel {

  // What a tab title renders from.
  struct TabLabelRequest {
    std::string_view text;
    std::array<float, 4> color{};
    std::string_view font;
    // Logical width the title is cut to with an ellipsis.
    int maxWidth = 0;
    // Device scale it is rendered at.
    double scale = 1.0;
  };

  struct TabLabel {
    wlr_buffer* buffer = nullptr;
    int width = 0;
    int height = 0;
  };

  // Rendered tab titles, kept by what they render from and dropped least recently used first. Every tab bar shares
  // one, so a title renders once however many bars show it and however often focus moves the bar between windows.
  class TabLabelCache {
  public:
    explicit TabLabelCache(size_t capacity = kDefaultCapacity);
    ~TabLabelCache();
    TabLabelCache(const TabLabelCache&) = delete;
    TabLabelCache& operator=(const TabLabelCache&) = delete;

    // The label for `request`, rendering it on a miss; null when there is nothing to draw. The buffer stays valid until
    // the cache drops it, so a scene buffer showing it takes its own lock.
    const TabLabel* label(const TabLabelRequest& request);
    size_t size() const { return m_entries.size(); }

    // The cache every live tab bar shares. It lives as long as one of them holds it.
    static std::shared_ptr<TabLabelCache> shared();

    static constexpr size_t kDefaultCapacity = 256;

  private:
    struct Entry {
      std::string key;
      TabLabel label;
    };

    size_t m_capacity;
    // Most recently used first.
    std::list<Entry> m_entries;
    std::unordered_map<std::string, std::list<Entry>::iterator> m_index;
  };

} // namespace umbriel
