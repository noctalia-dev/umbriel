#include "scene/tab_label_cache.h"

#include "scene/color.h"
#include "scene/text_buffer.h"

extern "C" {
#include <wlr/types/wlr_buffer.h>
}

#include <algorithm>
#include <format>
#include <iterator>
#include <utility>

namespace umbriel {

  TabLabelCache::TabLabelCache(size_t capacity) : m_capacity(std::max<size_t>(1, capacity)) {}

  TabLabelCache::~TabLabelCache() {
    for (Entry& entry : m_entries) {
      wlr_buffer_drop(entry.label.buffer);
    }
  }

  const TabLabel* TabLabelCache::label(const TabLabelRequest& request) {
    if (request.text.empty() || request.maxWidth <= 0) {
      return nullptr;
    }
    const std::string markup =
        std::format("<span foreground='{}'>{}</span>", rgbaHex(request.color), escapeMarkup(request.text));
    std::string key = std::format("{}\x1f{}\x1f{}\x1f{}", markup, request.font, request.maxWidth, request.scale);
    if (const auto found = m_index.find(key); found != m_index.end()) {
      m_entries.splice(m_entries.begin(), m_entries, found->second);
      return &found->second->label;
    }

    const TextBufferResult rendered = renderTextBuffer({
        .markup = markup,
        .font = std::string(request.font),
        .maxWidth = request.maxWidth,
        .padding = 0,
        .scale = request.scale,
        .ellipsize = true,
    });
    if (rendered.buffer == nullptr) {
      return nullptr;
    }
    m_entries.push_front({
        .key = key,
        .label = {.buffer = rendered.buffer, .width = rendered.logicalWidth, .height = rendered.logicalHeight},
    });
    m_index.emplace(std::move(key), m_entries.begin());
    trim();
    return &m_entries.front().label;
  }

  void TabLabelCache::trim() {
    const auto bytes = [](const Entry& entry) {
      return static_cast<size_t>(entry.label.buffer->width) * static_cast<size_t>(entry.label.buffer->height) * 4;
    };
    size_t unusedBytes = 0;
    for (auto it = std::next(m_entries.begin()); it != m_entries.end(); ++it) {
      if (it->label.buffer->n_locks == 0) {
        unusedBytes += bytes(*it);
      }
    }
    for (auto it = m_entries.end();
         it != std::next(m_entries.begin()) && (unusedBytes > kUnusedBudgetBytes || m_entries.size() > m_capacity);) {
      --it;
      if (it->label.buffer->n_locks != 0) {
        continue;
      }
      unusedBytes -= bytes(*it);
      wlr_buffer_drop(it->label.buffer);
      m_index.erase(it->key);
      it = m_entries.erase(it);
    }
  }

  std::shared_ptr<TabLabelCache> TabLabelCache::shared() {
    static std::weak_ptr<TabLabelCache> instance;
    std::shared_ptr<TabLabelCache> cache = instance.lock();
    if (cache == nullptr) {
      cache = std::make_shared<TabLabelCache>();
      instance = cache;
    }
    return cache;
  }

} // namespace umbriel
